/**
 * @file solar_membrane.cpp
 * @brief 太阳能膜能量模型实现
 *
 * 关键公式：
 *   P_net = η·I·A·cos(θ)·f_atm·f_T·f_shade·η_mppt·(1-loss)·A_membrane
 */

#include "solar_membrane.hpp"
#include <algorithm>

namespace FlyteOS::Power {

SolarMembrane::SolarMembrane(Config cfg) : cfg_(cfg) {}

// ════════════════════════════════════════════════════════════════
//  主更新（完整太阳位置）
// ════════════════════════════════════════════════════════════════

SolarMembrane::State SolarMembrane::update(
    f64 sun_elev_rad, f64 sun_azimuth_rad,
    f64 air_temp_k, f64 dt)
{
    if (dt <= 0) return state_;

    // ── 1. 太阳位置 ──
    state_.sun.elevation_rad = sun_elev_rad;
    state_.sun.azimuth_rad = sun_azimuth_rad;
    state_.is_day = sun_elev_rad > 0;

    if (!state_.is_day) {
        // 夜间零发电
        state_.power_gross_w = 0;
        state_.power_net_w = 0;
        state_.ts = now_us();
        return state_;
    }

    // ── 2. 大气质量数 → 衰减 ──
    // AM = 1/sin(elev) (Kasten-Young简化)
    f64 sin_elev = sin(sun_elev_rad);
    if (sin_elev < 0.01) sin_elev = 0.01;  // 防止除零
    state_.sun.airmass = 1.0 / sin_elev;
    if (state_.sun.airmass > 38) state_.sun.airmass = 38;  // 极低仰角上限

    state_.atm_factor = airmassToAtmFactor(state_.sun.airmass);

    // ── 3. 地面辐照度 ──
    // I = I0 × f_atm (太阳常数1361 W/m² × 大气透过率)
    f64 I0 = 1361.0;
    state_.sun.irradiance_wm2 = I0 * state_.atm_factor;

    // ── 4. 入射角余弦 ──
    state_.cos_theta = incidenceCosine(sun_elev_rad, sun_azimuth_rad);
    state_.cos_theta = std::max(0.0, state_.cos_theta);

    // ── 5. 膜面接收辐照度 ──
    state_.membrane_irr_wm2 = state_.sun.irradiance_wm2 * state_.cos_theta;

    // ── 6. 温度修正 ──
    // 电池温度 ≈ 环境温度 + 辐照温升
    f64 cell_temp_k = air_temp_k + state_.membrane_irr_wm2 * 0.03;  // 简化：30°C/kW温升
    state_.temp_factor = temperatureDerate(cell_temp_k);

    // ── 7. 综合效率 ──
    state_.effective_efficiency = cfg_.cell_efficiency *
                                  state_.temp_factor *
                                  cfg_.shading_factor;

    // ── 8. 毛发电功率 ──
    state_.power_gross_w = state_.effective_efficiency *
                           state_.membrane_irr_wm2 *
                           cfg_.membrane_area_m2;

    // ── 9. 净发电功率（扣除MPPT损耗和线缆损耗）──
    state_.power_net_w = state_.power_gross_w *
                         cfg_.mppt_efficiency *
                         (1.0 - cfg_.cable_loss_pct);

    // ── 10. 累计发电量 ──
    state_.energy_wh += state_.power_net_w * dt / 3600.0;

    state_.ts = now_us();
    return state_;
}

// ════════════════════════════════════════════════════════════════
//  简化接口
// ════════════════════════════════════════════════════════════════

SolarMembrane::State SolarMembrane::updateWithIrradiance(
    f64 sun_elev_rad, f64 irradiance_wm2,
    f64 air_temp_k, f64 dt)
{
    // 从已知辐照度反推方位角（简化：假设正南）
    State s = update(sun_elev_rad, 0.0, air_temp_k, dt);
    // 用实测辐照度覆盖计算值
    if (s.is_day) {
        state_.sun.irradiance_wm2 = irradiance_wm2;
        state_.membrane_irr_wm2 = irradiance_wm2 * state_.cos_theta;
        state_.power_gross_w = state_.effective_efficiency *
                               state_.membrane_irr_wm2 *
                               cfg_.membrane_area_m2;
        state_.power_net_w = state_.power_gross_w *
                              cfg_.mppt_efficiency *
                              (1.0 - cfg_.cable_loss_pct);
        state_.energy_wh += (state_.power_net_w - s.power_net_w) * dt / 3600.0;
    }
    return state_;
}

// ════════════════════════════════════════════════════════════════
//  太阳位置计算（简化日轨模型）
// ════════════════════════════════════════════════════════════════

SolarMembrane::SunPosition SolarMembrane::computeSunPosition(
    f64 hour_of_day, f64 latitude_deg, f64 day_of_year)
{
    SunPosition sun;

    // 赤纬角 (Cooper方程)
    f64 dec_rad = 23.45 * sin(2.0 * M_PI / 365.0 * (284 + day_of_year)) * M_PI / 180.0;

    // 时角 (正午=0, 上午为负, 下午为正)
    f64 lat_rad = latitude_deg * M_PI / 180.0;
    f64 hour_angle = (hour_of_day - 12.0) * 15.0 * M_PI / 180.0;  // 15°/h

    // 仰角: sin(elev) = sin(lat)·sin(dec) + cos(lat)·cos(dec)·cos(h)
    f64 sin_elev = sin(lat_rad) * sin(dec_rad) +
                   cos(lat_rad) * cos(dec_rad) * cos(hour_angle);
    sun.elevation_rad = asin(std::clamp(sin_elev, -1.0, 1.0));

    // 方位角
    if (cos(sun.elevation_rad) > 0.001) {
        f64 cos_az = (sin(dec_rad) - sin(lat_rad) * sin_elev) /
                     (cos(lat_rad) * cos(sun.elevation_rad));
        cos_az = std::clamp(cos_az, -1.0, 1.0);
        sun.azimuth_rad = acos(cos_az);
        if (hour_angle > 0) sun.azimuth_rad = 2 * M_PI - sun.azimuth_rad;  // 下午
    }

    // 辐照度
    f64 sin_e = sin(sun.elevation_rad);
    if (sin_e > 0.01) {
        sun.airmass = 1.0 / sin_e;
        if (sun.airmass > 38) sun.airmass = 38;
        sun.irradiance_wm2 = 1361.0 * airmassToAtmFactor(sun.airmass);
    } else {
        sun.airmass = 0;
        sun.irradiance_wm2 = 0;
    }

    return sun;
}

// ════════════════════════════════════════════════════════════════
//  辅助函数
// ════════════════════════════════════════════════════════════════

f64 SolarMembrane::airmassToAtmFactor(f64 am) {
    // Meinel模型：大气透过率
    // T = 0.7^(AM^0.678)
    if (am <= 0) return 0;
    return pow(0.7, pow(am, 0.678));
}

f64 SolarMembrane::temperatureDerate(f64 cell_temp_k) const {
    // 温度系数：每升高1°C效率降低temp_coeff_ppk
    f64 delta_k = cell_temp_k - cfg_.ref_temp_k;
    f64 derate = 1.0 - cfg_.temp_coeff_ppk * delta_k;
    return std::max(0.0, std::min(1.2, derate));  // 低温时效率可略高
}

f64 SolarMembrane::incidenceCosine(f64 sun_elev, f64 sun_azimuth) const {
    // 入射角：太阳方向与膜法线的夹角
    // 膜法线方向由tilt和azimuth定义
    f64 tilt = cfg_.membrane_tilt_rad;
    f64 facing = cfg_.membrane_azimuth_rad;

    // 太阳方向向量 (仰角→笛卡尔)
    f64 sx = cos(sun_elev) * sin(sun_azimuth);
    f64 sy = cos(sun_elev) * cos(sun_azimuth);
    f64 sz = sin(sun_elev);

    // 膜法线向量 (倾斜+朝向)
    f64 nx = sin(tilt) * sin(facing);
    f64 ny = sin(tilt) * cos(facing);
    f64 nz = cos(tilt);

    // 点积 = cos(入射角)
    return sx * nx + sy * ny + sz * nz;
}

} // namespace FlyteOS::Power
