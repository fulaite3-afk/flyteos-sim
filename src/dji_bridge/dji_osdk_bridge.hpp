#pragma once
/**
 * @file dji_osdk_bridge.hpp
 * @brief DJI OSDK 桥接层：对接大疆 Onboard SDK
 *        当前为接口定义，实际 OSDK 集成在 Phase 1 实现
 */
#include "../../include/flyteos_types.hpp"

namespace FlyteOS::DJIBridge {

enum class ConnectionStatus : u8 {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

struct DJIFlightData {
    f64 lat = 0, lon = 0;
    f32 alt = 0;
    f32 vx = 0, vy = 0, vz = 0;
    f32 roll = 0, pitch = 0, yaw = 0;
    f32 battery_percent = 0;
    TimeUs ts = 0;
};

class DJIOSDKBridge {
public:
    struct Config {
        std::string app_id;
        std::string app_key;
        std::string device_port = "/dev/ttyUSB0";
        u32 baud_rate = 230400;
    };

    explicit DJIOSDKBridge(Config cfg);
    DJIOSDKBridge() : DJIOSDKBridge(Config{}) {}

    // 连接管理
    Error connect();
    Error disconnect();
    ConnectionStatus status() const { return status_; }

    // 数据获取
    Option<DJIFlightData> getFlightData() const;

    // 飞行控制
    Error arm();
    Error disarm();
    Error takeoff(f32 altitude_m);
    Error land();
    Error goTo(const GeoPosition& target, f32 speed_ms);
    Error returnToHome();

    // 订阅回调
    using FlightDataCallback = std::function<void(const DJIFlightData&)>;
    void setFlightDataCallback(FlightDataCallback cb) { flight_data_cb_ = cb; }

private:
    Config cfg_;
    ConnectionStatus status_ = ConnectionStatus::DISCONNECTED;
    DJIFlightData last_data_;
    FlightDataCallback flight_data_cb_;
};

} // namespace FlyteOS::DJIBridge
