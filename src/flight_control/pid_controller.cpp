/**
 * @file pid_controller.cpp
 * @brief PID 姿态控制器实现
 *
 * @company 武汉福莱特航空科技有限公司
 * @author  ASUS
 * @date    2026-05-06
 */
#include "pid_controller.h"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace FlyteOS::Control {

// ════════════════════════════════════════════════════════════════
//  PIDAxis — 单轴 PID 控制器
// ════════════════════════════════════════════════════════════════

PIDAxis::PIDAxis(PIDConfig cfg) : cfg_(cfg) {}

f32 PIDAxis::update(f32 error, f32 dt) {
    if (dt <= 0.0f) return 0.0f;

    // 比例项
    f32 p_term = cfg_.kp * error;

    // 积分项（抗饱和 Clamping 法）
    integral_ += error * dt;
    // 条件积分：P+I 超出限幅时停止积分累积
    f32 pi_out = p_term + cfg_.ki * integral_;
    if (pi_out > cfg_.out_limit)
        integral_ = std::min(integral_, cfg_.i_limit);
    else if (pi_out < -cfg_.out_limit)
        integral_ = std::max(integral_, -cfg_.i_limit);
    integral_ = std::clamp(integral_, -cfg_.i_limit, cfg_.i_limit);

    f32 i_term = cfg_.ki * integral_;

    // 微分项（一阶低通滤波）
    f32 deriv = (error - prev_error_) / dt;
    deriv = cfg_.d_filter * deriv + (1.0f - cfg_.d_filter) * prev_d_;
    prev_d_     = deriv;
    prev_error_ = error;

    f32 d_term = cfg_.kd * deriv;

    // 汇总 + 输出限幅
    return std::clamp(p_term + i_term + d_term, -cfg_.out_limit, cfg_.out_limit);
}

void PIDAxis::reset() {
    integral_   = 0.0f;
    prev_error_ = 0.0f;
    prev_d_     = 0.0f;
}

// ════════════════════════════════════════════════════════════════
//  PIDAttitudeController — 三轴姿态控制器
// ════════════════════════════════════════════════════════════════

PIDAttitudeController::PIDAttitudeController(const Config& cfg)
    : cfg_(cfg),
      pid_roll_(cfg.roll),
      pid_pitch_(cfg.pitch),
      pid_yaw_(cfg.yaw) {}

AttitudeCtrlOutput PIDAttitudeController::update(
    const FlyteOS::Vec3& target,
    const FlyteOS::Vec3& current,
    const FlyteOS::Vec3& /*ang_vel*/,
    f32 dt)
{
    f32 e_roll  = target.x - current.x;
    f32 e_pitch = target.y - current.y;
    f32 e_yaw   = target.z - current.z;

    // 偏航误差归一化到 [-π, π]
    while (e_yaw >  M_PI) e_yaw -= 2.0f * static_cast<f32>(M_PI);
    while (e_yaw < -M_PI) e_yaw += 2.0f * static_cast<f32>(M_PI);

    return {
        pid_roll_.update(e_roll,  dt),
        pid_pitch_.update(e_pitch, dt),
        pid_yaw_.update(e_yaw,   dt)
    };
}

void PIDAttitudeController::reset() {
    pid_roll_.reset();
    pid_pitch_.reset();
    pid_yaw_.reset();
}

void PIDAttitudeController::setConfig(const Config& cfg) {
    cfg_ = cfg;
    pid_roll_.setConfig(cfg.roll);
    pid_pitch_.setConfig(cfg.pitch);
    pid_yaw_.setConfig(cfg.yaw);
}

// 标准 X 型四旋翼混控矩阵
void PIDAttitudeController::mixQuadX(
    f32 roll, f32 pitch, f32 yaw, f32 thrust, f32 motor[4])
{
    motor[0] = std::clamp(thrust + roll - pitch + yaw, 0.0f, 1.0f);
    motor[1] = std::clamp(thrust - roll - pitch - yaw, 0.0f, 1.0f);
    motor[2] = std::clamp(thrust - roll + pitch + yaw, 0.0f, 1.0f);
    motor[3] = std::clamp(thrust + roll + pitch - yaw, 0.0f, 1.0f);
}

} // namespace FlyteOS::Control
