#pragma once
/**
 * @file visualizer.hpp
 * @brief 控制台可视化器
 */
#include "../include/flyteos_types.hpp"
#include "../src/attitude/attitude_estimator.hpp"
#include "../src/safety/safety_monitor.hpp"
#include "physics_engine.hpp"
#include <string>

namespace FlyteOS::Simulator {

class Visualizer {
public:
    Visualizer();

    void update(const PhysicsState& physics,
                const Attitude::AttitudeEstimator::Estimate& attitude,
                const Safety::SafetyStatus& safety);
    void logEvent(const std::string& msg);

private:
    u32 frame_count_ = 0;
};

} // namespace FlyteOS::Simulator
