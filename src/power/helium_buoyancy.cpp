/**
 * @file helium_buoyancy.cpp
 * @brief 氦气浮力系统实现
 */
#include "helium_buoyancy.hpp"
#include <cmath>

namespace FlyteOS::Power {

HeliumBuoyancy::HeliumBuoyancy(Config cfg) : cfg_(cfg) {}

// 空气密度 kg/m³（理想气体近似）
f32 HeliumBuoyancy::air_density(f32 temp_c, f32 pressure_hpa) {
    const f32 R_air = 287.05f;   // J/(kg·K)
    f32 T = temp_c + 273.15f;
    f32 P = pressure_hpa * 100.0f; // Pa
    return P / (R_air * T);
}

// 浮力 = ρ_air × V × g - ρ_He × V × g
f32 HeliumBuoyancy::buoyancy_force_n(f32 vol_m3, f32 he_temp_c, f32 amb_temp_c, f32 amb_pressure_hpa) {
    const f32 g      = 9.81f;
    const f32 R_he   = 2077.1f;  // J/(kg·K)

    f32 rho_air = air_density(amb_temp_c, amb_pressure_hpa);
    f32 P_pa    = amb_pressure_hpa * 100.0f;
    f32 T_he    = he_temp_c + 273.15f;
    f32 rho_he  = P_pa / (R_he * T_he);

    return (rho_air - rho_he) * vol_m3 * g;
}

HeliumBuoyancy::Status HeliumBuoyancy::compute_status(const Sensors& s) const {
    f32 lift_n  = buoyancy_force_n(s.envelope_vol_m3, s.he_temp_c, s.amb_temp_c, s.amb_pressure_hpa);
    f32 weight_n = cfg_.aircraft_mass_kg * 9.81f;
    f32 net_kg  = (lift_n - weight_n) / 9.81f;

    Status st;
    st.lift_force_n  = lift_n;
    st.weight_n      = weight_n;
    st.net_buoyancy_kg = net_kg;
    st.buoyancy_ratio  = lift_n / (weight_n + 1e-6f);
    st.mode            = mode_;

    // 安全检查
    st.safe_to_fly = true;
    if (s.he_pressure_atm > cfg_.pressure_alarm_atm) {
        st.warnings.push_back("HIGH_PRESSURE: " + std::to_string(s.he_pressure_atm) + " atm");
        st.safe_to_fly = false;
    }
    if (s.altitude_m > cfg_.max_altitude_m) {
        st.warnings.push_back("ALTITUDE_EXCEEDED");
        st.safe_to_fly = false;
    }
    if (net_kg < cfg_.min_buoyancy_margin_kg && mode_ != Mode::EMERGENCY_DUMP) {
        st.warnings.push_back("LOW_BUOYANCY_MARGIN");
    }
    return st;
}

HeliumBuoyancy::ValveCmd HeliumBuoyancy::update(const Sensors& s) {
    ValveCmd cmd;
    cmd.status = compute_status(s);

    switch (mode_) {
    case Mode::AUTO_BALANCE: {
        // 根据净浮力与目标值的偏差调整阀门
        f32 error = cfg_.target_net_buoyancy_kg - cmd.status.net_buoyancy_kg;
        if (error > 0.5f) {
            // 浮力不足 → 充气
            cmd.valve_open[0] = true;
            cmd.valve_pos[0]  = std::min(1.0f, error / 5.0f);
        } else if (error < -0.5f) {
            // 浮力过大 → 放气
            cmd.valve_open[1] = true;
            cmd.valve_pos[1]  = std::min(1.0f, -error / 5.0f);
        }
        break;
    }
    case Mode::EMERGENCY_DUMP:
        for (int i = 0; i < 4; i++) { cmd.valve_open[i] = true; cmd.valve_pos[i] = 1.0f; }
        break;
    case Mode::ALTITUDE_HOLD:
        // 根据高度误差微调
        if (s.altitude_m > 50.0f) {
            cmd.valve_open[1] = true;
            cmd.valve_pos[1]  = 0.2f;
        }
        break;
    default:
        break;
    }
    return cmd;
}

HeliumBuoyancy::ValveCmd HeliumBuoyancy::set_mode(Mode m) {
    mode_ = m;
    Sensors dummy{};
    return update(dummy);
}

HeliumBuoyancy::ValveCmd HeliumBuoyancy::emergency_dump() {
    return set_mode(Mode::EMERGENCY_DUMP);
}

} // namespace FlyteOS::Power
