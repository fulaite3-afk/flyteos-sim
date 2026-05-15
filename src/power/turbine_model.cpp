/**
 * @file turbine_model.cpp
 * @brief 特斯拉电动涡轮动力模型实现
 *
 * 核心计算链路：
 *   油门 → 目标RPM → 实际RPM(一阶延迟) → 推力/扭矩(螺旋桨公式) → 电流/功率(电机方程) → 电池消耗
 */

#include "turbine_model.hpp"
#include <algorithm>

namespace FlyteOS::Power {

// ════════════════════════════════════════════════════════════════
//  构造/初始化
// ════════════════════════════════════════════════════════════════

TurbineModel::TurbineModel(Config cfg) : cfg_(cfg) {}

void TurbineModel::initialize(f32 battery_pct) {
    state_ = {};
    rpm_filtered_ = 0;
    battery_soc_ = std::clamp(battery_pct / 100.0f, 0.0f, 1.0f);
    battery_v_oc_ = estimateOpenCircuitVoltage(battery_soc_);
    state_.battery_voltage_v = battery_v_oc_;
    state_.battery_remaining_pct = battery_pct;
    printf("[Turbine] Initialized: KV=%.0f, D=%.0fmm, %dS %.0fmAh\n",
           cfg_.motor_kv, cfg_.prop_diameter_m * 1000,
           (int)cfg_.battery_cells, cfg_.battery_capacity_mah);
}

// ════════════════════════════════════════════════════════════════
//  主更新
// ════════════════════════════════════════════════════════════════

TurbineModel::State TurbineModel::update(f32 throttle_01, f32 battery_v, f32 dt) {
    if (dt <= 0) return state_;

    throttle_01 = std::clamp(throttle_01, 0.0f, 1.0f);
    state_.throttle_01 = throttle_01;

    // ── 1. 电池电压 ──
    if (battery_v > 0) {
        state_.battery_voltage_v = battery_v;
    } else {
        // 使用内置电池模型
        state_.battery_voltage_v = battery_v_oc_;
    }

    // ── 2. 油门 → 目标RPM ──
    // KV值：每伏特转速，线性映射
    f32 v_bus = state_.battery_voltage_v;
    f32 no_load_rpm = cfg_.motor_kv * v_bus;
    state_.target_rpm = throttle_01 * no_load_rpm;
    state_.target_rpm = std::min(state_.target_rpm, cfg_.max_rpm);

    // ── 3. 推力响应延迟（一阶低通）──
    f32 tau = cfg_.thrust_tau_s;
    f32 alpha = dt / (tau + dt);
    rpm_filtered_ += alpha * (state_.target_rpm - rpm_filtered_);
    state_.actual_rpm = std::max(0.0f, rpm_filtered_);

    // ── 4. RPM → 角速度 ──
    state_.omega_rads = state_.actual_rpm * 2.0f * (f32)M_PI / 60.0f;

    // ── 5. 螺旋桨推力/扭矩（叶素理论简化）──
    // T = CT × ρ × n² × D⁴
    // Q = CQ × ρ × n² × D⁵
    // n = RPM / 60 (转/秒)
    f32 n_rps = state_.actual_rpm / 60.0f;
    f32 D = cfg_.prop_diameter_m;
    f32 rho = cfg_.rho_kgm3;

    state_.thrust_n = cfg_.ct_coeff * rho * n_rps * n_rps *
                      D * D * D * D;
    state_.torque_nm = cfg_.cq_coeff * rho * n_rps * n_rps *
                       D * D * D * D * D;

    // ── 6. 电机电气 ──
    // 反电动势: Vemf = Ke × ω
    f32 v_emf = cfg_.motor_ke * state_.omega_rads;
    // 端电压: min(电池电压, 反电动势+余量)
    state_.motor_voltage_v = std::min(v_bus, v_emf + cfg_.motor_r_ohm * 30.0f);
    // 电流: I = (V - Vemf) / R
    f32 v_diff = state_.motor_voltage_v - v_emf;
    state_.motor_current_a = std::max(0.0f, v_diff / cfg_.motor_r_ohm);

    // ── 7. 功率计算 ──
    state_.power_elec_w = state_.motor_voltage_v * state_.motor_current_a;
    state_.power_mech_w = state_.torque_nm * state_.omega_rads;

    // ── 8. 效率 ──
    if (state_.power_elec_w > 0.1f) {
        // 推进效率 = 推力功率 / 电功率
        // 推力功率 = T × V (简化：悬停时V≈0，使用机械功率替代)
        state_.efficiency_pct = std::min(100.0f,
            state_.power_mech_w / state_.power_elec_w * 100.0f);
    } else {
        state_.efficiency_pct = 0;
    }

    // ── 9. 电池消耗 ──
    updateBatteryModel(state_.motor_current_a, dt);

    state_.ts = now_us();
    return state_;
}

// ════════════════════════════════════════════════════════════════
//  电池放电模型
// ════════════════════════════════════════════════════════════════

void TurbineModel::updateBatteryModel(f32 current_a, f32 dt) {
    // 库仑计数法
    // ΔmAh = I(A) × Δt(s) / 3.6
    f32 d_mah = current_a * dt / 3.6f;
    state_.consumed_mah += d_mah;

    f32 total_mah = cfg_.battery_capacity_mah;
    if (total_mah > 0) {
        battery_soc_ = std::max(0.0f, 1.0f - state_.consumed_mah / total_mah);
    }

    // 更新开路电压（简化线性+内阻压降）
    battery_v_oc_ = estimateOpenCircuitVoltage(battery_soc_);

    // 端电压 = 开路电压 - 内阻压降
    f32 v_terminal = battery_v_oc_ - current_a * cfg_.battery_ir_ohm;
    v_terminal = std::max(cfg_.battery_v_min, v_terminal);

    state_.battery_voltage_v = v_terminal;
    state_.battery_remaining_pct = battery_soc_ * 100.0f;
}

f32 TurbineModel::estimateOpenCircuitVoltage(f32 soc) const {
    // 简化锂电池放电曲线：3.5V(空) ~ 4.2V(满) per cell
    // 线性近似（实际为中段平坦曲线，后续可替换为查找表）
    f32 v_cell = 3.5f + soc * 0.7f;  // 3.5~4.2V
    return v_cell * cfg_.battery_cells;
}

} // namespace FlyteOS::Power
