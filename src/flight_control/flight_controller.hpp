#pragma once
/**
 * @file flight_controller.hpp
 * @brief 飞行控制器：位置/速度/姿态三环PID + 浮空器专用混控
 */
#include "../../include/flyteos_types.hpp"
#include "../attitude/attitude_estimator.hpp"
#include "../power/helium_buoyancy.hpp"

namespace FlyteOS::Control {

// ─── 通用PID ───────────────────────────────────────────────────────
struct PIDConfig {
    f32 kp = 1.0f, ki = 0.0f, kd = 0.0f;
    f32 i_limit = 10.0f;
    f32 out_limit = 1.0f;
    f32 d_filter  = 0.1f;  // 微分低通滤波系数
};

class PID {
public:
    explicit PID(PIDConfig cfg);
    PID() : PID(PIDConfig{}) {}
    f32 update(f32 error, f32 dt);
    void reset();
    void setConfig(const PIDConfig& c) { cfg_ = c; }

private:
    PIDConfig cfg_;
    f32 integral_ = 0, prev_err_ = 0, prev_d_ = 0;
};

// ─── 位置控制器（外环）────────────────────────────────────────────────
class PositionController {
public:
    struct Config {
        PIDConfig xy = {2.0f, 0.05f, 0.3f, 5.0f, 5.0f};
        PIDConfig z  = {3.0f, 0.1f,  0.5f, 3.0f, 3.0f};
        f32 max_horiz_speed = 6.0f;  // m/s
        f32 max_vert_speed  = 2.0f;
    };

    explicit PositionController(Config cfg);
    PositionController() : PositionController(Config{}) {}
    VelocityCmd update(const NEDPosition& target, const NEDPosition& current,
                       const NEDVelocity& vel, f32 dt);
    void reset();
    void setConfig(const Config& c);

private:
    Config cfg_;
    PID pid_n_, pid_e_, pid_d_;
};

// ─── 速度控制器（中环）────────────────────────────────────────────────
class VelocityController {
public:
    struct Config {
        PIDConfig horiz = {3.0f, 0.1f, 0.2f, 10.0f, 0.6f};
        PIDConfig vert  = {4.0f, 0.2f, 0.3f, 5.0f,  0.8f};
    };

    explicit VelocityController(Config cfg);
    VelocityController() : VelocityController(Config{}) {}
    AttitudeCmd update(const VelocityCmd& target, const NEDVelocity& current,
                       f32 yaw, f32 dt);
    void reset();

private:
    Config cfg_;
    PID pid_vn_, pid_ve_, pid_vd_;
};

// ─── 姿态控制器（内环）────────────────────────────────────────────────
class AttitudeController {
public:
    enum class Mode : u8 { STABILIZE, ALT_HOLD, POS_HOLD, AUTO, LANDING };

    struct Config {
        PIDConfig roll  = {4.5f, 0.2f, 0.5f, 5.0f, 1.0f};
        PIDConfig pitch = {4.5f, 0.2f, 0.5f, 5.0f, 1.0f};
        PIDConfig yaw   = {3.0f, 0.1f, 0.3f, 3.0f, 0.8f};

        f32 buoyancy_ratio      = 0.60f;  // 氦气分担60%升力
        f32 max_roll_rad        = 0.35f;
        f32 max_pitch_rad       = 0.35f;
        f32 max_yaw_rate_rads   = 1.5f;
    };

    explicit AttitudeController(Config cfg);
    AttitudeController() : AttitudeController(Config{}) {}

    ActuatorOutput update(
        const AttitudeCmd& cmd,
        const Attitude::AttitudeEstimator::Estimate& est,
        const Power::HeliumBuoyancy::Status& buoy,
        f32 dt
    );

    void setMode(Mode m) { mode_ = m; }
    Mode mode() const    { return mode_; }
    void reset();

private:
    Config cfg_;
    Mode   mode_ = Mode::STABILIZE;
    PID    pid_roll_, pid_pitch_, pid_yaw_;

    // 浮空器专用混控矩阵（四旋翼 + 氦气补偿）
    void mixActuators(f32 roll_cmd, f32 pitch_cmd, f32 yaw_cmd,
                      f32 thrust, f32 buoyancy_ratio, ActuatorOutput& out);
};

// ─── 飞行状态机 ────────────────────────────────────────────────────
class FlightStateMachine {
public:
    enum class Event : u8 {
        ARM, DISARM, TAKEOFF, AIRBORNE, LAND, GO_HOME,
        EMERGENCY, FAULT, FAULT_CLEAR, LAND_COMPLETE
    };

    struct Transition {
        bool        allowed;
        FlightState from, to;
        std::string reason;
    };

    Transition handleEvent(Event ev);
    FlightState state() const { return state_; }
    bool isFlying()    const;
    bool canArm()      const;

private:
    FlightState state_ = FlightState::DISARMED;
    TimeUs      enter_ts_ = 0;

    void onEnter(FlightState s);
    void onExit(FlightState s);
};

} // namespace FlyteOS::Control
