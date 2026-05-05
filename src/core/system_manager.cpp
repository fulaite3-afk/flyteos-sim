/**
 * @file system_manager.cpp
 * @brief 系统管理器实现
 */
#include "system_manager.hpp"
#include <cstdio>

namespace FlyteOS::Core {

SystemManager::SystemManager(Config cfg) : cfg_(cfg) {}

const char* SystemManager::stateStr() const {
    static const char* names[] = {
        "BOOT", "INIT", "READY", "RUNNING", "ERROR", "SHUTDOWN"
    };
    return names[static_cast<int>(state_)];
}

Error SystemManager::init() {
    if (state_ != State::BOOT) {
        return Error{ErrorKind::SystemInitFailed, 1, "Not in BOOT state", __FILE__, now_us()};
    }
    state_ = State::INIT;
    boot_ts_ = now_us();

    auto err = initSubsystems();
    if (err.is_err()) {
        state_ = State::ERROR;
        return err;
    }

    state_ = State::READY;
    return Error::ok();
}

Error SystemManager::initSubsystems() {
    // 各子系统初始化
    attitude_.reset();
    // 其他子系统按需初始化
    return Error::ok();
}

Error SystemManager::step(f32 dt) {
    if (state_ == State::ERROR || state_ == State::SHUTDOWN) {
        return Error{ErrorKind::ControlLoopFailure, 0, "System not running", __FILE__, now_us()};
    }

    if (state_ == State::READY) {
        state_ = State::RUNNING;
    }

    return runControlLoop(dt);
}

Error SystemManager::runControlLoop(f32 dt) {
    // 1. 安全检查
    auto safety_status = safety_.check();
    if (!safety_status.safe) {
        // 触发紧急处理
    }

    // 2. 姿态估计 (由外部提供IMU数据后调用)
    // 3. 导航计算
    // 4. 飞行控制
    // 5. 执行器输出
    (void)dt;
    return Error::ok();
}

Error SystemManager::shutdown() {
    state_ = State::SHUTDOWN;
    return Error::ok();
}

} // namespace FlyteOS::Core
