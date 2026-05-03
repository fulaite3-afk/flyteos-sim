#pragma once
/**
 * @file system_manager.hpp
 * @brief 系统管理器：初始化、主循环、模块协调
 */
#include "../../include/flyteos_types.hpp"
#include "../attitude/attitude_estimator.hpp"
#include "../flight_control/flight_controller.hpp"
#include "../power/helium_buoyancy.hpp"
#include "../power/solar_mppt.hpp"
#include "../navigation/waypoint_navigator.hpp"
#include "../safety/safety_monitor.hpp"

namespace FlyteOS::Core {

class SystemManager {
public:
    struct Config {
        f32 loop_rate_hz        = 400.0f;   // 主循环频率
        f32 watchdog_timeout_s  = 0.5f;     // 看门狗超时
        bool enable_logging     = true;
    };

    enum class State : u8 {
        BOOT,
        INIT,
        READY,
        RUNNING,
        ERROR,
        SHUTDOWN
    };

    explicit SystemManager(Config cfg);
    SystemManager() : SystemManager(Config{}) {}

    Error init();
    Error step(f32 dt);
    Error shutdown();

    State state() const { return state_; }
    const char* stateStr() const;

    // 子系统访问
    Attitude::AttitudeEstimator&    attitude()   { return attitude_; }
    Control::FlightStateMachine&    fsm()        { return fsm_; }
    Power::HeliumBuoyancy&          buoyancy()   { return buoyancy_; }
    Power::SolarMPPT&               solar()      { return solar_; }
    Navigation::WaypointNavigator&  navigator()  { return navigator_; }
    Safety::SafetyMonitor&          safety()     { return safety_; }

private:
    Config cfg_;
    State  state_ = State::BOOT;
    TimeUs boot_ts_ = 0;

    // 子系统实例
    Attitude::AttitudeEstimator    attitude_;
    Control::FlightStateMachine    fsm_;
    Power::HeliumBuoyancy          buoyancy_;
    Power::SolarMPPT               solar_;
    Navigation::WaypointNavigator  navigator_;
    Safety::SafetyMonitor          safety_;

    Error initSubsystems();
    Error runControlLoop(f32 dt);
};

} // namespace FlyteOS::Core
