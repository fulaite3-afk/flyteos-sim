/**
 * @file thermal_predictor.cpp
 * @brief 温度预测模块实现
 *
 * 前向欧拉积分求解气囊热平衡方程，输出多时间尺度的温度预测。
 */

#include "thermal_predictor.hpp"
#include <algorithm>
#include <cmath>

namespace FlyteOS::Power {

ThermalPredictor::ThermalPredictor(Config cfg) : cfg_(cfg) {}

// ════════════════════════════════════════════════════════════════
//  热源计算
// ════════════════════════════════════════════════════════════════

f64 ThermalPredictor::qSolar(f64 solar_elev, f64 solar_irr, f64 wind) const {
    if (solar_elev <= 0) return 0;  // 太阳在地平线以下无加热
    f64 q = cfg_.solar_absorptivity * solar_irr * cfg_.envelope_area_m2 * sin(solar_elev);
    // 风冷效应：风速越大，对流冷却越强，等效太阳加热减弱
    f64 wind_factor = 1.0 / (1.0 + 0.05 * wind);
    return q * wind_factor;
}

f64 ThermalPredictor::qConv(f64 temp_k, f64 air_temp, f64 wind) const {
    // 对流换热：Q = h·A·(T_air - T_He)
    // 风速增大→对流系数增大
    f64 h = cfg_.h_conv_wm2k * (1.0 + 0.5 * wind);  // 简化：h随风速线性增加
    return h * cfg_.envelope_area_m2 * (air_temp - temp_k);
}

f64 ThermalPredictor::qRad(f64 temp_k, f64 air_temp) const {
    // 辐射换热：Q = ε·σ·A·(T_air⁴ - T_He⁴)
    f64 sigma = cfg_.stefan_boltzmann;
    return cfg_.emissivity * sigma * cfg_.envelope_area_m2 *
           (pow(air_temp, 4) - pow(temp_k, 4));
}

f64 ThermalPredictor::qVent(f64 vent_flow, f64 air_temp, f64 temp_k) const {
    // 放气带走热量（氦气带走内能）
    if (vent_flow <= 0) return 0;
    return vent_flow * cfg_.cp_he_jkgk * (air_temp - temp_k);
}

f64 ThermalPredictor::qPump(f64 pump_flow, f64 air_temp, f64 temp_k) const {
    // 补气带入环境温度的气体
    if (pump_flow <= 0) return 0;
    return pump_flow * cfg_.cp_he_jkgk * (air_temp - temp_k);
}

// ════════════════════════════════════════════════════════════════
//  单步积分
// ════════════════════════════════════════════════════════════════

f64 ThermalPredictor::stepThermal(f64 temp_k, const EnvInput& env, f64 dt) const {
    f64 q_solar = qSolar(env.solar_elev_rad, env.solar_irr_wm2, env.wind_speed_ms);
    f64 q_conv  = qConv(temp_k, env.air_temp_k, env.wind_speed_ms);
    f64 q_rad   = qRad(temp_k, env.air_temp_k);
    f64 q_vent  = qVent(env.vent_flow_kgs, env.air_temp_k, temp_k);
    f64 q_pump  = qPump(env.pump_flow_kgs, env.air_temp_k, temp_k);

    f64 q_total = q_solar + q_conv + q_rad + q_vent + q_pump;

    // dT/dt = Q_total / (m · Cp)
    f64 dTdt = q_total / (cfg_.he_mass_kg * cfg_.cp_he_jkgk);
    return temp_k + dTdt * dt;
}

// ════════════════════════════════════════════════════════════════
//  多步预测
// ════════════════════════════════════════════════════════════════

ThermalPredictor::Summary ThermalPredictor::predict(const EnvInput& env) {
    Summary sum;
    sum.temp_now_k = env.current_temp_k;

    predictions_.clear();
    f64 temp = env.current_temp_k;
    f64 max_temp = temp;
    f64 min_temp = temp;

    i32 steps = cfg_.predict_horizon_s / static_cast<i32>(cfg_.predict_step_s);

    for (i32 i = 0; i < steps; i++) {
        f64 t = (i + 1) * cfg_.predict_step_s;
        temp = stepThermal(temp, env, cfg_.predict_step_s);

        if (temp > max_temp) max_temp = temp;
        if (temp < min_temp) min_temp = temp;

        // 记录关键时间点
        Prediction p;
        p.time_s = t;
        p.predicted_temp_k = temp;
        p.delta_temp_k = temp - env.current_temp_k;
        p.q_solar_w = qSolar(env.solar_elev_rad, env.solar_irr_wm2, env.wind_speed_ms);
        p.q_conv_w = qConv(temp, env.air_temp_k, env.wind_speed_ms);
        p.q_rad_w = qRad(temp, env.air_temp_k);
        predictions_.push_back(p);

        // 记录关键时间点
        f64 t_f = t;
        if (std::fabs(t_f - 30.0) < cfg_.predict_step_s) sum.temp_30s_k = temp;
        if (std::fabs(t_f - 60.0) < cfg_.predict_step_s) sum.temp_1min_k = temp;
        if (std::fabs(t_f - 300.0) < cfg_.predict_step_s) sum.temp_5min_k = temp;
    }

    sum.max_temp_k = max_temp;
    sum.min_temp_k = min_temp;
    sum.trend_kps = (predictions_.empty()) ? 0 :
                    (predictions_.back().predicted_temp_k - env.current_temp_k) /
                    cfg_.predict_horizon_s;
    sum.heating = sum.trend_kps > 0.001;
    sum.overheat_risk = max_temp > 333.15;   // >60°C
    sum.superheat_risk = max_temp > 353.15;  // >80°C

    return sum;
}

} // namespace FlyteOS::Power
