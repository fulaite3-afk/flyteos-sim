/**
 * @file physics_engine.cpp
 * @brief 飞行器物理仿真引擎：6DOF刚体动力学
 */
#include "physics_engine.hpp"
#include <cmath>

namespace FlyteOS::Simulator {

PhysicsEngine::PhysicsEngine(Config cfg) : cfg_(cfg) {
    state_.position = {0, 0, -cfg_.launch_altitude_m};
    state_.velocity = {0, 0, 0};
    state_.attitude = {1, 0, 0, 0}; // 单位四元数
}

PhysicsState PhysicsEngine::step(const AttitudeCmd& cmd, f32 dt) {
    // 1. 计算气动力
    Vec3 aero_force = computeAerodynamicForces(state_.velocity, state_.attitude);

    // 2. 计算推力（在机体坐标系）
    f32 g = 9.81f;
    f32 thrust_n = cmd.thrust * cfg_.max_thrust_n;

    // 3. 浮力补偿
    f32 buoyancy_n = cfg_.aircraft_mass_kg * g * cfg_.buoyancy_ratio;

    // 4. 合力计算（简化：NED坐标系）
    Vec3 total_force;
    total_force.x = aero_force.x;
    total_force.y = aero_force.y;
    total_force.z = -(thrust_n + buoyancy_n) + cfg_.aircraft_mass_kg * g + aero_force.z;

    // 5. 加速度
    Vec3 accel;
    accel.x = total_force.x / cfg_.aircraft_mass_kg;
    accel.y = total_force.y / cfg_.aircraft_mass_kg;
    accel.z = total_force.z / cfg_.aircraft_mass_kg;

    // 6. 积分
    state_.velocity.vn += accel.x * dt;
    state_.velocity.ve += accel.y * dt;
    state_.velocity.vd += accel.z * dt;

    // 速度限制
    f32 horiz_speed = sqrtf(state_.velocity.vn * state_.velocity.vn +
                            state_.velocity.ve * state_.velocity.ve);
    if (horiz_speed > cfg_.max_speed_ms) {
        f32 ratio = cfg_.max_speed_ms / horiz_speed;
        state_.velocity.vn *= ratio;
        state_.velocity.ve *= ratio;
    }
    state_.velocity.vd = std::clamp(state_.velocity.vd, -cfg_.max_climb_rate, cfg_.max_descent_rate);

    // 7. 位置积分
    state_.position.north += state_.velocity.vn * dt;
    state_.position.east  += state_.velocity.ve * dt;
    state_.position.down  += state_.velocity.vd * dt;

    // 8. 姿态更新（简化）
    state_.attitude = Quat::fromEuler(cmd.roll, cmd.pitch, cmd.yaw);

    state_.ts = now_us();
    return state_;
}

Vec3 PhysicsEngine::computeAerodynamicForces(const NEDVelocity& vel, const Quat& /*att*/) {
    // 简化气动模型：线性阻力
    Vec3 drag;
    f32 cd = cfg_.drag_coefficient;
    drag.x = -cd * vel.vn;
    drag.y = -cd * vel.ve;
    drag.z = -cd * vel.vd;
    return drag;
}

void PhysicsEngine::reset() {
    state_ = PhysicsState{};
    state_.position = {0, 0, -cfg_.launch_altitude_m};
}

void PhysicsEngine::setWind(const Vec3& wind) {
    wind_ = wind;
}

} // namespace FlyteOS::Simulator
