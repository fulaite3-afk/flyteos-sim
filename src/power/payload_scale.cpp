/**
 * @file payload_scale.cpp
 * @brief 载荷实时称重模块实现
 *
 * 传感器链路：
 *   载荷→应变→电阻变化ΔR→电桥电压Vout→ADC数字量→滤波→重量
 *
 * 关键公式：
 *   电桥输出: Vout = Vin × (GF × ε) / 4
 *   微应变: με = (4 × Vout) / (Vin × GF) × 1e6
 *   重量: W = με / (灵敏度 × 1e3 × 额定载荷)
 */

#include "payload_scale.hpp"
#include <algorithm>
#include <cstdlib>

namespace FlyteOS::Power {

// ════════════════════════════════════════════════════════════════
//  构造/初始化
// ════════════════════════════════════════════════════════════════

PayloadScale::PayloadScale(Config cfg) : cfg_(cfg) {
    avg_buffer_.resize(cfg_.avg_window, 0);
}

void PayloadScale::initialize(f32 empty_mass_kg) {
    empty_mass_kg_ = empty_mass_kg;
    state_ = {};
    avg_buffer_.assign(cfg_.avg_window, 0);
    avg_idx_ = 0;
    lpf_prev_ = 0;
    prev_weight_ = 0;
    printf("[PayloadScale] Initialized: empty_mass=%.1fkg, capacity=%.0fkg\n",
           empty_mass_kg, cfg_.rated_capacity_kg);
}

// ════════════════════════════════════════════════════════════════
//  主更新（ADC原始数据输入）
// ════════════════════════════════════════════════════════════════

PayloadScale::State PayloadScale::update(i32 adc_raw, f32 dt) {
    if (dt <= 0) return state_;

    state_.adc_raw = static_cast<f32>(adc_raw);

    // 1. ADC → 电桥输出电压 (mV)
    f32 bridge_mv = adcToBridgeMv(adc_raw);
    state_.bridge_mv = bridge_mv;

    // 2. 电桥电压 → 微应变
    f32 strain_ue = bridgeMvToStrainUe(bridge_mv);
    state_.strain_uv = strain_ue;

    // 3. 微应变 → 重量
    f32 raw_weight = strainToWeight(strain_ue);
    state_.raw_weight_kg = raw_weight;

    // 4. 尖峰过滤
    if (isSpike(raw_weight)) {
        // 尖峰：使用上次滤波值
        state_.filtered_weight_kg = lpf_prev_;
    } else {
        // 5. 滑动平均 + 低通滤波
        f32 avg = movingAverage(raw_weight);
        state_.filtered_weight_kg = lowPassFilter(avg);
    }

    // 6. 计算有效载荷和总质量
    state_.payload_kg = std::max(0.0f, state_.filtered_weight_kg - empty_mass_kg_);
    state_.total_mass_kg = state_.filtered_weight_kg;

    // 7. 过载检测
    state_.overload = state_.payload_kg > cfg_.rated_capacity_kg;
    state_.valid = true;

    // 8. 载荷变化检测
    detectChange(dt);

    state_.ts = now_us();
    prev_weight_ = state_.filtered_weight_kg;
    return state_;
}

// ════════════════════════════════════════════════════════════════
//  仿真模式（从真实重量生成ADC读数 + 噪声）
// ════════════════════════════════════════════════════════════════

PayloadScale::State PayloadScale::updateSimulated(f32 true_weight_kg, f32 dt) {
    if (dt <= 0) return state_;

    // 1. 重量 → 微应变
    // 简化：在额定载荷下产生额定微应变
    f32 rated_strain_ue = cfg_.sensitivity_mvv * 1000.0f / cfg_.bridge_vin *
                          4.0f / cfg_.gauge_factor * 1e6f;
    f32 strain_ue = rated_strain_ue * true_weight_kg / cfg_.rated_capacity_kg;

    // 2. 微应变 → 电桥电压
    f32 bridge_mv = strain_ue * cfg_.bridge_vin * cfg_.gauge_factor / 4.0f / 1e6f * 1000.0f;

    // 3. 电桥电压 → ADC读数
    f32 adc_fullscale = cfg_.adc_vref / cfg_.adc_gain;
    f32 adc_max = static_cast<f32>((1 << (cfg_.adc_bits - 1)) - 1);
    i32 adc_raw = static_cast<i32>(bridge_mv / 1000.0f / adc_fullscale * adc_max);

    // 4. 添加噪声（高斯，σ ≈ 2 ADC LSB ≈ 数克级）
    f32 noise_sigma = 3.0f;  // LSB
    f32 u1 = (rand() + 1) / (f32)(RAND_MAX + 1u);
    f32 u2 = (rand() + 1) / (f32)(RAND_MAX + 1u);
    f32 z0 = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (f32)M_PI * u2);
    adc_raw += static_cast<i32>(z0 * noise_sigma);

    // 5. 使用标准更新流程
    return update(adc_raw, dt);
}

// ════════════════════════════════════════════════════════════════
//  标定
// ════════════════════════════════════════════════════════════════

void PayloadScale::calibrateZero(i32 adc_raw_zero) {
    cfg_.scale_offset_raw = static_cast<f32>(adc_raw_zero);
    printf("[PayloadScale] Zero calibration: offset_raw=%.0f\n", cfg_.scale_offset_raw);
}

void PayloadScale::calibrateSpan(f32 known_weight_kg, i32 adc_raw_at_weight) {
    f32 delta_adc = static_cast<f32>(adc_raw_at_weight) - cfg_.scale_offset_raw;
    if (std::fabs(delta_adc) < 1.0f) return;  // 防止除零
    cfg_.scale_factor = known_weight_kg / delta_adc;
    printf("[PayloadScale] Span calibration: factor=%.6f kg/LSB\n", cfg_.scale_factor);
}

// ════════════════════════════════════════════════════════════════
//  ADC → 物理量转换
// ════════════════════════════════════════════════════════════════

f32 PayloadScale::adcToBridgeMv(i32 adc_raw) const {
    // ADC值 → 电压：考虑增益和偏移
    f32 adc_max = static_cast<f32>((1 << (cfg_.adc_bits - 1)) - 1);
    f32 adc_fullscale_v = cfg_.adc_vref / cfg_.adc_gain;

    // 去偏移
    f32 adc_corrected = static_cast<f32>(adc_raw) - cfg_.scale_offset_raw;

    // 转电压
    f32 voltage_v = adc_corrected / adc_max * adc_fullscale_v;

    // 转mV
    return voltage_v * 1000.0f * cfg_.scale_factor;
}

f32 PayloadScale::bridgeMvToStrainUe(f32 bridge_mv) const {
    // Vout = Vin × GF × ε / 4
    // ε = 4 × Vout / (Vin × GF)
    // με = ε × 1e6
    if (cfg_.bridge_vin <= 0 || cfg_.gauge_factor <= 0) return 0;
    f32 strain = (4.0f * bridge_mv / 1000.0f) / (cfg_.bridge_vin * cfg_.gauge_factor);
    return strain * 1e6f;
}

f32 PayloadScale::strainToWeight(f32 strain_ue) const {
    // 传感器灵敏度: S mV/V @ 额定载荷 C kg
    // 即在C kg时输出 S mV/V
    // 微应变与重量线性关系
    f32 rated_strain_ue = cfg_.sensitivity_mvv * 1000.0f / cfg_.bridge_vin *
                          4.0f / cfg_.gauge_factor * 1e6f;
    if (rated_strain_ue <= 0) return 0;
    return strain_ue / rated_strain_ue * cfg_.rated_capacity_kg;
}

// ════════════════════════════════════════════════════════════════
//  滤波
// ════════════════════════════════════════════════════════════════

f32 PayloadScale::lowPassFilter(f32 raw) {
    f32 alpha = cfg_.lpf_alpha;
    f32 filtered = alpha * raw + (1.0f - alpha) * lpf_prev_;
    lpf_prev_ = filtered;
    return filtered;
}

f32 PayloadScale::movingAverage(f32 raw) {
    avg_buffer_[avg_idx_ % cfg_.avg_window] = raw;
    avg_idx_++;

    f32 sum = 0;
    i32 count = std::min(avg_idx_, cfg_.avg_window);
    for (i32 i = 0; i < count; i++) {
        sum += avg_buffer_[i];
    }
    return count > 0 ? sum / count : raw;
}

bool PayloadScale::isSpike(f32 raw) {
    if (prev_weight_ == 0) return false;
    f32 delta = std::fabs(raw - prev_weight_);
    return delta > cfg_.spike_threshold_kg;
}

// ════════════════════════════════════════════════════════════════
//  载荷变化检测
// ════════════════════════════════════════════════════════════════

void PayloadScale::detectChange(f32 dt) {
    if (dt <= 0 || prev_weight_ == 0) return;

    f32 delta = state_.filtered_weight_kg - prev_weight_;
    f32 rate = delta / dt;

    state_.rate_of_change_kg_s = rate;

    // 显著变化判定：|Δ| > 0.5kg 且 |rate| > 0.1 kg/s
    bool significant = std::fabs(delta) > 0.5f && std::fabs(rate) > 0.1f;

    if (significant) {
        last_event_ = PayloadChangeEvent{
            delta,
            state_.payload_kg,
            rate,
            true,
            now_us()
        };
    }
}

} // namespace FlyteOS::Power
