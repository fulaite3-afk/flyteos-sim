#pragma once
/**
 * @file safety_monitor.hpp
 * @brief 安全监控器：地理围栏、电池监控、氦气压力、紧急处理
 */
#include "../../include/flyteos_types.hpp"
#include <vector>
#include <string>

namespace FlyteOS::Safety {

struct GeofencePoint {
    f64 lat;
    f64 lon;
};

struct SafetyStatus {
    bool safe = true;
    std::vector<std::string> violations;
    std::vector<std::string> warnings;
};

class SafetyMonitor {
public:
    struct Config {
        f32 max_altitude_m          = 300.0f;
        f32 min_altitude_m          = 5.0f;
        f32 max_distance_m          = 2000.0f;
        f32 battery_warn_percent    = 30.0f;
        f32 battery_crit_percent    = 15.0f;
        f32 helium_warn_pressure    = 1.15f;   // atm
        f32 helium_crit_pressure    = 1.30f;   // atm
        f32 max_wind_speed_ms       = 15.0f;
        f32 max_tilt_rad            = 0.52f;   // ~30度
        bool geofence_enabled       = true;
    };

    struct EnvData {
        f32 altitude_m       = 0;
        f32 battery_percent  = 100;
        f32 helium_pressure  = 1.0f;
        f32 wind_speed_ms    = 0;
        f32 tilt_rad         = 0;
        f64 lat              = 30.5023;
        f64 lon              = 114.4047;
        f32 distance_home_m  = 0;
    };

    explicit SafetyMonitor(Config cfg);
    SafetyMonitor() : SafetyMonitor(Config{}) {}

    SafetyStatus check() const;
    SafetyStatus check(const EnvData& env) const;

    // 地理围栏
    void setGeofence(const std::vector<GeofencePoint>& polygon);
    bool isInsideGeofence(f64 lat, f64 lon) const;

    const Config& config() const { return cfg_; }
    void setConfig(const Config& c) { cfg_ = c; }

private:
    Config cfg_;
    std::vector<GeofencePoint> geofence_;
};

} // namespace FlyteOS::Safety
