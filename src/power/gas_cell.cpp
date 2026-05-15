/**
 * @file gas_cell.cpp
 * @brief 浮力气囊物理模型实现 —— PV=nRT 理想气体定律
 */

#include "gas_cell.hpp"
#include <algorithm>
#include <cmath>

namespace FlyteOS::Power {

// ════════════════════════════════════════════════════════════════
//  国际标准大气模型 ISA (International Standard Atmosphere)
// ════════════════════════════════════════════════════════════════

f64 GasCell::isaTemperature(f64 alt_m) {
    // 对流层：T = T0 - L·h (0-11km)
    if (alt_m < 11000.0) {
        return Phys::T_SEA - Phys::LAPSE_RATE * alt_m;
    }
    // 平流层下层：T ≈ 216.65K（等温层）
    return 216.65;
}

f64 GasCell::isaPressure(f64 alt_m) {
    // 气压高度公式（对流层）：
    // P = P0 · (1 - L·h/T0)^(g·M_air/(R·L))
    if (alt_m < 11000.0) {
        const f64 exponent = Phys::G * Phys::M_AIR / (Phys::R_GAS * Phys::LAPSE_RATE);
        return Phys::P_SEA * std::pow(1.0 - Phys::LAPSE_RATE * alt_m / Phys::T_SEA, exponent);
    }
    // 平流层下层（简化）
    const f64 p_11km = Phys::P_SEA * std::pow(1.0 - Phys::LAPSE_RATE * 11000.0 / Phys::T_SEA,
                    Phys::G * Phys::M_AIR / (Phys::R_GAS * Phys::LAPSE_RATE));
    const f64 t_11km = 216.65;
    return p_11km * std::exp(-Phys::G * Phys::M_AIR * (alt_m - 11000.0) / (Phys::R_GAS * t_11km));
}

f64 GasCell::isaAirDensity(f64 alt_m) {
    // ρ = P·M_air / (R·T)
    const f64 P = isaPressure(alt_m);
    const f64 T = isaTemperature(alt_m);
    return P * Phys::M_AIR / (Phys::R_GAS * T);
}

// ════════════════════════════════════════════════════════════════
//  构造与初始化
// ════════════════════════════════════════════════════════════════

GasCell::GasCell(Config cfg) : cfg_(std::move(cfg)) {
    // 初始状态为零
    state_ = {};
}

void GasCell::initialize(f64 target_bw, f64 total_mass_kg) {
    // 目标浮力 = B/W × 总重量
    const f64 target_buoyancy_n = target_bw * total_mass_kg * Phys::G;

    // 在海平面条件下计算所需氦气体积
    const f64 rho_air = isaAirDensity(0);
    const f64 rho_he  = rho_air * Phys::M_HE / Phys::M_AIR;

    // 浮力 = (ρ_air - ρ_He) · V · g
    // V = target_buoyancy / ((ρ_air - ρ_He) · g)
    const f64 delta_rho = rho_air - rho_he;
    if (delta_rho > 0) {
        state_.volume_m3 = target_buoyancy_n / (delta_rho * Phys::G);
    } else {
        state_.volume_m3 = cfg_.max_volume_m3;
    }

    // 限制在最大容积内
    state_.volume_m3 = std::min(state_.volume_m3, cfg_.max_volume_m3);

    // PV = nRT → n = PV/(RT)
    state_.pressure_pa = Phys::P_SEA;
    state_.temperature_k = Phys::T_SEA;
    state_.he_moles = (state_.pressure_pa * state_.volume_m3) /
                       (Phys::R_GAS * state_.temperature_k);

    // 环境初始值
    state_.rho_air_kgm3 = rho_air;
    state_.rho_he_kgm3  = rho_he;
    state_.he_mass_kg = state_.he_moles * Phys::M_HE;
    state_.gas_temperature_k = state_.temperature_k;

    updateBuoyancy();
}

// ════════════════════════════════════════════════════════════════
//  主更新函数
// ════════════════════════════════════════════════════════════════

GasCell::State GasCell::update(const EnvInput& env) {
    // 1. 热力学更新（温度、压力、体积）
    updateThermodynamics(env);

    // 2. 超压保护
    checkOverpressure(env.dt);

    // 3. 浮力计算
    updateBuoyancy();

    return state_;
}

// ════════════════════════════════════════════════════════════════
//  热力学更新
// ════════════════════════════════════════════════════════════════

void GasCell::updateThermodynamics(const EnvInput& env) {
    const f64 dt = env.dt;
    if (dt <= 0) return;

    // ── 太阳加热模型 ──
    // Q_solar = α · I · A_effective · sin(elevation)
    // A_effective ≈ V^(2/3) · π (简化球体投影面积)
    const f64 solar_heating_k = computeSolarHeating(
        env.solar_elevation_rad, env.solar_irradiance_wm2, env.wind_speed_ms);

    // ── 热交换模型（牛顿冷却定律）──
    // dT_gas/dt = (T_env - T_gas) / τ + Q_solar / τ
    // 温度趋向环境温度 + 太阳加热偏移
    const f64 target_temp_k = env.air_temp_k + solar_heating_k;

    // 指数衰减趋向目标温度
    const f64 alpha = 1.0 - std::exp(-dt / cfg_.thermal_time_const_s);
    state_.gas_temperature_k += (target_temp_k - state_.gas_temperature_k) * alpha;

    // 风冷效应：风速越大，热时间常数越小（冷却更快）
    // 实际应用：τ_effective = τ / (1 + k·v)
    // 简化：已在 solar_heating 中考虑

    // ── PV=nRT 状态更新 ──
    // 气囊体积受最大容积限制
    // 如果 P·n·R·T 计算的体积 > max_volume，则压力增加（超压状态）
    state_.temperature_k = state_.gas_temperature_k;

    // 根据当前温度和氦气量计算新体积
    // V = n·R·T / P_env（等压过程，气囊与环境相通的简化假设）
    f64 new_volume = state_.he_moles * Phys::R_GAS * state_.temperature_k / env.air_pressure_pa;

    if (new_volume > cfg_.max_volume_m3) {
        // 超压状态：体积被限制，压力升高
        state_.volume_m3 = cfg_.max_volume_m3;
        state_.pressure_pa = state_.he_moles * Phys::R_GAS * state_.temperature_k / state_.volume_m3;
    } else if (new_volume < cfg_.max_volume_m3 * 0.3) {
        // 瘪气状态：气囊未充满
        state_.volume_m3 = std::max(new_volume, cfg_.max_volume_m3 * 0.1);
        state_.pressure_pa = env.air_pressure_pa;  // 内外压平衡
    } else {
        // 正常状态
        state_.volume_m3 = new_volume;
        state_.pressure_pa = env.air_pressure_pa;
    }

    // 更新氦气密度
    state_.rho_he_kgm3 = (state_.he_moles * Phys::M_HE) / state_.volume_m3;

    // 压气机/放气阀效果
    if (state_.pump_active > 0) {
        // 压气机：从储气罐补氦（简化为增加摩尔数）
        // 假设储气罐有足够的氦气
        const f64 dn = state_.pump_active * cfg_.pump_flow_rate_m3s * dt *
                       env.air_pressure_pa / (Phys::R_GAS * state_.temperature_k);
        state_.he_moles += dn;
    }

    if (state_.vent_open > 0) {
        // 放气阀：减少摩尔数
        const f64 dn = state_.vent_open * cfg_.vent_flow_rate_m3s * dt *
                       state_.pressure_pa / (Phys::R_GAS * state_.temperature_k);
        state_.he_moles = std::max(0.0, state_.he_moles - dn);
    }

    // 氦气质量
    state_.he_mass_kg = state_.he_moles * Phys::M_HE;

    // 环境空气密度
    state_.rho_air_kgm3 = isaAirDensity(env.altitude_m);
}

// ════════════════════════════════════════════════════════════════
//  太阳加热模型
// ════════════════════════════════════════════════════════════════

f64 GasCell::computeSolarHeating(f64 solar_elev, f64 solar_irr, f64 wind_speed) const {
    // 太阳仰角 < 0 → 无太阳（夜间）
    if (solar_elev <= 0) return 0;

    // 有效投影面积 ≈ V^(2/3) · π · sin(elevation)
    const f64 a_eff = std::pow(state_.volume_m3, 2.0 / 3.0) * M_PI * std::sin(solar_elev);

    // 吸收功率 = α · I · A
    const f64 q_absorbed_w = cfg_.solar_absorptivity * solar_irr * a_eff;

    // 热容 C_He = n · Cv_He，Cv_He ≈ 12.5 J/(mol·K)（单原子理想气体 3R/2）
    const f64 cv_he = 1.5 * Phys::R_GAS;
    const f64 c_thermal = state_.he_moles * cv_he;

    if (c_thermal <= 0) return 0;

    // 温升 ΔT = Q / C，但受风冷衰减
    // 风冷系数：k_wind = 1 / (1 + 0.05·v)
    const f64 wind_cooling = 1.0 / (1.0 + 0.05 * wind_speed);

    // 太阳加热导致的稳态温升（简化）
    const f64 delta_t_steady = (q_absorbed_w * wind_cooling) / (c_thermal / cfg_.thermal_time_const_s);

    return delta_t_steady;
}

// ════════════════════════════════════════════════════════════════
//  超压保护
// ════════════════════════════════════════════════════════════════

void GasCell::checkOverpressure(f64 dt) {
    if (state_.pressure_pa > cfg_.max_pressure_pa) {
        // 超压！自动打开放气阀
        // 放气速率正比于超压量
        const f64 overpressure_ratio = (state_.pressure_pa - cfg_.max_pressure_pa) /
                                        (cfg_.max_pressure_pa * 0.1);  // 10%超压为满开度
        const f64 vent_openness = std::min(1.0, overpressure_ratio);

        // 放气：减少摩尔数
        const f64 dn = vent_openness * cfg_.vent_flow_rate_m3s * dt *
                       state_.pressure_pa / (Phys::R_GAS * state_.temperature_k);
        state_.he_moles = std::max(0.0, state_.he_moles - dn);

        // 重新计算压力
        state_.pressure_pa = state_.he_moles * Phys::R_GAS * state_.temperature_k / state_.volume_m3;

        state_.vent_open = vent_openness;
    } else {
        state_.vent_open = 0;
    }
}

// ════════════════════════════════════════════════════════════════
//  浮力计算
// ════════════════════════════════════════════════════════════════

void GasCell::updateBuoyancy() {
    // 阿基米德原理：浮力 = 排开空气重量 - 氦气重量
    // F_b = (ρ_air - ρ_He) · V · g
    const f64 delta_rho = state_.rho_air_kgm3 - state_.rho_he_kgm3;
    state_.buoyancy_n = delta_rho * state_.volume_m3 * Phys::G;

    // 毛升力 = 浮力/g（等效kg）
    state_.gross_lift_kg = state_.buoyancy_n / Phys::G;

    // 净升力 = 毛升力 - 囊体质量 - 氦气质量
    state_.net_lift_kg = state_.gross_lift_kg - cfg_.envelope_mass_kg - state_.he_mass_kg;
}

// ════════════════════════════════════════════════════════════════
//  手动控制接口
// ════════════════════════════════════════════════════════════════

void GasCell::setVentOpen(f64 open01) {
    state_.vent_open = std::max(0.0, std::min(1.0, open01));
}

void GasCell::setPumpActive(f64 active01) {
    state_.pump_active = std::max(0.0, std::min(1.0, active01));
}

void GasCell::setTargetBuoyancyRatio(f64 bw, f64 total_mass_kg) {
    // 根据目标浮力比计算所需浮力
    const f64 target_buoyancy_n = bw * total_mass_kg * Phys::G;

    // 当前浮力
    const f64 current_buoyancy_n = state_.buoyancy_n;

    if (target_buoyancy_n > current_buoyancy_n) {
        // 需要增加浮力 → 压气机补气
        state_.pump_active = 1.0;
        state_.vent_open = 0.0;
    } else if (target_buoyancy_n < current_buoyancy_n * 0.98) {
        // 需要减少浮力 → 放气
        state_.pump_active = 0.0;
        state_.vent_open = std::min(1.0, (current_buoyancy_n - target_buoyancy_n) /
                                              (current_buoyancy_n * 0.05));
    } else {
        // 浮力在目标范围内
        state_.pump_active = 0.0;
        state_.vent_open = 0.0;
    }
}

} // namespace FlyteOS::Power
