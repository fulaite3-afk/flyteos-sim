/**
 * @file flyteos_types.cpp
 * @brief FlyteOS 全局类型的函数实现
 */
#include "flyteos_types.hpp"
#include <cmath>

namespace FlyteOS {

// ══════════════════ Vec3 ══════════════════════════════════════════
f32 Vec3::norm() const {
    return sqrtf(x*x + y*y + z*z);
}

Vec3 Vec3::normalized() const {
    f32 n = norm();
    if (n < 1e-6f) return {0, 0, 0};
    return {x/n, y/n, z/n};
}

// ══════════════════ Quat ══════════════════════════════════════════
void Quat::normalize() {
    f32 n = sqrtf(w*w + x*x + y*y + z*z);
    if (n < 1e-6f) { w=1; x=y=z=0; return; }
    w/=n; x/=n; y/=n; z/=n;
}

Vec3 Quat::toEuler() const {
    Vec3 e;
    // roll
    f32 sinr_cosp = 2*(w*x + y*z);
    f32 cosr_cosp = 1 - 2*(x*x + y*y);
    e.x = atan2f(sinr_cosp, cosr_cosp);
    // pitch
    f32 sinp = 2*(w*y - z*x);
    e.y = (fabsf(sinp) >= 1) ? copysignf(M_PI/2, sinp) : asinf(sinp);
    // yaw
    f32 siny_cosp = 2*(w*z + x*y);
    f32 cosy_cosp = 1 - 2*(y*y + z*z);
    e.z = atan2f(siny_cosp, cosy_cosp);
    return e;
}

Quat Quat::fromEuler(f32 roll, f32 pitch, f32 yaw) {
    f32 cr = cosf(roll*0.5f),  sr = sinf(roll*0.5f);
    f32 cp = cosf(pitch*0.5f), sp = sinf(pitch*0.5f);
    f32 cy = cosf(yaw*0.5f),   sy = sinf(yaw*0.5f);
    Quat q;
    q.w = cr*cp*cy + sr*sp*sy;
    q.x = sr*cp*cy - cr*sp*sy;
    q.y = cr*sp*cy + sr*cp*sy;
    q.z = cr*cp*sy - sr*sp*cy;
    return q;
}

// ══════════════════ FlightState 字符串 ════════════════════════════
const char* flightStateStr(FlightState s) {
    static const char* names[] = {
        "DISARMED", "STANDBY", "ARMED", "TAKING_OFF",
        "IN_FLIGHT", "HOVERING", "WAYPOINT_NAV", "LANDING",
        "EMERGENCY_LANDING", "FAILSAFE", "GROUND_ERROR"
    };
    auto idx = static_cast<unsigned>(s);
    return idx < sizeof(names)/sizeof(names[0]) ? names[idx] : "UNKNOWN";
}

} // namespace FlyteOS
