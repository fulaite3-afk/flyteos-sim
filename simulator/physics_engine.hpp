#pragma once
/**
 * @file physics_engine.hpp
 * @brief 飞行器物理仿真引擎
 */
#include "../include/flyteos_types.hpp"
#include "../src/flight_control/flight_controller.hpp"

namespace FlyteOS::Simulator {

struct PhysicsState {
    NEDPosition position = {};
    NEDVelocity velocity = {};
    Quat        attitude = {};
    TimeUs      ts = 0;
};

class PhysicsEngine {
public:
    struct Config {
        f32 aircraft_mass_kg     = 12.0f;
        f32 max_thrust_n         = 60.0f;
        f32 buoyancy_ratio       = 0.60f;
        f32 drag_coefficient     = 0.5f;
        f32 max_speed_ms         = 18.0f;
        f32 max_climb_rate       = 8.0f;
        f32 max_descent_rate     = 5.0f;
        f32 launch_altitude_m    = 0.0f;
    };

    explicit PhysicsEngine(Config cfg);
    PhysicsEngine() : PhysicsEngine(Config{}) {}

    PhysicsState step(const AttitudeCmd& cmd, f32 dt);
    void reset();
    void setWind(const Vec3& wind);

    const PhysicsState& state() const { return state_; }

private:
    Config cfg_;
    PhysicsState state_;
    Vec3 wind_ = {0, 0, 0};

    Vec3 computeAerodynamicForces(const NEDVelocity& vel, const Quat& att);
};

} // namespace FlyteOS::Simulator
