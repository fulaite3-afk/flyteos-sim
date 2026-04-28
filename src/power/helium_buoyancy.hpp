#pragma once
/**
 * @file helium_buoyancy.hpp
 * @brief 氦气浮力系统控制器
 */
#include "../../include/flyteos_types.hpp"

namespace FlyteOS::Power {

class HeliumBuoyancy {
public:
    enum class Mode : u8 {
        AUTO_BALANCE,
        ALTITUDE_HOLD,
        EMERGENCY_DUMP,
        INFLATION,
        MANUAL
    };

    struct Config {
        f32 target_net_buoyancy_kg  = 5.0f;   // 目标净浮力 kg
        f32 min_buoyancy_margin_kg  = 2.0f;
        f32 max_altitude_m          = 3000.0f;
        f32 pressure_alarm_atm      = 1.25f;
        f32 temp_coefficient        = 0.0036f; // /K
        f32 aircraft_mass_kg        = 12.0f;   // 飞行器自重
    };

    struct Sensors {
        f32 he_pressure_atm  = 1.0f;
        f32 he_temp_c        = 20.0f;
        f32 amb_pressure_hpa = 1013.25f;
        f32 amb_temp_c       = 20.0f;
        f32 altitude_m       = 0.0f;
        f32 he_mass_kg       = 0.05f;   // 当前氦气质量
        f32 envelope_vol_m3  = 2.5f;    // 气囊体积
    };

    struct Status {
        f32  net_buoyancy_kg;    // 净浮力 kg
        f32  lift_force_n;       // 升力 N
        f32  weight_n;           // 重力 N
        f32  buoyancy_ratio;     // 浮力/重力 比
        Mode mode;
        bool safe_to_fly;
        std::vector<std::string> warnings;
    };

    struct ValveCmd {
        bool   valve_open[4] = {};      // 四个阀门
        f32    valve_pos[4]  = {};      // 开度 0~1
        Status status;
    };

    explicit HeliumBuoyancy(Config cfg = {}) : cfg_(cfg) {}

    ValveCmd update(const Sensors& s);
    ValveCmd set_mode(Mode m);
    ValveCmd emergency_dump();
    Status   compute_status(const Sensors& s) const;

    // 物理计算
    static f32 buoyancy_force_n(f32 vol_m3, f32 he_temp_c, f32 amb_temp_c, f32 amb_pressure_hpa);
    static f32 air_density(f32 temp_c, f32 pressure_hpa);

private:
    Config cfg_;
    Mode   mode_ = Mode::AUTO_BALANCE;
    bool   valves_[4] = {};
    f32    valve_pos_[4] = {};
};

} // namespace FlyteOS::Power
