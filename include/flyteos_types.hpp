#pragma once
/**
 * @file flyteos_types.hpp
 * @brief FlyteOS 全局类型定义
 * @company 武汉福莱特航空科技有限公司
 * @version 1.0.0
 *
 * 借鉴 Rust 的强类型、Result/Option 模式，实现类型安全的飞控系统
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <chrono>

namespace FlyteOS {

// ════════════════════════════════════════════════════════════════
//  基础数值类型
// ════════════════════════════════════════════════════════════════

using f32 = float;
using f64 = double;
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using TimeUs = uint64_t;   // 微秒时间戳

inline TimeUs now_us() {
    return static_cast<TimeUs>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

// ════════════════════════════════════════════════════════════════
//  Result / Option 类型（借鉴 Rust）
// ════════════════════════════════════════════════════════════════

enum class ErrorKind : u32 {
    None = 0,
    // 系统级
    SystemInitFailed,
    HardwareFault,
    WatchdogTimeout,
    // 传感器
    SensorTimeout,
    SensorDataInvalid,
    SensorNotAvailable,
    // 控制
    ControlLoopFailure,
    ActuatorSaturation,
    AttitudeLimitReached,
    // 通信
    CommTimeout,
    DJIBridgeError,
    // 安全
    GeofenceViolation,
    BatteryLow,
    HeliumPressureCritical,
    // 任务
    MissionAborted,
    EmergencyLandTriggered,
};

struct Error {
    ErrorKind kind   = ErrorKind::None;
    u32       code   = 0;
    std::string msg;
    std::string src;
    TimeUs      ts   = 0;

    static Error ok()  { return {}; }
    bool is_ok()  const { return kind == ErrorKind::None; }
    bool is_err() const { return !is_ok(); }
};

template<typename T>
struct Result {
    T     value{};
    Error error;

    static Result<T> Ok(T v)        { Result<T> r; r.value = v; return r; }
    static Result<T> Err(Error e)   { Result<T> r; r.error = e; return r; }

    bool is_ok()  const { return error.is_ok(); }
    bool is_err() const { return error.is_err(); }
    T    unwrap_or(T def) const { return is_ok() ? value : def; }
};

template<typename T>
struct Option {
    bool has;
    T    value{};

    static Option<T> Some(T v) { return {true, v};  }
    static Option<T> None()    { return {false, {}};  }

    bool is_some() const { return has; }
    bool is_none() const { return !has; }
    T    unwrap_or(T def) const { return has ? value : def; }
};

// ════════════════════════════════════════════════════════════════
//  三维向量 / 四元数
// ════════════════════════════════════════════════════════════════

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(f32 s)         const { return {x*s, y*s, z*s}; }
    f32  norm()                   const;
    Vec3 normalized()             const;
};

struct Quat {
    f32 w = 1, x = 0, y = 0, z = 0;
    void normalize();
    Vec3 toEuler() const;       // roll, pitch, yaw  (rad)
    static Quat fromEuler(f32 roll, f32 pitch, f32 yaw);
};

// ════════════════════════════════════════════════════════════════
//  GPS / 位置
// ════════════════════════════════════════════════════════════════

struct GeoPosition {
    f64   lat = 0;       // 纬度 (deg)
    f64   lon = 0;       // 经度 (deg)
    f32   alt = 0;       // 高度 MSL (m)
    TimeUs ts = 0;
};

struct NEDPosition {
    f32 north = 0, east = 0, down = 0;
    TimeUs ts = 0;
};

struct NEDVelocity {
    f32 vn = 0, ve = 0, vd = 0;
    TimeUs ts = 0;
};

// ════════════════════════════════════════════════════════════════
//  飞行器状态机状态
// ════════════════════════════════════════════════════════════════

enum class FlightState : u8 {
    DISARMED = 0,
    STANDBY,
    ARMED,
    TAKING_OFF,
    IN_FLIGHT,
    HOVERING,
    WAYPOINT_NAV,
    LANDING,
    EMERGENCY_LANDING,
    FAILSAFE,
    GROUND_ERROR,
};

const char* flightStateStr(FlightState s);

// ════════════════════════════════════════════════════════════════
//  飞控命令
// ════════════════════════════════════════════════════════════════

struct AttitudeCmd {
    f32 roll    = 0;    // rad
    f32 pitch   = 0;    // rad
    f32 yaw     = 0;    // rad
    f32 thrust  = 0;    // 0.0~1.0
};

struct VelocityCmd {
    f32 vn = 0, ve = 0, vd = 0;   // m/s NED
    f32 yaw_rate = 0;              // rad/s
};

struct PositionCmd {
    GeoPosition target;
    f32 max_speed    = 5.0f;      // m/s
    f32 accept_radius = 2.0f;     // m
};

// ════════════════════════════════════════════════════════════════
//  执行器输出
// ════════════════════════════════════════════════════════════════

struct ActuatorOutput {
    f32 motor[4]  = {};            // PWM 0.0~1.0（对应1000~2000μs）
    f32 thrust_n  = 0;             // 总推力 N
    f32 torque[3] = {};            // 力矩 Nm (roll,pitch,yaw)

    // ── 第5轴：浮力控制（浮空器专属）──
    f32 vent_open = 0;             // 放气阀开度 0.0~1.0
    f32 pump_01   = 0;             // 压气机状态 0.0~1.0
    f32 buoyancy_cmd_n = 0;        // 浮力指令输出 N（用于日志/遥测）

    TimeUs ts = 0;
};

} // namespace FlyteOS
