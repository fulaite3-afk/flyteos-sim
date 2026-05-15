#pragma once
/**
 * @file gas_cell.hpp
 * @brief 浮力气囊物理模型 —— 基于 PV=nRT 理想气体定律
 *
 * 核心物理：
 *   P·V = n·R·T
 *   浮力 = (ρ_air - ρ_He) · V · g
 *   温度影响：T变化→V变化→浮力变化
 *   超压保护：P > P_max 时自动放气
 */

#include "../../include/flyteos_types.hpp"
#include <cmath>

namespace FlyteOS::Power {

/// 物理常量
namespace Phys {
    constexpr f64 R_GAS      = 8.314462618;   // 通用气体常数 J/(mol·K)
    constexpr f64 M_HE       = 0.004002602;   // 氦气摩尔质量 kg/mol
    constexpr f64 M_AIR      = 0.028964700;   // 空气摩尔质量 kg/mol
    constexpr f64 G          = 9.80665;        // 重力加速度 m/s²
    constexpr f64 P_SEA      = 101325.0;       // 海平面标准气压 Pa
    constexpr f64 T_SEA      = 288.15;         // 海平面标准温度 K (15°C)
    constexpr f64 LAPSE_RATE = 0.0065;         // 温度递减率 K/m (对流层)
    constexpr f64 RHO_AIR_SEA = 1.225;         // 海平面空气密度 kg/m³
}

/**
 * 浮力气囊模型
 *
 * 模拟氦气囊的热力学行为：
 * - PV=nRT 状态方程
 * - 环境温度/气压随高度变化（国际标准大气模型 ISA）
 * - 太阳辐射加热
 * - 超压自动放气
 * - 压气机补气
 */
class GasCell {
public:
    /// 气囊配置
    struct Config {
        f64 max_volume_m3     = 120.0;     // 最大容积 m³
        f64 max_pressure_pa   = 110000.0;  // 超压保护阈值 Pa (约1.085atm)
        f64 min_pressure_pa   = 90000.0;   // 最低气压 Pa (约0.888atm)
        f64 envelope_mass_kg  = 8.5;        // 囊体质量 kg
        f64 solar_absorptivity = 0.35;      // 太阳吸收率（深色涂层0.5，银色0.15）
        f64 thermal_time_const_s = 600.0;   // 热时间常数（秒），越大热惯性越大
        f64 vent_flow_rate_m3s = 0.5;        // 放气阀流量 m³/s（标准工况）
        f64 pump_flow_rate_m3s = 0.02;       // 压气机补气流量 m³/s
    };

    /// 气囊状态
    struct State {
        f64 he_moles     = 0;      // 氦气摩尔数 mol
        f64 volume_m3    = 0;      // 当前体积 m³
        f64 pressure_pa  = 0;      // 当前气压 Pa
        f64 temperature_k = 0;     // 氦气温度 K
        f64 buoyancy_n   = 0;      // 当前浮力 N
        f64 gross_lift_kg = 0;     // 毛升力 kg（浮力/g）
        f64 net_lift_kg  = 0;      // 净升力 kg（毛升力 - 囊体质量 - 氦气质量）
        f64 he_mass_kg   = 0;      // 氦气质量 kg
        f64 rho_air_kgm3 = 0;     // 空气密度 kg/m³
        f64 rho_he_kgm3  = 0;     // 氦气密度 kg/m³
        f64 gas_temperature_k = 0; // 气体温度（含太阳加热）K
        f64 vent_open     = 0;     // 放气阀开度 [0,1]
        f64 pump_active   = 0;     // 压气机状态 [0,1]
    };

    /// 环境数据（从传感器获取）
    struct EnvInput {
        f64 altitude_m    = 0;     // 海拔高度 m
        f64 air_temp_k    = 288.15;// 环境空气温度 K
        f64 air_pressure_pa = 101325; // 环境气压 Pa
        f64 solar_elevation_rad = 0;  // 太阳仰角 rad
        f64 solar_irradiance_wm2 = 0; // 太阳辐照度 W/m²
        f64 wind_speed_ms  = 0;    // 风速 m/s（风冷效应）
        f64 dt             = 0.01;  // 时间步长 s
    };

    explicit GasCell(Config cfg);
    GasCell() : GasCell(Config{}) {}

    /// 初始化：设定初始氦气量使浮力比达到目标值
    /// @param target_bw 目标浮力比 B/W (如0.95)
    /// @param total_mass_kg 飞行器总质量 kg
    void initialize(f64 target_bw, f64 total_mass_kg);

    /// 主更新函数：每步调用
    State update(const EnvInput& env);

    /// 手动控制接口
    void setVentOpen(f64 open01);        // 放气阀开度 [0,1]
    void setPumpActive(f64 active01);    // 压气机 [0,1]

    /// 设定目标浮力比（浮力优化器调用）
    void setTargetBuoyancyRatio(f64 bw, f64 total_mass_kg);

    const State& state() const { return state_; }
    const Config& config() const { return cfg_; }
    void setConfig(const Config& c) { cfg_ = c; }

private:
    Config cfg_;
    State  state_;

    // 国际标准大气模型 ISA
    static f64 isaTemperature(f64 alt_m);
    static f64 isaPressure(f64 alt_m);
    static f64 isaAirDensity(f64 alt_m);

    // 热力学计算
    f64 computeSolarHeating(f64 solar_elev, f64 solar_irr, f64 wind_speed) const;
    f64 computeHeatExchange(f64 dt) const;

    // 超压保护：自动放气
    void checkOverpressure(f64 dt);

    // 状态更新
    void updateThermodynamics(const EnvInput& env);
    void updateBuoyancy();
};

} // namespace FlyteOS::Power
