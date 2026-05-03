/**
 * @file dji_osdk_bridge.cpp
 * @brief DJI OSDK 桥接层实现（Stub - Phase 1 待集成真实 OSDK）
 */
#include "dji_osdk_bridge.hpp"

namespace FlyteOS::DJIBridge {

DJIOSDKBridge::DJIOSDKBridge(Config cfg) : cfg_(cfg) {}

Error DJIOSDKBridge::connect() {
    // TODO: Phase 1 - 集成 DJI OSDK 实际连接逻辑
    status_ = ConnectionStatus::CONNECTING;
    // 模拟连接成功
    status_ = ConnectionStatus::CONNECTED;
    return Error::ok();
}

Error DJIOSDKBridge::disconnect() {
    status_ = ConnectionStatus::DISCONNECTED;
    return Error::ok();
}

Option<DJIFlightData> DJIOSDKBridge::getFlightData() const {
    if (status_ != ConnectionStatus::CONNECTED) {
        return Option<DJIFlightData>::None();
    }
    return Option<DJIFlightData>::Some(last_data_);
}

Error DJIOSDKBridge::arm() {
    if (status_ != ConnectionStatus::CONNECTED) {
        return Error{ErrorKind::DJIBridgeError, 1, "DJI not connected", __FILE__, now_us()};
    }
    // TODO: 调用 OSDK arm API
    return Error::ok();
}

Error DJIOSDKBridge::disarm() {
    if (status_ != ConnectionStatus::CONNECTED) {
        return Error{ErrorKind::DJIBridgeError, 1, "DJI not connected", __FILE__, now_us()};
    }
    // TODO: 调用 OSDK disarm API
    return Error::ok();
}

Error DJIOSDKBridge::takeoff(f32 altitude_m) {
    if (status_ != ConnectionStatus::CONNECTED) {
        return Error{ErrorKind::DJIBridgeError, 1, "DJI not connected", __FILE__, now_us()};
    }
    // TODO: 调用 OSDK takeoff API
    (void)altitude_m;
    return Error::ok();
}

Error DJIOSDKBridge::land() {
    if (status_ != ConnectionStatus::CONNECTED) {
        return Error{ErrorKind::DJIBridgeError, 1, "DJI not connected", __FILE__, now_us()};
    }
    // TODO: 调用 OSDK landing API
    return Error::ok();
}

Error DJIOSDKBridge::goTo(const GeoPosition& target, f32 speed_ms) {
    if (status_ != ConnectionStatus::CONNECTED) {
        return Error{ErrorKind::DJIBridgeError, 1, "DJI not connected", __FILE__, now_us()};
    }
    // TODO: 调用 OSDK mission waypoint API
    (void)target;
    (void)speed_ms;
    return Error::ok();
}

Error DJIOSDKBridge::returnToHome() {
    if (status_ != ConnectionStatus::CONNECTED) {
        return Error{ErrorKind::DJIBridgeError, 1, "DJI not connected", __FILE__, now_us()};
    }
    // TODO: 调用 OSDK RTH API
    return Error::ok();
}

} // namespace FlyteOS::DJIBridge
