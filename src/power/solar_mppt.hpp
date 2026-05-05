#pragma once
/**
 * @file solar_mppt.hpp
 * @brief 太阳能最大功率点追踪控制器
 *        改进型扰动观察法（P&O）+ 温度补偿
 */
#include "../../include/flyteos_types.hpp"

namespace FlyteOS::Power {

class SolarMPPT {
public:
    enum class State : u8 {
        INIT,
        TRACKING,       // 正常追踪
        PARTIAL_SHADE,  // 部分遮阴
        TEMP_LIMIT,     // 温度限制
        EMERGENCY_LIMIT // 紧急限功率
    };

    struct Config {
        f32 perturb_step       = 0.5f;    // 电压扰动步长 V
        f32 deadband           = 0.1f;    // 死区电压 V
        f32 temp_coeff         = -0.0045f;// 功率温度系数 /°C
        f32 max_power_w        = 300.0f;  // 最大功率 W
        f32 panel_area_m2      = 2.0f;    // 面板面积 m²
        f32 panel_efficiency   = 0.22f;   // 面板效率
    };

    struct Reading {
        f32 voltage = 0;    // V
        f32 current = 0;    // A
        f32 temp    = 25.0f;// °C
        TimeUs ts   = 0;
        f32 power() const { return voltage * current; }
    };

    struct Output {
        bool    ok;
        f32     target_voltage;   // 建议工作电压
        f32     actual_power_w;   // 实际输出功率
        f32     efficiency_pct;   // 效率 %
        State   state;
        std::string diag;
    };

    explicit SolarMPPT(Config cfg);
    SolarMPPT() : SolarMPPT(Config{}) {}

    Output update(const Reading& now, const Reading& prev);
    f32    estimated_irradiance(f32 voltage, f32 current, f32 temp) const;
    State  state() const { return state_; }

private:
    Config cfg_;
    State  state_ = State::INIT;
    f32    duty_  = 0.5f;
    f32    best_v_ = 0, best_p_ = 0;

    Output perturbAndObserve(const Reading& now, const Reading& prev);
    f32    tempCompensate(f32 power, f32 temp) const;
};

} // namespace FlyteOS::Power
