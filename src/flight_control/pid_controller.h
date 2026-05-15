/**
 * @file pid_controller.h
 * @brief 四轴飞行器 PID 姿态控制器（独立模块）
 *
 * 功能：
 *   - 三轴 PID 姿态控制（Roll / Pitch / Yaw）
 *   - 输入：目标姿态角、当前姿态角、角速度
 *   - 输出：电机差分量（用于混控矩阵）
 *   - 经典 PID，参数可配置（Kp / Ki / Kd）
 *   - 抗积分饱和（Clamping 法）
 *   - 输出限幅
 *   - 微分项低通滤波，抑制噪声
 *
 * @company 武汉福莱特航空科技有限公司
 * @author  ASUS
 * @date    2026-05-06
 * @version 1.0.0
 */
#pragma once

#include <cstdint>

#ifndef FLYTEOS_TYPES_HPP_INCLUDED
namespace FlyteOS {
using f32 = float;
struct Vec3 {
    f32 x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(f32 s)         const { return {x*s, y*s, z*s}; }
};
}
#else
#include "../../include/flyteos_types.hpp"
#endif

namespace FlyteOS::Control {

// ════════════════════════════════════════════════════════════════
//  单轴 PID 控制器
// ════════════════════════════════════════════════════════════════

/**
 * @brief 单轴 PID 控制器配置
 */
struct PIDConfig {
    f32 kp        = 4.5f;   // 比例增益
    f32 ki        = 0.2f;   // 积分增益
    f32 kd        = 0.5f;   // 微分增益
    f32 i_limit   = 5.0f;   // 积分项限幅（抗饱和）
    f32 out_limit = 1.0f;   // 输出限幅
    f32 d_filter  = 0.1f;   // 微分低通滤波系数 [0,1]
};

/**
 * @brief 单轴 PID 控制器
 *
 * 算法：
 *   error   = target - current
 *   I      += error * dt          (抗饱和钳位)
 *   D       = d_filter * (error - prev_err)/dt + (1-d_filter) * prev_D
 *   output  = Kp * error + Ki * I + Kd * D   (限幅输出)
 */
class PIDAxis {
public:
    explicit PIDAxis(PIDConfig cfg = PIDConfig{});
    f32 update(f32 error, f32 dt);
    void reset();
    void setConfig(const PIDConfig& cfg) { cfg_ = cfg; }
    const PIDConfig& getConfig() const { return cfg_; }
    f32 getIntegral() const { return integral_; }
    f32 getPrevDerivative() const { return prev_d_; }

private:
    PIDConfig cfg_;
    f32 integral_   = 0.0f;
    f32 prev_error_ = 0.0f;
    f32 prev_d_     = 0.0f;
};

// ════════════════════════════════════════════════════════════════
//  三轴 PID 姿态控制器
// ════════════════════════════════════════════════════════════════

struct AttitudeCtrlOutput {
    f32 roll_cmd  = 0.0f;
    f32 pitch_cmd = 0.0f;
    f32 yaw_cmd   = 0.0f;
};

/**
 * @brief 四轴飞行器 PID 姿态控制器（Roll / Pitch / Yaw）
 */
class PIDAttitudeController {
public:
    struct Config {
        PIDConfig roll;
        PIDConfig pitch;
        PIDConfig yaw;
        Config() {
            roll  = {4.5f, 0.2f, 0.5f, 5.0f, 1.0f, 0.1f};
            pitch = {4.5f, 0.2f, 0.5f, 5.0f, 1.0f, 0.1f};
            yaw   = {3.0f, 0.1f, 0.3f, 3.0f, 0.8f, 0.1f};
        }
    };

    explicit PIDAttitudeController(const Config& cfg = Config());

    AttitudeCtrlOutput update(
        const FlyteOS::Vec3& target,
        const FlyteOS::Vec3& current,
        const FlyteOS::Vec3& ang_vel,
        f32 dt
    );

    void reset();
    void setConfig(const Config& cfg);
    const Config& getConfig() const { return cfg_; }

    /**
     * @brief 标准 X 型四旋翼混控矩阵
     */
    static void mixQuadX(f32 roll, f32 pitch, f32 yaw, f32 thrust, f32 motor[4]);

private:
    Config cfg_;
    PIDAxis pid_roll_, pid_pitch_, pid_yaw_;
};

} // namespace FlyteOS::Control
