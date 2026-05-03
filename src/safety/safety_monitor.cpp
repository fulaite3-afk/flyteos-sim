/**
 * @file safety_monitor.cpp
 * @brief 安全监控器实现
 */
#include "safety_monitor.hpp"
#include <cmath>
#include <algorithm>

namespace FlyteOS::Safety {

SafetyMonitor::SafetyMonitor(Config cfg) : cfg_(cfg) {}

SafetyStatus SafetyMonitor::check() const {
    return SafetyStatus{}; // 默认安全
}

SafetyStatus SafetyMonitor::check(const EnvData& env) const {
    SafetyStatus status;

    // 高度检查
    if (env.altitude_m > cfg_.max_altitude_m) {
        status.safe = false;
        status.violations.push_back("Altitude exceeds maximum");
    }
    if (env.altitude_m < cfg_.min_altitude_m && env.altitude_m > 0) {
        status.warnings.push_back("Altitude below minimum");
    }

    // 电池检查
    if (env.battery_percent <= cfg_.battery_crit_percent) {
        status.safe = false;
        status.violations.push_back("Battery critically low");
    } else if (env.battery_percent <= cfg_.battery_warn_percent) {
        status.warnings.push_back("Battery low");
    }

    // 氦气压力检查
    if (env.helium_pressure >= cfg_.helium_crit_pressure) {
        status.safe = false;
        status.violations.push_back("Helium pressure critical");
    } else if (env.helium_pressure >= cfg_.helium_warn_pressure) {
        status.warnings.push_back("Helium pressure high");
    }

    // 风速检查
    if (env.wind_speed_ms > cfg_.max_wind_speed_ms) {
        status.safe = false;
        status.violations.push_back("Wind speed exceeds limit");
    }

    // 倾斜角检查
    if (env.tilt_rad > cfg_.max_tilt_rad) {
        status.warnings.push_back("Excessive tilt angle");
    }

    // 距离检查
    if (env.distance_home_m > cfg_.max_distance_m) {
        status.warnings.push_back("Distance from home exceeds limit");
    }

    // 地理围栏检查
    if (cfg_.geofence_enabled && !geofence_.empty()) {
        if (!isInsideGeofence(env.lat, env.lon)) {
            status.safe = false;
            status.violations.push_back("Geofence violation");
        }
    }

    return status;
}

void SafetyMonitor::setGeofence(const std::vector<GeofencePoint>& polygon) {
    geofence_ = polygon;
}

bool SafetyMonitor::isInsideGeofence(f64 lat, f64 lon) const {
    if (geofence_.size() < 3) return true; // 需要至少3个点形成多边形

    // 射线法判断点是否在多边形内
    bool inside = false;
    size_t n = geofence_.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if (((geofence_[i].lat > lat) != (geofence_[j].lat > lat)) &&
            (lon < (geofence_[j].lon - geofence_[i].lon) * (lat - geofence_[i].lat) /
                   (geofence_[j].lat - geofence_[i].lat) + geofence_[i].lon)) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace FlyteOS::Safety
