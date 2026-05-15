#pragma once
/**
 * @file payload_scale.hpp
 * @brief 载荷实时称重模块 —— 应变片+ADC实时称重
 *
 * 设计依据：
 *   - 浮空器载荷变化直接影响B/W比需求
 *   - 载荷增重→需更多浮力→B/W上调
 *   - 载荷减重→需减少浮力→B/W下调
 *   - 实时称重→动态浮力比调整的输入信号
 *
 * 传感器模型：
 *   - 应变片(Strain Gauge)：电阻变化ΔR/R = GF × ε
 *   - 惠斯通电桥：Vout = Vin × GF × ε / 4
 *   - ADC采样：24bit Σ-Δ (如HX711)，增益128
 *   - 滤波：滑动平均 + 一阶低通
 */

#include "../../include/flyteos_types.hpp"
#include <cmath>
#include <vector>

namespace FlyteOS::Power {

class PayloadScale {
public:
    /// 硬件配置
    struct Config {
        f32 gauge_factor     = 2.0f;      // 应变片灵敏系数 GF
        f32 bridge_vin       = 5.0f;      // 电桥激励电压 V
        f32 adc_gain         = 128.0f;    // ADC可编程增益 (HX711: 128/64/32)
        f32 adc_vref         = 4.3f;      // ADC参考电压 V
        i32 adc_bits         = 24;        // ADC分辨率 (24bit Σ-Δ)
        f32 rated_capacity_kg = 50.0f;    // 传感器额定载荷 kg
        f32 sensitivity_mvv  = 2.0f;      // 传感器灵敏度 mV/V
        f32 scale_offset_raw = 0.0f;      // 零点偏移（标定值）
        f32 scale_factor     = 1.0f;      // 标定系数（标定后更新）

        // 滤波参数
        f32 lpf_alpha        = 0.1f;      // 低通滤波系数 (0~1, 越小越平滑)
        i32 avg_window       = 8;         // 滑动平均窗口大小
        f32 spike_threshold_kg = 5.0f;    // 尖峰过滤阈值 kg
    };

    /// 称重状态
    struct State {
        f32 raw_weight_kg    = 0;         // 原始重量 kg（未滤波）
        f32 filtered_weight_kg = 0;       // 滤波后重量 kg
        f32 payload_kg       = 0;         // 有效载荷 = 当前重量 - 空机重量
        f32 total_mass_kg    = 0;         // 总质量 = 空机重量 + 载荷
        f32 rate_of_change_kg_s = 0;      // 载荷变化率 kg/s
        f32 adc_raw          = 0;         // ADC原始读数
        f32 strain_uv        = 0;         // 微应变 με
        f32 bridge_mv        = 0;         // 电桥输出 mV
        bool valid           = false;     // 数据有效标志
        bool overload        = false;     // 过载警告
        TimeUs ts            = 0;
    };

    /// 载荷变化事件
    struct PayloadChangeEvent {
        f32  delta_kg       = 0;          // 载荷变化量 kg
        f32  new_payload_kg = 0;          // 新载荷 kg
        f32  rate_kg_s      = 0;          // 变化率 kg/s
        bool significant     = false;     // 是否为显著变化
        TimeUs ts           = 0;
    };

    explicit PayloadScale(Config cfg);
    PayloadScale() : PayloadScale(Config{}) {}

    /// 初始化：设置空机重量
    /// @param empty_mass_kg 飞行器空机重量（含气囊、框架、电池、不含载荷）
    void initialize(f32 empty_mass_kg);

    /// 主更新：传入ADC原始读数
    /// @param adc_raw ADC原始采样值 (24bit signed)
    /// @param dt 时间步长 s
    State update(i32 adc_raw, f32 dt);

    /// 仿真模式：传入真实重量，自动生成ADC读数+噪声
    /// @param true_weight_kg 真实重量 kg
    /// @param dt 时间步长 s
    State updateSimulated(f32 true_weight_kg, f32 dt);

    /// 标定：零点校准（空载时调用）
    void calibrateZero(i32 adc_raw_zero);

    /// 标定：满量程校准
    /// @param known_weight_kg 已知标准重量 kg
    /// @param adc_raw_at_weight 对应ADC读数
    void calibrateSpan(f32 known_weight_kg, i32 adc_raw_at_weight);

    /// 获取最近的载荷变化事件
    const PayloadChangeEvent& lastChangeEvent() const { return last_event_; }

    /// 获取状态
    const State& state() const { return state_; }
    const Config& config() const { return cfg_; }

    /// 设置空机重量（如加注氦气后需要更新）
    void setEmptyMass(f32 kg) { empty_mass_kg_ = kg; }
    f32 emptyMass() const { return empty_mass_kg_; }

private:
    Config cfg_;
    State  state_;
    PayloadChangeEvent last_event_;

    f32 empty_mass_kg_ = 12.0f;  // 默认空机重量

    // 滤波缓冲
    std::vector<f32> avg_buffer_;
    i32 avg_idx_ = 0;
    f32 lpf_prev_ = 0;
    f32 prev_weight_ = 0;

    // ADC → 物理量转换
    f32 adcToBridgeMv(i32 adc_raw) const;
    f32 bridgeMvToStrainUe(f32 bridge_mv) const;
    f32 strainToWeight(f32 strain_ue) const;

    // 滤波
    f32 lowPassFilter(f32 raw);
    f32 movingAverage(f32 raw);
    bool isSpike(f32 raw);

    // 载荷变化检测
    void detectChange(f32 dt);
};

} // namespace FlyteOS::Power
