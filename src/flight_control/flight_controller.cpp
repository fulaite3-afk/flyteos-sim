/**
 * @file flight_controller.cpp
 * @brief 飞行控制器实现
 */
#include "flight_controller.hpp"
#include <cmath>
#include <algorithm>

namespace FlyteOS::Control {

// ══════════════════ PID ══════════════════════════════════════════
f32 PID::update(f32 error, f32 dt) {
    if (dt <= 0) return 0;
    integral_ += error * dt;
    integral_  = std::clamp(integral_, -cfg_.i_limit, cfg_.i_limit);
    f32 deriv  = (error - prev_err_) / dt;
    // 低通滤波微分
    deriv = cfg_.d_filter * deriv + (1.0f - cfg_.d_filter) * prev_d_;
    prev_d_    = deriv;
    prev_err_  = error;
    f32 out    = cfg_.kp * error + cfg_.ki * integral_ + cfg_.kd * deriv;
    return std::clamp(out, -cfg_.out_limit, cfg_.out_limit);
}

void PID::reset() { integral_ = prev_err_ = prev_d_ = 0; }

// ══════════════════ 位置控制器 ════════════════════════════════════
PositionController::PositionController(Config cfg)
    : cfg_(cfg), pid_n_(cfg.xy), pid_e_(cfg.xy), pid_d_(cfg.z) {}

VelocityCmd PositionController::update(
    const NEDPosition& target, const NEDPosition& current,
    const NEDVelocity& vel, f32 dt)
{
    (void)vel;
    f32 en = target.north - current.north;
    f32 ee = target.east  - current.east;
    f32 ed = target.down  - current.down;

    VelocityCmd cmd;
    cmd.vn = pid_n_.update(en, dt);
    cmd.ve = pid_e_.update(ee, dt);
    cmd.vd = pid_d_.update(ed, dt);

    // 限速
    cmd.vn = std::clamp(cmd.vn, -cfg_.max_horiz_speed, cfg_.max_horiz_speed);
    cmd.ve = std::clamp(cmd.ve, -cfg_.max_horiz_speed, cfg_.max_horiz_speed);
    cmd.vd = std::clamp(cmd.vd, -cfg_.max_vert_speed,  cfg_.max_vert_speed);
    return cmd;
}

void PositionController::reset() { pid_n_.reset(); pid_e_.reset(); pid_d_.reset(); }
void PositionController::setConfig(const Config& c) {
    cfg_ = c;
    pid_n_.setConfig(c.xy); pid_e_.setConfig(c.xy); pid_d_.setConfig(c.z);
}

// ══════════════════ 速度控制器 ════════════════════════════════════
VelocityController::VelocityController(Config cfg)
    : cfg_(cfg), pid_vn_(cfg.horiz), pid_ve_(cfg.horiz), pid_vd_(cfg.vert) {}

AttitudeCmd VelocityController::update(
    const VelocityCmd& target, const NEDVelocity& current, f32 yaw, f32 dt)
{
    f32 evn = target.vn - current.vn;
    f32 eve = target.ve - current.ve;
    f32 evd = target.vd - current.vd;

    f32 an = pid_vn_.update(evn, dt);
    f32 ae = pid_ve_.update(eve, dt);
    f32 ad = pid_vd_.update(evd, dt);

    const f32 g = 9.81f;
    AttitudeCmd cmd;
    // 水平加速度 → 姿态角（小角近似）
    cmd.pitch = std::clamp( (an * cosf(yaw) + ae * sinf(yaw)) / g, -0.35f, 0.35f);
    cmd.roll  = std::clamp( (ae * cosf(yaw) - an * sinf(yaw)) / g, -0.35f, 0.35f);
    cmd.yaw   = yaw;
    cmd.thrust = std::clamp(0.5f - ad / g, 0.0f, 1.0f);
    return cmd;
}

void VelocityController::reset() { pid_vn_.reset(); pid_ve_.reset(); pid_vd_.reset(); }

// ══════════════════ 姿态控制器 ════════════════════════════════════
AttitudeController::AttitudeController(Config cfg)
    : cfg_(cfg), pid_roll_(cfg.roll), pid_pitch_(cfg.pitch), pid_yaw_(cfg.yaw) {}

ActuatorOutput AttitudeController::update(
    const AttitudeCmd& cmd,
    const Attitude::AttitudeEstimator::Estimate& est,
    const Power::HeliumBuoyancy::Status& buoy,
    f32 dt)
{
    f32 e_roll  = cmd.roll  - est.euler_rad.x;
    f32 e_pitch = cmd.pitch - est.euler_rad.y;
    f32 e_yaw   = cmd.yaw   - est.euler_rad.z;
    // 偏航误差归一化到 [-π, π]
    while (e_yaw >  M_PI) e_yaw -= 2*M_PI;
    while (e_yaw < -M_PI) e_yaw += 2*M_PI;

    f32 roll_out  = pid_roll_.update(e_roll,  dt);
    f32 pitch_out = pid_pitch_.update(e_pitch, dt);
    f32 yaw_out   = pid_yaw_.update(e_yaw,   dt);

    // 浮力补偿：氦气承担部分推力
    f32 buoy_comp = std::clamp(buoy.buoyancy_ratio * cfg_.buoyancy_ratio, 0.0f, 0.9f);
    f32 effective_thrust = cmd.thrust * (1.0f - buoy_comp);

    ActuatorOutput out;
    out.ts = now_us();
    mixActuators(roll_out, pitch_out, yaw_out, effective_thrust, buoy_comp, out);
    return out;
}

void AttitudeController::mixActuators(
    f32 roll, f32 pitch, f32 yaw, f32 thrust, f32 /*buoy*/, ActuatorOutput& out)
{
    // 标准四旋翼混控 (X型)
    // M1:前左  M2:前右  M3:后右  M4:后左
    out.motor[0] = std::clamp(thrust + roll - pitch + yaw, 0.0f, 1.0f);
    out.motor[1] = std::clamp(thrust - roll - pitch - yaw, 0.0f, 1.0f);
    out.motor[2] = std::clamp(thrust - roll + pitch + yaw, 0.0f, 1.0f);
    out.motor[3] = std::clamp(thrust + roll + pitch - yaw, 0.0f, 1.0f);
    out.thrust_n = thrust * 60.0f;  // 假设最大推力60N
}

void AttitudeController::reset() {
    pid_roll_.reset(); pid_pitch_.reset(); pid_yaw_.reset();
}

// ══════════════════ 飞行状态机 ════════════════════════════════════
FlightStateMachine::Transition FlightStateMachine::handleEvent(Event ev) {
    FlightState from = state_, to = from;
    bool allowed = false;
    std::string reason;

    switch (state_) {
    case FlightState::DISARMED:
        if (ev == Event::ARM) { to = FlightState::STANDBY; allowed = true; reason = "Armed OK"; }
        break;
    case FlightState::STANDBY:
        if (ev == Event::TAKEOFF) { to = FlightState::TAKING_OFF; allowed = true; reason = "Takeoff cmd"; }
        if (ev == Event::DISARM)  { to = FlightState::DISARMED;   allowed = true; reason = "Disarmed"; }
        break;
    case FlightState::TAKING_OFF:
        if (ev == Event::FAULT)   { to = FlightState::FAILSAFE;    allowed = true; reason = "Fault during takeoff"; }
        // 外部判断离地后设为 IN_FLIGHT
        { to = FlightState::IN_FLIGHT; allowed = true; reason = "Airborne"; }
        break;
    case FlightState::IN_FLIGHT:
    case FlightState::HOVERING:
    case FlightState::WAYPOINT_NAV:
        if (ev == Event::LAND)      { to = FlightState::LANDING;          allowed = true; }
        if (ev == Event::GO_HOME)   { to = FlightState::WAYPOINT_NAV;     allowed = true; }
        if (ev == Event::EMERGENCY) { to = FlightState::EMERGENCY_LANDING;allowed = true; }
        if (ev == Event::FAULT)     { to = FlightState::FAILSAFE;          allowed = true; }
        break;
    case FlightState::LANDING:
        if (ev == Event::LAND_COMPLETE) { to = FlightState::STANDBY; allowed = true; }
        if (ev == Event::EMERGENCY)     { to = FlightState::EMERGENCY_LANDING; allowed = true; }
        break;
    case FlightState::FAILSAFE:
        if (ev == Event::FAULT_CLEAR)   { to = FlightState::STANDBY; allowed = true; reason = "Fault cleared"; }
        break;
    default:
        break;
    }

    if (allowed) {
        onExit(state_);
        state_ = to;
        onEnter(to);
        enter_ts_ = now_us();
    }
    return {allowed, from, to, reason};
}

bool FlightStateMachine::isFlying() const {
    return state_ == FlightState::IN_FLIGHT    ||
           state_ == FlightState::HOVERING     ||
           state_ == FlightState::WAYPOINT_NAV ||
           state_ == FlightState::TAKING_OFF   ||
           state_ == FlightState::LANDING      ||
           state_ == FlightState::EMERGENCY_LANDING;
}
bool FlightStateMachine::canArm() const { return state_ == FlightState::DISARMED; }
void FlightStateMachine::onEnter(FlightState) {}
void FlightStateMachine::onExit(FlightState)  {}

} // namespace FlyteOS::Control
