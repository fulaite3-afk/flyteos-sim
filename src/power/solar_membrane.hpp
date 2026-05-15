#pragma once
/**
 * @file solar_membrane.hpp
 * @brief 太阳能膜能量模型 —— 计算柔性薄膜在不同日照条件下的发电功率
 *
 * 物理模型：
 *   P = η·I·A·cos(θ)·f(α)·f(T)·f(shading)
 *
 *   η:       电池效率 (薄膜8~12%, 刚性18~22%)
 *   I:       太阳辐照度 W/m² (AM0=1361, AM1.5≈1000)
 *   A:       膜面积 m²
 *   θ:       太阳光入射角 (与膜法线的夹角)
 *   f(α):    大气衰减 (仰角越低衰减越大)
 *   f(T):    温度系数 (每升高1°C效率降低0.3~0.5%)
 *   f(shading): 阴影遮挡因子 0~1
 *
 * 太阳位置计算：
 *   基于经纬度+时间，简化模型：
 *   - 仰角: 取决于时角和赤纬
 *   - 方位角: 取决于时角和赤纬
 */

#include "../../include/flyteos_types.hpp"
#include <cmath>

namespace FlyteOS::Power {

class SolarMembrane {
public:
    struct Config {
        f64 membrane_area_m2    = 6.0;       // 膜面积 m²
        f64 cell_efficiency     = 0.10;      // 电池效率10% (薄膜)
        f64 temp_coeff_ppk      = 0.004;     // 温度系数 -0.4%/°C
        f64 ref_temp_k          = 298.15;    // 参考温度25°C
        f64 membrane_tilt_rad   = 0.1745;    // 膜倾斜角10° (浮空器顶部)
        f64 membrane_azimuth_rad = 0;        // 膜朝向 (0=南, π/2=西)

        // MPPT参数
        f64 mppt_efficiency     = 0.95;      // MPPT变换器效率
        f64 cable_loss_pct      = 0.02;      // 线缆损耗2%

        // 局部遮挡
        f64 shading_factor      = 1.0;       // 阴影遮挡因子 0~1 (1=无遮挡)
    };

    /// 太阳位置
    struct SunPosition {
        f64 elevation_rad = 0;     // 仰角 rad (0=地平线, π/2=天顶)
        f64 azimuth_rad   = 0;     // 方位角 rad (0=南, π/2=西)
        f64 irradiance_wm2 = 0;    // 地面辐照度 W/m²
        f64 airmass       = 0;     // 大气质量数 AM
    };

    /// 能量状态
    struct State {
        // 发电
        f64 power_gross_w    = 0;    // 毛发电功率 W
        f64 power_net_w      = 0;    // 净发电功率 W (扣除损耗)
        f64 energy_wh        = 0;    // 累计发电量 Wh

        // 太阳位置
        SunPosition sun;

        // 效率因子
        f64 cos_theta        = 0;    // 入射角余弦
        f64 atm_factor       = 0;    // 大气衰减因子
        f64 temp_factor      = 0;    // 温度效率因子
        f64 effective_efficiency = 0; // 综合效率

        // 膜面辐照
        f64 membrane_irr_wm2 = 0;   // 膜面接收辐照度 W/m²

        bool is_day           = false; // 是否白天(太阳仰角>0)
        TimeUs ts             = 0;
    };

    explicit SolarMembrane(Config cfg);
    SolarMembrane() : SolarMembrane(Config{}) {}

    /// 主更新
    /// @param sun_elev_rad  太阳仰角 rad
    /// @param sun_azimuth_rad 太阳方位角 rad
    /// @param air_temp_k    环境温度 K
    /// @param dt            时间步长 s
    State update(f64 sun_elev_rad, f64 sun_azimuth_rad,
                 f64 air_temp_k, f64 dt);

    /// 简化接口：传仰角和实测辐照度（跳过大气模型）
    State updateWithIrradiance(f64 sun_elev_rad, f64 irradiance_wm2, f64 air_temp_k, f64 dt);

    /// 计算太阳位置（简化日轨模型）
    /// @param hour_of_day  当前小时 (0~24)
    /// @param latitude_deg 纬度 (北半球为正)
    /// @param day_of_year  一年中的第几天 (1~365)
    static SunPosition computeSunPosition(f64 hour_of_day, f64 latitude_deg, f64 day_of_year);

    /// 设置阴影遮挡因子
    void setShading(f64 factor) { cfg_.shading_factor = std::clamp(factor, 0.0, 1.0); }

    const State& state() const { return state_; }
    const Config& config() const { return cfg_; }
    void setConfig(const Config& c) { cfg_ = c; }

private:
    Config cfg_;
    State  state_;

    /// 大气质量数 → 衰减因子
    static f64 airmassToAtmFactor(f64 am);

    /// 温度效率修正
    f64 temperatureDerate(f64 cell_temp_k) const;

    /// 入射角余弦
    f64 incidenceCosine(f64 sun_elev, f64 sun_azimuth) const;
};

} // namespace FlyteOS::Power
