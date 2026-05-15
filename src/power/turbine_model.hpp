#pragma once
/**
 * @file turbine_model.hpp
 * @brief 特斯拉电动涡轮动力模型 —— 无刷电机+螺旋桨推力系统
 *
 * 设计依据：
 *   - 浮空器推力需求远小于多旋翼（浮力承担90%+升力）
 *   - 推力主要用于：姿态控制、水平机动、抗风
 *   - 电机效率曲线：中等转速最优
 *   - 电池放电特性影响最大可用推力
 *
 * 物理模型：
 *   推力 = CT × ρ × n² × D⁴    (螺旋桨推力系数)
 *   扭矩 = CQ × ρ × n² × D⁵    (螺旋桨扭矩系数)
 *   电机: V = R×I + Ke×ω        (电机方程)
 *   效率: η = P_thrust / P_elec  (推进效率)
 */

#include "../../include/flyteos_types.hpp"
#include <cmath>

namespace FlyteOS::Power {

class TurbineModel {
public:
    /// 电机/螺旋桨配置
    struct Config {
        // 螺旋桨参数
        f32 prop_diameter_m   = 0.254f;    // 螺旋桨直径 m (10寸)
        f32 prop_pitch_m      = 0.114f;    // 螺旋桨螺距 m (4.5寸)
        f32 ct_coeff          = 0.1f;      // 推力系数 CT (静态)
        f32 cq_coeff          = 0.006f;    // 扭矩系数 CQ

        // 电机参数
        f32 motor_kv          = 920.0f;    // 电机KV值 RPM/V
        f32 motor_r_ohm       = 0.08f;     // 绕组电阻 Ω
        f32 motor_ke          = 0.0104f;   // 反电动势常数 V/(rad/s) ≈ 60/(2π×KV)
        f32 motor_inertia     = 1.5e-5f;   // 转子惯量 kg·m²
        f32 max_rpm           = 15000.0f;  // 最大转速 RPM

        // 电池参数
        f32 battery_cells     = 6;          // 6S
        f32 battery_v_nom     = 22.2f;     // 标称电压 V
        f32 battery_v_max     = 25.2f;     // 满电电压 V
        f32 battery_v_min     = 19.8f;     // 最低放电电压 V
        f32 battery_capacity_mah = 5000.0f; // 电池容量 mAh
        f32 battery_ir_ohm   = 0.02f;      // 电池内阻 Ω

        // 涡轮推力响应（一阶延迟）
        f32 thrust_tau_s      = 0.05f;      // 推力响应时间常数 s

        // 空气密度
        f32 rho_kgm3          = 1.225f;     // 空气密度 kg/m³
    };

    /// 涡轮状态
    struct State {
        // 输入
        f32 throttle_01       = 0;         // 油门指令 0~1

        // 电机状态
        f32 target_rpm        = 0;         // 目标转速 RPM
        f32 actual_rpm        = 0;         // 实际转速 RPM (含响应延迟)
        f32 omega_rads        = 0;         // 角速度 rad/s

        // 推力输出
        f32 thrust_n          = 0;         // 推力 N
        f32 torque_nm         = 0;         // 扭矩 Nm

        // 电气
        f32 motor_voltage_v   = 0;         // 电机端电压 V
        f32 motor_current_a   = 0;         // 电机电流 A
        f32 battery_voltage_v = 22.2f;     // 电池当前电压 V
        f32 power_elec_w      = 0;         // 电功率 W
        f32 power_mech_w      = 0;         // 机械功率 W

        // 效率
        f32 efficiency_pct    = 0;         // 推进效率 %

        // 电池
        f32 battery_remaining_pct = 100;  // 电池剩余 %
        f32 consumed_mah       = 0;         // 已消耗 mAh

        TimeUs ts = 0;
    };

    explicit TurbineModel(Config cfg);
    TurbineModel() : TurbineModel(Config{}) {}

    /// 初始化
    void initialize(f32 battery_pct = 100.0f);

    /// 主更新
    /// @param throttle_01 油门指令 0~1
    /// @param battery_v   当前电池电压 V（0则使用内置模型）
    /// @param dt          时间步长 s
    State update(f32 throttle_01, f32 battery_v, f32 dt);

    /// 获取状态
    const State& state() const { return state_; }
    const Config& config() const { return cfg_; }

    /// 设置空气密度（随高度变化）
    void setAirDensity(f32 rho) { cfg_.rho_kgm3 = rho; }

private:
    Config cfg_;
    State  state_;

    // 推力响应一阶延迟
    f32 rpm_filtered_ = 0;

    // 电池放电模型
    f32 battery_soc_ = 1.0f;  // State of Charge 0~1
    f32 battery_v_oc_ = 22.2f; // 开路电压

    void updateBatteryModel(f32 current_a, f32 dt);
    f32 estimateOpenCircuitVoltage(f32 soc) const;
};

} // namespace FlyteOS::Power
