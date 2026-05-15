#pragma once
/**
 * @file sitl_manager.hpp
 * @brief SITL(Software-In-The-Loop)仿真管理器
 *
 * 将飞控各模块通过FlyteBus串联，形成完整仿真闭环：
 *
 *   传感器注入 → 姿态估计 → 飞行控制 → 混控输出 → 物理引擎推进 → 传感器注入
 *        ↑                                                                    ↓
 *        └────────────────── FlyteBus 消息总线 ──────────────────────────────┘
 *
 * 仿真频率：
 *   - 飞控内环：400Hz（姿态/混控）
 *   - 姿态估计：200Hz
 *   - 浮力/环境：50Hz
 *   - 安全检查：10Hz
 */

#include "../../include/flyteos_types.hpp"
#include "../bus/flytebus.hpp"
#include "../attitude/attitude_estimator.hpp"
#include "../flight_control/flight_controller.hpp"
#include "../power/helium_buoyancy.hpp"
#include "../power/gas_cell.hpp"
#include "../power/solar_mppt.hpp"
#include "../navigation/waypoint_navigator.hpp"
#include "../safety/safety_monitor.hpp"
#include <cstdio>
#include <cmath>

namespace FlyteOS::Sim {

class SITLManager {
public:
    struct Config {
        // 仿真时间
        f32 sim_dt            = 0.0025f;    // 仿真步长 2.5ms = 400Hz
        f32 realtime_factor   = 1.0f;       // 实时倍速

        // 物理引擎
        f32 aircraft_mass_kg  = 12.0f;
        f32 max_thrust_n      = 60.0f;
        f32 drag_coefficient  = 0.5f;
        f32 launch_altitude_m = 0.0f;

        // 气囊
        Power::GasCell::Config gas_cell_cfg = {};

        // 传感器噪声
        f32 gyro_noise_rads   = 0.01f;
        f32 accel_noise_ms2   = 0.05f;
        f32 gps_noise_m       = 2.0f;
        f32 baro_noise_m      = 0.5f;

        // 环境
        f64 wind_n_ms         = 0;
        f64 wind_e_ms         = 0;
        f64 solar_elevation_rad = 0.5;  // 太阳仰角 ~28.6°
        f64 solar_irradiance_wm2 = 800; // W/m²
    };

    /// 仿真状态快照（用于遥测输出）
    struct TelemetrySnapshot {
        // 位置/速度
        f32 pos_n = 0, pos_e = 0, pos_d = 0;
        f32 vel_n = 0, vel_e = 0, vel_d = 0;

        // 姿态
        f32 roll_rad = 0, pitch_rad = 0, yaw_rad = 0;

        // 飞控
        FlightState flight_state = FlightState::DISARMED;
        f32 motor[4] = {};
        f32 thrust_n = 0;

        // 浮力
        f64 buoyancy_n    = 0;
        f64 buoyancy_ratio = 0;
        f64 gas_volume_m3 = 0;
        f64 gas_temp_k    = 0;
        f64 gas_pressure_pa = 0;
        f64 vent_open     = 0;
        f64 pump_active   = 0;

        // 电池
        f32 battery_pct = 100;

        // 仿真
        f64 sim_time_s    = 0;
        u64  step_count   = 0;
    };

    explicit SITLManager(Config cfg);
    SITLManager() : SITLManager(Config{}) {}

    /// 初始化仿真
    Error init();

    /// 推进一步仿真
    Error step();

    /// 发送遥控指令
    void setRcInput(const Bus::MsgRcInput& rc);

    /// 获取遥测快照
    TelemetrySnapshot telemetry() const;

    /// 重置仿真
    void reset();

    // 状态查询
    f64 simTime() const { return sim_time_s_; }
    u64  stepCount() const { return step_count_; }
    bool isRunning() const { return running_; }

private:
    Config cfg_;
    bool  running_ = false;
    f64   sim_time_s_ = 0;
    u64   step_count_ = 0;

    // ── 物理状态 ──
    struct PhysicsState {
        f32 pos_n = 0, pos_e = 0, pos_d = 0;  // NED
        f32 vel_n = 0, vel_e = 0, vel_d = 0;
        f32 roll = 0, pitch = 0, yaw = 0;       // rad
    } physics_;

    // ── 子系统实例 ──
    Attitude::AttitudeEstimator    attitude_;
    Control::FlightStateMachine    fsm_;
    Control::PositionController    pos_ctrl_;
    Control::VelocityController    vel_ctrl_;
    Control::AttitudeController    att_ctrl_;
    Power::GasCell                 gas_cell_;
    Power::HeliumBuoyancy          buoyancy_ctrl_;
    Safety::SafetyMonitor          safety_;

    // ── 仿真内部流程 ──
    void injectSensors();            // 生成仿真传感器数据 → FlyteBus
    void runAttitudeEstimation();    // 姿态估计
    void runFlightControl();         // 飞控三环PID
    void runBuoyancyControl();       // 浮力控制
    void runPhysics();               // 物理引擎推进
    void runSafetyCheck();           // 安全检查
    void updateEnvironment();        // 环境更新

    // ── 传感器模拟 ──
    Bus::MsgImuRaw  generateIMU();
    Bus::MsgGpsRaw  generateGPS();
    Bus::MsgBaro    generateBaro();
    void addNoise(f32* data, int count, f32 sigma);

    // ── 物理推进 ──
    void applyForces(f32 dt);

    // ── 指令 ──
    Bus::MsgRcInput rc_input_;
};

} // namespace FlyteOS::Sim
