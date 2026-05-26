/**
 * @file sitl_manager.cpp
 * @brief SITL仿真管理器实现 —— FlyteOS闭环仿真核心
 *
 * 数据流：
 *   RC输入 → FlyteBus(MsgRcInput)
 *   传感器注入 → FlyteBus(MsgImuRaw, MsgGpsRaw, MsgBaro)
 *   姿态估计 ← FlyteBus(MsgImuRaw, MsgGpsRaw)  → FlyteBus(MsgAttitudeEstimate)
 *   载荷称重 → FlyteBus(MsgPayloadState) → 动态调整B/W目标
 *   浮力控制 → FlyteBus(MsgGasCellState)
 *   温度预测 ← FlyteBus(MsgGasCellState) → FlyteBus(MsgThermalPrediction)
 *   飞控三环 ← FlyteBus(MsgAttitudeEstimate, MsgGasCellState) → FlyteBus(MsgActuatorOutput)
 *   涡轮模型 ← FlyteBus(MsgActuatorOutput) → FlyteBus(MsgTurbineState)
 *   太阳能膜 ← FlyteBus(MsgEnvironment) → FlyteBus(MsgSolarState)
 *   物理推进 ← FlyteBus(MsgTurbineState, MsgGasCellState, MsgPayloadState) → 更新physics_
 *   安全检查 ← FlyteBus(MsgFlightState, MsgThermalPrediction, MsgPayloadState)
 */

#include "sitl_manager.hpp"
#include <algorithm>
#include <cstdlib>

namespace FlyteOS::Sim {

// ════════════════════════════════════════════════════════════════
//  构造/初始化
// ════════════════════════════════════════════════════════════════

SITLManager::SITLManager(Config cfg) : cfg_(cfg) {}

Error SITLManager::init() {
    // 1. 清空FlyteBus
    Bus::FlyteBus::reset();

    // 2. 初始化物理状态
    physics_ = {};
    physics_.pos_d = -cfg_.launch_altitude_m;

    // 3. 初始化气囊
    gas_cell_ = Power::GasCell(cfg_.gas_cell_cfg);
    gas_cell_.initialize(0.95, cfg_.aircraft_mass_kg);

    // 4. 初始化涡轮模型
    turbine_ = Power::TurbineModel();
    turbine_.initialize(100.0f);  // 电池满电

    // 5. 初始化太阳能膜
    solar_membrane_ = Power::SolarMembrane();

    // 6. 初始化温度预测
    thermal_predictor_ = Power::ThermalPredictor();

    // 7. 初始化载荷称重
    payload_scale_ = Power::PayloadScale();
    payload_scale_.initialize(cfg_.aircraft_mass_kg);

    // 8. 初始化姿态估计
    attitude_.reset();

    // 9. 重置飞控
    pos_ctrl_.reset();
    vel_ctrl_.reset();
    att_ctrl_.reset();

    // 10. 发布初始状态
    Bus::FlyteBus::publish<Bus::MsgFlightState>(
        Bus::MsgFlightState{FlightState::DISARMED, now_us()}
    );
    Bus::FlyteBus::publish<Bus::MsgEnvironment>(
        Bus::MsgEnvironment{
            cfg_.launch_altitude_m, 288.15, 101325.0,
            cfg_.wind_n_ms, cfg_.wind_e_ms, 0,
            cfg_.solar_elevation_rad, cfg_.solar_irradiance_wm2,
            now_us()
        }
    );

    sim_time_s_ = 0;
    step_count_ = 0;
    running_ = true;

    printf("[SITL] Initialized: mass=%.1fkg, gas_cell=%.0fm³, B/W=0.95, turbine+payload+solar+thermal online\n",
           cfg_.aircraft_mass_kg, cfg_.gas_cell_cfg.max_volume_m3);
    return Error::ok();
}

// ════════════════════════════════════════════════════════════════
//  主步进
// ════════════════════════════════════════════════════════════════

Error SITLManager::step() {
    if (!running_) {
        return Error{ErrorKind::ControlLoopFailure, 0, "SITL not running", __FILE__, now_us()};
    }

    f32 dt = cfg_.sim_dt;
    step_count_++;
    sim_time_s_ += dt;

    // 1. 环境更新（50Hz）
    if (step_count_ % 8 == 0) {
        updateEnvironment();
    }

    // 2. 传感器注入（400Hz）
    injectSensors();

    // 3. 姿态估计（200Hz）
    if (step_count_ % 2 == 0) {
        runAttitudeEstimation();
    }

    // 4. 载荷称重（50Hz）—— 在浮力控制之前，提供动态质量
    if (step_count_ % 8 == 0) {
        runPayloadScale();
    }

    // 5. 浮力控制（50Hz）—— 使用载荷称重的动态质量
    if (step_count_ % 8 == 0) {
        runBuoyancyControl();
    }

    // 6. 飞行控制（400Hz）—— 输出执行器指令
    runFlightControl();

    // 7. 涡轮推力模型（400Hz）—— 执行器指令→真实推力
    runTurbineControl();

    // 8. 太阳能发电（10Hz）
    if (step_count_ % 40 == 0) {
        runSolarEnergy();
    }

    // 9. 温度预测（10Hz）
    if (step_count_ % 40 == 0) {
        runThermalPrediction();
    }

    // 10. 物理推进 —— 使用涡轮真实推力
    applyForces(dt);

    // 11. 安全检查（10Hz）
    if (step_count_ % 40 == 0) {
        runSafetyCheck();
    }

    return Error::ok();
}

// ════════════════════════════════════════════════════════════════
//  传感器注入
// ════════════════════════════════════════════════════════════════

Bus::MsgImuRaw SITLManager::generateIMU() {
    Bus::MsgImuRaw imu;

    // 角速度（从姿态变化率推导，简化模型）
    // 在真实仿真中应从物理引擎角动量推导
    imu.gyro[0] = 0;  // roll rate
    imu.gyro[1] = 0;  // pitch rate
    imu.gyro[2] = 0;  // yaw rate

    // 线加速度（从推力+浮力-重力推导）
    f32 g = 9.81f;
    f32 thrust_n = 0;
    auto sub_act = Bus::FlyteBus::subscribe<Bus::MsgActuatorOutput>();
    if (sub_act.has()) {
        thrust_n = sub_act.get()->thrust_n;
    }

    f64 buoyancy_n = 0;
    auto sub_gas = Bus::FlyteBus::subscribe<Bus::MsgGasCellState>();
    if (sub_gas.has()) {
        buoyancy_n = sub_gas.get()->buoyancy_n;
    }

    // 动态质量（用于IMU加速度计算）
    f32 mass = cfg_.aircraft_mass_kg;
    auto sub_payload_imu = Bus::FlyteBus::subscribe<Bus::MsgPayloadState>();
    if (sub_payload_imu.has() && sub_payload_imu.get()->total_mass_kg > 1.0f) {
        mass = sub_payload_imu.get()->total_mass_kg;
    }

    f32 weight_n = mass * g;
    f32 net_vertical_n = thrust_n + static_cast<f32>(buoyancy_n) - weight_n;

    // 机体坐标系加速度（简化：假设小角度）
    imu.accel[0] = 0;  // 前向
    imu.accel[1] = 0;  // 右向
    imu.accel[2] = -net_vertical_n / mass;  // 垂直

    // 磁力计
    imu.mag[0] = 20.0f;
    imu.mag[1] = 0.0f;
    imu.mag[2] = -40.0f;

    // 噪声
    addNoise(imu.gyro, 3, cfg_.gyro_noise_rads);
    addNoise(imu.accel, 3, cfg_.accel_noise_ms2);

    imu.ts = now_us();
    return imu;
}

Bus::MsgGpsRaw SITLManager::generateGPS() {
    Bus::MsgGpsRaw gps;

    // NED → 经纬度（参考点已脱敏）
    f64 ref_lat = 28.0;
    f64 ref_lon = 112.0;
    gps.lat = ref_lat + physics_.pos_n / 111320.0;
    gps.lon = ref_lon + physics_.pos_e / (111320.0 * cos(ref_lat * M_PI / 180.0));
    gps.alt_msl = -physics_.pos_d;

    gps.vel_ned[0] = physics_.vel_n;
    gps.vel_ned[1] = physics_.vel_e;
    gps.vel_ned[2] = physics_.vel_d;

    gps.hdop = 0.8f + (rand() % 100) / 100.0f * 0.4f;
    gps.fix = 5;  // RTK Fixed
    gps.ts = now_us();
    return gps;
}

Bus::MsgBaro SITLManager::generateBaro() {
    Bus::MsgBaro baro;
    baro.altitude_m = -physics_.pos_d + ((rand() % 1000) - 500) / 1000.0f * cfg_.baro_noise_m;
    baro.pressure_pa = 101325.0f * powf(1.0f - 0.0065f * baro.altitude_m / 288.15f, 5.255f);
    baro.temperature_k = 288.15f - 0.0065f * baro.altitude_m;
    baro.ts = now_us();
    return baro;
}

void SITLManager::injectSensors() {
    Bus::FlyteBus::publish<Bus::MsgImuRaw>(generateIMU());
    Bus::FlyteBus::publish<Bus::MsgGpsRaw>(generateGPS());
    Bus::FlyteBus::publish<Bus::MsgBaro>(generateBaro());
}

void SITLManager::addNoise(f32* data, int count, f32 sigma) {
    for (int i = 0; i < count; i++) {
        f32 u1 = (rand() + 1) / (f32)(RAND_MAX + 1u);
        f32 u2 = (rand() + 1) / (f32)(RAND_MAX + 1u);
        f32 z0 = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * M_PI * u2);
        data[i] += z0 * sigma;
    }
}

// ════════════════════════════════════════════════════════════════
//  姿态估计
// ════════════════════════════════════════════════════════════════

void SITLManager::runAttitudeEstimation() {
    auto sub_imu = Bus::FlyteBus::subscribe<Bus::MsgImuRaw>();
    auto sub_gps = Bus::FlyteBus::subscribe<Bus::MsgGpsRaw>();

    if (!sub_imu.has()) return;

    const auto& imu = *sub_imu.get();

    // 转换为AttitudeEstimator的输入格式
    Attitude::AttitudeEstimator::IMURaw imu_raw;
    imu_raw.gyro[0] = imu.gyro[0];
    imu_raw.gyro[1] = imu.gyro[1];
    imu_raw.gyro[2] = imu.gyro[2];
    imu_raw.accel[0] = imu.accel[0];
    imu_raw.accel[1] = imu.accel[1];
    imu_raw.accel[2] = imu.accel[2];
    imu_raw.mag[0] = imu.mag[0];
    imu_raw.mag[1] = imu.mag[1];
    imu_raw.mag[2] = imu.mag[2];
    imu_raw.ts = imu.ts;

    Attitude::AttitudeEstimator::GPSRaw gps_raw = {};
    if (sub_gps.has()) {
        const auto& gps = *sub_gps.get();
        gps_raw.lat = gps.lat;
        gps_raw.lon = gps.lon;
        gps_raw.alt = gps.alt_msl;
        gps_raw.vel_ned[0] = gps.vel_ned[0];
        gps_raw.vel_ned[1] = gps.vel_ned[1];
        gps_raw.vel_ned[2] = gps.vel_ned[2];
        gps_raw.fix = gps.fix;
        gps_raw.ts = gps.ts;
    }

    // 更新姿态估计（使用updateWithGPS融合GPS数据）
    Attitude::AttitudeEstimator::Estimate est;
    if (sub_gps.has() && gps_raw.fix > 0) {
        est = attitude_.updateWithGPS(imu_raw, gps_raw);
    } else {
        est = attitude_.update(imu_raw);
    }
    Bus::FlyteBus::publish<Bus::MsgAttitudeEstimate>(
        Bus::MsgAttitudeEstimate{
            est.euler_rad.x, est.euler_rad.y, est.euler_rad.z,
            0, 0, 0,  // 角速率（简化）
            -physics_.pos_d,
            now_us()
        }
    );
    Bus::FlyteBus::publish<Bus::MsgPositionEstimate>(
        Bus::MsgPositionEstimate{
            physics_.pos_n, physics_.pos_e, physics_.pos_d,
            physics_.vel_n, physics_.vel_e, physics_.vel_d,
            now_us()
        }
    );
}

// ════════════════════════════════════════════════════════════════
//  飞行控制
// ════════════════════════════════════════════════════════════════

void SITLManager::runFlightControl() {
    auto sub_att = Bus::FlyteBus::subscribe<Bus::MsgAttitudeEstimate>();
    auto sub_gas = Bus::FlyteBus::subscribe<Bus::MsgGasCellState>();
    auto sub_rc  = Bus::FlyteBus::subscribe<Bus::MsgRcInput>();
    auto sub_fsm = Bus::FlyteBus::subscribe<Bus::MsgFlightState>();

    // ── 状态机事件处理 ──
    if (sub_rc.has()) {
        const auto& rc = *sub_rc.get();
        if (rc.arm) {
            fsm_.handleEvent(Control::FlightStateMachine::Event::ARM);
        }
        if (rc.takeoff && fsm_.state() == FlightState::STANDBY) {
            fsm_.handleEvent(Control::FlightStateMachine::Event::TAKEOFF);
        }
        if (rc.land && fsm_.isFlying()) {
            fsm_.handleEvent(Control::FlightStateMachine::Event::LAND);
        }
        if (rc.emergency) {
            fsm_.handleEvent(Control::FlightStateMachine::Event::EMERGENCY);
        }
    }

    // 自动转IN_FLIGHT（起飞后）
    if (fsm_.state() == FlightState::TAKING_OFF && (-physics_.pos_d) > 2.0f) {
        fsm_.handleEvent(Control::FlightStateMachine::Event::AIRBORNE);
    }

    // 发布飞行状态
    Bus::FlyteBus::publish<Bus::MsgFlightState>(
        Bus::MsgFlightState{fsm_.state(), now_us()}
    );

    // ── 飞控计算（仅飞行状态下执行）──
    if (!fsm_.isFlying()) {
        // 未飞行时输出全零
        Bus::FlyteBus::publish<Bus::MsgActuatorOutput>(Bus::MsgActuatorOutput{});
        return;
    }

    // 构造AttitudeCmd（从RC或自动模式）
    AttitudeCmd cmd;
    if (sub_rc.has()) {
        const auto& rc = *sub_rc.get();
        cmd.roll   = rc.roll * 0.35f;    // max ±0.35 rad ≈ 20°
        cmd.pitch  = rc.pitch * 0.35f;
        cmd.yaw    = physics_.yaw + rc.yaw * 0.5f;
        cmd.thrust = rc.throttle;
    }

    // 姿态控制
    Attitude::AttitudeEstimator::Estimate est;
    if (sub_att.has()) {
        const auto& a = *sub_att.get();
        est.euler_rad = {a.roll_rad, a.pitch_rad, a.yaw_rad};
    }

    Power::HeliumBuoyancy::Status buoy_status;
    if (sub_gas.has()) {
        const auto& g = *sub_gas.get();
        buoy_status.buoyancy_ratio = static_cast<f32>(g.buoyancy_ratio);
        buoy_status.lift_force_n = static_cast<f32>(g.buoyancy_n);
    }

    f32 dt = cfg_.sim_dt;
    f32 gas_buoyancy_n = 0;
    if (sub_gas.has()) gas_buoyancy_n = static_cast<f32>(sub_gas.get()->buoyancy_n);
    ActuatorOutput act_out = att_ctrl_.update(cmd, est, buoy_status, gas_buoyancy_n, dt);

    // 发布执行器输出
    Bus::MsgActuatorOutput msg_act;
    for (int i = 0; i < 4; i++) msg_act.motor[i] = act_out.motor[i];
    msg_act.thrust_n = act_out.thrust_n;

    // 浮力轴控制信号
    if (sub_gas.has()) {
        const auto& g = *sub_gas.get();
        msg_act.vent_open = static_cast<f32>(g.vent_open);
        msg_act.pump_01   = static_cast<f32>(g.pump_active);
    }

    msg_act.ts = now_us();
    Bus::FlyteBus::publish<Bus::MsgActuatorOutput>(msg_act);
}

// ════════════════════════════════════════════════════════════════
//  浮力控制
// ════════════════════════════════════════════════════════════════

void SITLManager::runBuoyancyControl() {
    auto sub_env = Bus::FlyteBus::subscribe<Bus::MsgEnvironment>();

    Power::GasCell::EnvInput env;
    if (sub_env.has()) {
        const auto& e = *sub_env.get();
        env.altitude_m        = e.altitude_m;
        env.air_temp_k        = e.air_temp_k;
        env.air_pressure_pa   = e.air_pressure_pa;
        env.solar_elevation_rad = e.solar_elevation_rad;
        env.solar_irradiance_wm2 = e.solar_irradiance_wm2;
        env.wind_speed_ms     = sqrt(e.wind_n_ms * e.wind_n_ms + e.wind_e_ms * e.wind_e_ms);
    }
    env.dt = cfg_.sim_dt * 8;  // 50Hz的dt

    // 更新气囊
    auto gas_state = gas_cell_.update(env);

    // 根据飞行状态调整目标浮力比
    auto sub_fsm = Bus::FlyteBus::subscribe<Bus::MsgFlightState>();
    if (sub_fsm.has()) {
        f64 target_bw = 0.95;  // 默认巡航
        switch (sub_fsm.get()->state) {
            case FlightState::TAKING_OFF:    target_bw = 1.05; break;  // 起飞多充
            case FlightState::LANDING:       target_bw = 0.85; break;  // 降落放气
            case FlightState::HOVERING:      target_bw = 0.98; break;  // 悬停近平衡
            case FlightState::IN_FLIGHT:      target_bw = 0.93; break;  // 巡航省气
            case FlightState::EMERGENCY_LANDING: target_bw = 0.70; break; // 紧急大放气
            default: break;
        }
        gas_cell_.setTargetBuoyancyRatio(target_bw, cfg_.aircraft_mass_kg);
    }

    // 发布气囊状态
    Bus::FlyteBus::publish<Bus::MsgGasCellState>(
        Bus::MsgGasCellState{
            gas_state.volume_m3, gas_state.pressure_pa, gas_state.temperature_k,
            gas_state.buoyancy_n, gas_state.net_lift_kg,
            cfg_.aircraft_mass_kg * 9.80665 > 0 ?
                gas_state.buoyancy_n / (cfg_.aircraft_mass_kg * 9.80665) : 0,
            gas_state.vent_open, gas_state.pump_active,
            now_us()
        }
    );
}

// ════════════════════════════════════════════════════════════════
//  载荷称重（50Hz）
// ════════════════════════════════════════════════════════════════

void SITLManager::runPayloadScale() {
    // 仿真模式：传入当前总质量（空机+载荷变化模拟）
    // 载荷微小波动模拟（风扰、振动等）
    f32 sim_payload = 0;  // 默认无额外载荷
    // 模拟5秒后加载2kg载荷（模拟投放/拾取场景）
    if (sim_time_s_ > 5.0) {
        sim_payload = 2.0f + 0.1f * sinf(static_cast<f32>(sim_time_s_) * 0.5f);
    }
    f32 true_weight = cfg_.aircraft_mass_kg + sim_payload;

    auto state = payload_scale_.updateSimulated(true_weight, cfg_.sim_dt * 8);

    Bus::FlyteBus::publish<Bus::MsgPayloadState>(
        Bus::MsgPayloadState{
            state.payload_kg,
            state.total_mass_kg,
            state.rate_of_change_kg_s,
            state.overload,
            now_us()
        }
    );
}

// ════════════════════════════════════════════════════════════════
//  涡轮推力模型（400Hz）
// ════════════════════════════════════════════════════════════════

void SITLManager::runTurbineControl() {
    // 从执行器输出获取油门指令
    auto sub_act = Bus::FlyteBus::subscribe<Bus::MsgActuatorOutput>();

    f32 throttle = 0;
    if (sub_act.has()) {
        // 油门映射：飞控输出 thrust_n (0~60N) → 涡轮油门 (0~1)
        // 使用 thrust_n/max_thrust 而非电机均值，确保推力一致性
        constexpr f32 MAX_THRUST_N = 60.0f;
        throttle = std::clamp(sub_act.get()->thrust_n / MAX_THRUST_N, 0.0f, 1.0f);
    }

    // 更新涡轮模型，传入当前电池电压
    f32 battery_v = 0;  // 0表示使用内置电池模型
    auto sub_turb = Bus::FlyteBus::subscribe<Bus::MsgTurbineState>();
    if (sub_turb.has()) {
        battery_v = sub_turb.get()->battery_voltage_v;
    }

    // 根据高度调整空气密度
    f64 alt = -physics_.pos_d;
    f32 rho = static_cast<f32>(1.225 * pow(1.0 - 0.0065 * alt / 288.15, 4.255));
    turbine_.setAirDensity(rho);

    auto turb_state = turbine_.update(throttle, battery_v, cfg_.sim_dt);

    // 发布涡轮状态
    Bus::FlyteBus::publish<Bus::MsgTurbineState>(
        Bus::MsgTurbineState{
            turb_state.throttle_01,
            turb_state.actual_rpm,
            turb_state.thrust_n,
            turb_state.torque_nm,
            turb_state.motor_voltage_v,
            turb_state.motor_current_a,
            turb_state.battery_voltage_v,
            turb_state.power_elec_w,
            turb_state.efficiency_pct,
            turb_state.battery_remaining_pct,
            now_us()
        }
    );
}

// ════════════════════════════════════════════════════════════════
//  太阳能发电（10Hz）
// ════════════════════════════════════════════════════════════════

void SITLManager::runSolarEnergy() {
    auto sub_env = Bus::FlyteBus::subscribe<Bus::MsgEnvironment>();

    f64 sun_elev = cfg_.solar_elevation_rad;
    f64 sun_azimuth = 0;  // 简化：南向
    f64 air_temp_k = 288.15;

    if (sub_env.has()) {
        sun_elev = sub_env.get()->solar_elevation_rad;
        air_temp_k = sub_env.get()->air_temp_k;
    }

    f32 dt_10hz = cfg_.sim_dt * 40;  // 10Hz的dt

    auto solar_state = solar_membrane_.updateWithIrradiance(
        sun_elev, sub_env.has() ? sub_env.get()->solar_irradiance_wm2 : 800.0,
        air_temp_k, dt_10hz
    );

    // 太阳能为电池充电（简化：充电功率 = 净发电功率）
    if (solar_state.power_net_w > 0) {
        // 涡轮电池充电（简化模型：直接增加剩余电量）
        // 注意：TurbineModel内部有自己的电池模型，这里只发布状态供显示
    }

    Bus::FlyteBus::publish<Bus::MsgSolarState>(
        Bus::MsgSolarState{
            solar_state.power_gross_w,
            solar_state.power_net_w,
            solar_state.energy_wh,
            solar_state.sun.elevation_rad,
            solar_state.sun.azimuth_rad,
            solar_state.membrane_irr_wm2,
            solar_state.effective_efficiency,
            solar_state.is_day,
            now_us()
        }
    );
}

// ════════════════════════════════════════════════════════════════
//  温度预测（10Hz）
// ════════════════════════════════════════════════════════════════

void SITLManager::runThermalPrediction() {
    auto sub_gas = Bus::FlyteBus::subscribe<Bus::MsgGasCellState>();
    auto sub_env = Bus::FlyteBus::subscribe<Bus::MsgEnvironment>();

    Power::ThermalPredictor::EnvInput env;
    env.current_temp_k = 288.15;  // 默认
    env.air_temp_k     = 288.15;
    env.solar_elev_rad = cfg_.solar_elevation_rad;
    env.solar_irr_wm2  = cfg_.solar_irradiance_wm2;

    if (sub_gas.has()) {
        env.current_temp_k = sub_gas.get()->temperature_k;
        env.vent_flow_kgs  = sub_gas.get()->vent_open * 0.01;  // 简化流量映射
        env.pump_flow_kgs  = sub_gas.get()->pump_active * 0.005;
    }

    if (sub_env.has()) {
        env.air_temp_k      = sub_env.get()->air_temp_k;
        env.solar_elev_rad  = sub_env.get()->solar_elevation_rad;
        env.solar_irr_wm2   = sub_env.get()->solar_irradiance_wm2;
        env.wind_speed_ms   = sqrt(sub_env.get()->wind_n_ms * sub_env.get()->wind_n_ms +
                                    sub_env.get()->wind_e_ms * sub_env.get()->wind_e_ms);
    }

    auto summary = thermal_predictor_.predict(env);

    Bus::FlyteBus::publish<Bus::MsgThermalPrediction>(
        Bus::MsgThermalPrediction{
            summary.temp_now_k,
            summary.temp_30s_k,
            summary.temp_1min_k,
            summary.temp_5min_k,
            summary.trend_kps,
            summary.heating,
            summary.overheat_risk,
            summary.superheat_risk,
            now_us()
        }
    );
}

// ════════════════════════════════════════════════════════════════
//  物理推进
// ════════════════════════════════════════════════════════════════

void SITLManager::applyForces(f32 dt) {
    auto sub_gas = Bus::FlyteBus::subscribe<Bus::MsgGasCellState>();
    auto sub_env = Bus::FlyteBus::subscribe<Bus::MsgEnvironment>();
    auto sub_turb = Bus::FlyteBus::subscribe<Bus::MsgTurbineState>();
    auto sub_payload = Bus::FlyteBus::subscribe<Bus::MsgPayloadState>();

    f32 g = 9.80665f;

    // 动态质量：空机质量 + 载荷（由PayloadScale提供）
    // 安全防护：确保质量不为零或负值
    f32 mass = cfg_.aircraft_mass_kg;
    if (sub_payload.has() && sub_payload.get()->total_mass_kg > 1.0f) {
        mass = sub_payload.get()->total_mass_kg;
    }

    // 1. 推力 —— 使用飞控输出的推力（包含浮力补偿）
    // 涡轮推力模型提供真实的电机推力数据供遥测显示
    // 浮空器的主要升力来自浮力（B/W≈1.0），电机推力仅辅助
    // 飞控的thrust_n已经包含了浮力补偿分量，直接用于物理推进
    auto sub_act_phys = Bus::FlyteBus::subscribe<Bus::MsgActuatorOutput>();
    f32 thrust_n = 0;
    if (sub_act_phys.has()) {
        thrust_n = sub_act_phys.get()->thrust_n;
    }

    // 2. 浮力
    f64 buoyancy_n = 0;
    if (sub_gas.has()) buoyancy_n = sub_gas.get()->buoyancy_n;

    // 3. 重力（使用动态质量）
    f32 weight_n = mass * g;

    // 4. 气动阻力
    f32 cd = cfg_.drag_coefficient;
    f32 drag_n = cd * physics_.vel_n;
    f32 drag_e = cd * physics_.vel_e;
    f32 drag_d = cd * physics_.vel_d;

    // 5. 风力
    f64 wind_n = 0, wind_e = 0;
    if (sub_env.has()) {
        wind_n = sub_env.get()->wind_n_ms;
        wind_e = sub_env.get()->wind_e_ms;
    }
    f32 wind_force_n = cd * (static_cast<f32>(wind_n) - physics_.vel_n) * 0.1f;
    f32 wind_force_e = cd * (static_cast<f32>(wind_e) - physics_.vel_e) * 0.1f;

    // 6. 合力（NED坐标系，down为正）
    f32 F_n = -drag_n + wind_force_n;
    f32 F_e = -drag_e + wind_force_e;
    f32 F_d = weight_n - static_cast<f32>(buoyancy_n) - thrust_n - drag_d;

    // 7. 加速度（使用动态质量）
    f32 a_n = F_n / mass;
    f32 a_e = F_e / mass;
    f32 a_d = F_d / mass;

    // 8. 速度积分
    physics_.vel_n += a_n * dt;
    physics_.vel_e += a_e * dt;
    physics_.vel_d += a_d * dt;

    // 速度限幅
    f32 horiz_speed = sqrtf(physics_.vel_n * physics_.vel_n + physics_.vel_e * physics_.vel_e);
    if (horiz_speed > 18.0f) {
        f32 ratio = 18.0f / horiz_speed;
        physics_.vel_n *= ratio;
        physics_.vel_e *= ratio;
    }
    physics_.vel_d = std::clamp(physics_.vel_d, -8.0f, 5.0f);

    // 9. 位置积分
    physics_.pos_n += physics_.vel_n * dt;
    physics_.pos_e += physics_.vel_e * dt;
    physics_.pos_d += physics_.vel_d * dt;

    // 10. 地面碰撞保护
    if (physics_.pos_d > 0) {
        physics_.pos_d = 0;
        if (physics_.vel_d > 0) physics_.vel_d = 0;
    }

    // 11. 姿态简化更新
    f32 tau = 0.1f;
    physics_.roll  += (0 - physics_.roll) * dt / tau;
    physics_.pitch += (0 - physics_.pitch) * dt / tau;
}

// ════════════════════════════════════════════════════════════════
//  环境更新
// ════════════════════════════════════════════════════════════════

void SITLManager::updateEnvironment() {
    f64 alt = -physics_.pos_d;

    // ISA大气模型
    f64 T = 288.15 - 0.0065 * alt;
    f64 P = 101325.0 * pow(1.0 - 0.0065 * alt / 288.15, 5.255);

    Bus::FlyteBus::publish<Bus::MsgEnvironment>(
        Bus::MsgEnvironment{
            alt, T, P,
            cfg_.wind_n_ms, cfg_.wind_e_ms, 0,
            cfg_.solar_elevation_rad, cfg_.solar_irradiance_wm2,
            now_us()
        }
    );
}

// ════════════════════════════════════════════════════════════════
//  安全检查
// ════════════════════════════════════════════════════════════════

void SITLManager::runSafetyCheck() {
    Bus::MsgSafetyStatus safety;
    safety.safe = true;

    f32 alt = -physics_.pos_d;
    if (alt > 300.0f) {
        safety.safe = false;
        safety.warnings.push_back("Altitude exceeds 300m limit");
    }

    auto sub_gas = Bus::FlyteBus::subscribe<Bus::MsgGasCellState>();
    if (sub_gas.has() && sub_gas.get()->pressure_pa > 110000) {
        safety.safe = false;
        safety.warnings.push_back("Gas cell overpressure");
    }

    // 温度预测告警
    auto sub_thermal = Bus::FlyteBus::subscribe<Bus::MsgThermalPrediction>();
    if (sub_thermal.has()) {
        if (sub_thermal.get()->superheat_risk) {
            safety.safe = false;
            safety.warnings.push_back("Thermal: superheat risk (>80C predicted)");
        } else if (sub_thermal.get()->overheat_risk) {
            safety.warnings.push_back("Thermal: overheat warning (>60C predicted)");
        }
    }

    // 载荷过载告警
    auto sub_payload = Bus::FlyteBus::subscribe<Bus::MsgPayloadState>();
    if (sub_payload.has() && sub_payload.get()->overload) {
        safety.safe = false;
        safety.warnings.push_back("Payload overload detected");
    }

    // 电池低电量告警
    auto sub_turb = Bus::FlyteBus::subscribe<Bus::MsgTurbineState>();
    if (sub_turb.has() && sub_turb.get()->battery_pct < 15.0f) {
        safety.warnings.push_back("Battery low: <15%");
        if (sub_turb.get()->battery_pct < 5.0f) {
            safety.safe = false;
            safety.warnings.push_back("Battery critical: <5%");
        }
    }

    safety.ts = now_us();
    Bus::FlyteBus::publish<Bus::MsgSafetyStatus>(safety);
}

// ════════════════════════════════════════════════════════════════
//  遥控/遥测
// ════════════════════════════════════════════════════════════════

void SITLManager::setRcInput(const Bus::MsgRcInput& rc) {
    rc_input_ = rc;
    Bus::FlyteBus::publish<Bus::MsgRcInput>(rc);
}

SITLManager::TelemetrySnapshot SITLManager::telemetry() const {
    TelemetrySnapshot snap;
    snap.pos_n = physics_.pos_n;
    snap.pos_e = physics_.pos_e;
    snap.pos_d = physics_.pos_d;
    snap.vel_n = physics_.vel_n;
    snap.vel_e = physics_.vel_e;
    snap.vel_d = physics_.vel_d;
    snap.roll_rad  = physics_.roll;
    snap.pitch_rad = physics_.pitch;
    snap.yaw_rad   = physics_.yaw;

    auto sub_fsm = Bus::FlyteBus::subscribe<Bus::MsgFlightState>();
    snap.flight_state = sub_fsm.has() ? sub_fsm.get()->state : FlightState::DISARMED;

    auto sub_act = Bus::FlyteBus::subscribe<Bus::MsgActuatorOutput>();
    if (sub_act.has()) {
        for (int i = 0; i < 4; i++) snap.motor[i] = sub_act.get()->motor[i];
        snap.thrust_n = sub_act.get()->thrust_n;
    }

    auto sub_gas = Bus::FlyteBus::subscribe<Bus::MsgGasCellState>();
    if (sub_gas.has()) {
        snap.buoyancy_n     = sub_gas.get()->buoyancy_n;
        snap.buoyancy_ratio = sub_gas.get()->buoyancy_ratio;
        snap.gas_volume_m3  = sub_gas.get()->volume_m3;
        snap.gas_temp_k     = sub_gas.get()->temperature_k;
        snap.gas_pressure_pa = sub_gas.get()->pressure_pa;
        snap.vent_open      = sub_gas.get()->vent_open;
        snap.pump_active    = sub_gas.get()->pump_active;
    }

    // 涡轮状态
    auto sub_turb = Bus::FlyteBus::subscribe<Bus::MsgTurbineState>();
    if (sub_turb.has()) {
        snap.turbine_rpm      = sub_turb.get()->actual_rpm;
        snap.turbine_thrust_n = sub_turb.get()->thrust_n;
        snap.turbine_eff_pct  = sub_turb.get()->efficiency_pct;
        snap.motor_current_a  = sub_turb.get()->motor_current_a;
        snap.motor_voltage_v  = sub_turb.get()->motor_voltage_v;
        snap.battery_pct       = sub_turb.get()->battery_pct;
    }

    // 太阳能状态
    auto sub_solar = Bus::FlyteBus::subscribe<Bus::MsgSolarState>();
    if (sub_solar.has()) {
        snap.solar_power_w   = sub_solar.get()->power_net_w;
        snap.solar_energy_wh = sub_solar.get()->energy_wh;
        snap.solar_is_day    = sub_solar.get()->is_day;
    }

    // 温度预测
    auto sub_thermal = Bus::FlyteBus::subscribe<Bus::MsgThermalPrediction>();
    if (sub_thermal.has()) {
        snap.thermal_now_k    = sub_thermal.get()->temp_now_k;
        snap.thermal_5min_k   = sub_thermal.get()->temp_5min_k;
        snap.thermal_overheat = sub_thermal.get()->overheat_risk;
    }

    // 载荷状态
    auto sub_payload = Bus::FlyteBus::subscribe<Bus::MsgPayloadState>();
    if (sub_payload.has()) {
        snap.payload_kg    = sub_payload.get()->payload_kg;
        snap.total_mass_kg = sub_payload.get()->total_mass_kg;
    }

    snap.sim_time_s  = sim_time_s_;
    snap.step_count  = step_count_;
    return snap;
}

void SITLManager::reset() {
    physics_ = {};
    physics_.pos_d = -cfg_.launch_altitude_m;
    sim_time_s_ = 0;
    step_count_ = 0;
    Bus::FlyteBus::reset();
    printf("[SITL] Reset\n");
}

} // namespace FlyteOS::Sim
