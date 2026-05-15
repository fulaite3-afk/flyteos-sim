#pragma once
/**
 * @file thermal_predictor.hpp
 * @brief 温度预测模块 —— 基于热力学的气囊温度趋势预测
 *
 * 核心物理：
 *   气囊热平衡方程：
 *     m_He·Cp·dT/dt = Q_solar + Q_conv + Q_rad + Q_vent + Q_pump
 *
 *   各热源：
 *     Q_solar = α·I·A·sin(elev) · 1/(1+0.05·v)     太阳辐射加热
 *     Q_conv  = h·A·(T_air - T_He)                    对流换热
 *     Q_rad   = ε·σ·A·(T_air⁴ - T_He⁴)              辐射换热
 *     Q_vent  = ṁ_vent·Cp·(T_air - T_He)             放气冷却
 *     Q_pump  = ṁ_pump·Cp·(T_air - T_He)             补气冷却
 *
 *   预测方法：前向欧拉积分，多步预测
 */

#include "../../include/flyteos_types.hpp"
#include <vector>
#include <cmath>

namespace FlyteOS::Power {

class ThermalPredictor {
public:
    struct Config {
        f64 he_mass_kg        = 0.2;       // 氦气质量 kg
        f64 cp_he_jkgk        = 5193.0;    // 氦气定压比热 J/(kg·K)
        f64 envelope_area_m2   = 12.0;      // 囊体表面积 m²
        f64 solar_absorptivity = 0.35;      // 太阳吸收率
        f64 emissivity         = 0.1;       // 红外发射率
        f64 h_conv_wm2k        = 5.0;       // 对流换热系数 W/(m²·K)
        f64 stefan_boltzmann   = 5.67e-8;   // 斯特番-玻尔兹曼常数

        // 预测参数
        f64 predict_step_s    = 1.0;        // 预测步长 s
        i32 predict_horizon_s = 300;        // 预测时域 s (默认5分钟)
    };

    /// 环境输入
    struct EnvInput {
        f64 current_temp_k    = 288.15;     // 当前气囊温度 K
        f64 air_temp_k        = 288.15;     // 环境温度 K
        f64 wind_speed_ms     = 0;          // 风速 m/s
        f64 solar_elev_rad    = 0;           // 太阳仰角 rad
        f64 solar_irr_wm2     = 0;           // 太阳辐照度 W/m²
        f64 vent_flow_kgs     = 0;           // 放气质量流量 kg/s
        f64 pump_flow_kgs     = 0;           // 补气质量流量 kg/s
    };

    /// 预测结果
    struct Prediction {
        f64 time_s            = 0;           // 预测时间点 s
        f64 predicted_temp_k  = 0;          // 预测温度 K
        f64 delta_temp_k      = 0;          // 相对当前温度变化 K
        f64 q_solar_w         = 0;          // 太阳加热功率 W
        f64 q_conv_w          = 0;          // 对流功率 W
        f64 q_rad_w           = 0;          // 辐射功率 W
    };

    /// 预测摘要（关键时间点）
    struct Summary {
        f64 temp_now_k        = 0;          // 当前温度
        f64 temp_30s_k        = 0;          // 30秒预测
        f64 temp_1min_k       = 0;          // 1分钟预测
        f64 temp_5min_k       = 0;          // 5分钟预测
        f64 max_temp_k        = 0;          // 预测窗口内最高温度
        f64 min_temp_k        = 0;          // 预测窗口内最低温度
        f64 trend_kps         = 0;          // 温度趋势 K/s
        bool heating          = false;      // 正在升温
        bool overheat_risk    = false;      // 过热风险（>60°C）
        bool superheat_risk   = false;      // 超温风险（>80°C）
    };

    explicit ThermalPredictor(Config cfg);
    ThermalPredictor() : ThermalPredictor(Config{}) {}

    /// 执行预测
    /// @param env 当前环境状态
    /// @return 预测摘要
    Summary predict(const EnvInput& env);

    /// 获取完整预测序列
    const std::vector<Prediction>& predictionSeries() const { return predictions_; }

    /// 更新氦气质量（气囊充放气后质量变化）
    void setHeliumMass(f64 kg) { cfg_.he_mass_kg = kg; }

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    std::vector<Prediction> predictions_;

    /// 单步热力学积分
    f64 stepThermal(f64 temp_k, const EnvInput& env, f64 dt) const;

    /// 计算各热源功率
    f64 qSolar(f64 solar_elev, f64 solar_irr, f64 wind) const;
    f64 qConv(f64 temp_k, f64 air_temp, f64 wind) const;
    f64 qRad(f64 temp_k, f64 air_temp) const;
    f64 qVent(f64 vent_flow, f64 air_temp, f64 temp_k) const;
    f64 qPump(f64 pump_flow, f64 air_temp, f64 temp_k) const;
};

} // namespace FlyteOS::Power
