/**
 * @file solar_mppt.cpp
 * @brief 太阳能MPPT控制器实现
 */
#include "solar_mppt.hpp"
#include <cmath>

namespace FlyteOS::Power {

SolarMPPT::Output SolarMPPT::update(const Reading& now, const Reading& prev) {
    if (state_ == State::INIT) {
        best_v_ = now.voltage;
        best_p_ = now.power();
        state_  = State::TRACKING;
    }

    // 温度补偿功率
    f32 p_now  = tempCompensate(now.power(),  now.temp);
    f32 p_prev = tempCompensate(prev.power(), prev.temp);

    // 紧急限功率
    if (p_now > cfg_.max_power_w * 1.1f) {
        state_ = State::EMERGENCY_LIMIT;
        return { true, now.voltage * 0.9f, p_now, 0.0f, state_, "Emergency power limit" };
    }

    // 部分遮阴检测：功率抖动幅度大
    if (std::fabs(p_now - p_prev) > cfg_.max_power_w * 0.3f) {
        state_ = State::PARTIAL_SHADE;
    } else {
        state_ = State::TRACKING;
    }

    return perturbAndObserve(now, prev);
}

SolarMPPT::Output SolarMPPT::perturbAndObserve(const Reading& now, const Reading& prev) {
    f32 dp = now.power() - prev.power();
    f32 dv = now.voltage - prev.voltage;

    f32 target_v = now.voltage;

    if (std::fabs(dv) < cfg_.deadband) {
        // 电压无变化：维持
    } else if (dp > 0) {
        // 功率增加：继续同向扰动
        target_v = now.voltage + (dv > 0 ? cfg_.perturb_step : -cfg_.perturb_step);
    } else {
        // 功率减少：反向扰动
        target_v = now.voltage + (dv > 0 ? -cfg_.perturb_step : cfg_.perturb_step);
    }

    // 记录最佳工作点
    if (now.power() > best_p_) {
        best_p_ = now.power();
        best_v_ = now.voltage;
    }

    f32 eff = (cfg_.panel_area_m2 > 0 && now.voltage > 1.0f)
              ? (now.power() / (cfg_.panel_area_m2 * 1000.0f)) * 100.0f
              : 0.0f;

    return {
        true, target_v, now.power(), eff, state_,
        state_ == State::PARTIAL_SHADE ? "Partial shade detected" : "Normal tracking"
    };
}

f32 SolarMPPT::tempCompensate(f32 power, f32 temp) const {
    return power * (1.0f + cfg_.temp_coeff * (temp - 25.0f));
}

f32 SolarMPPT::estimated_irradiance(f32 voltage, f32 current, f32 temp) const {
    f32 p = tempCompensate(voltage * current, temp);
    return p / (cfg_.panel_area_m2 * cfg_.panel_efficiency + 1e-6f);
}

} // namespace FlyteOS::Power
