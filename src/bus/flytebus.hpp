#pragma once
/**
 * @file flytebus.hpp
 * @brief FlyteBus 消息总线 —— 类似PX4 uORB的发布-订阅通信层
 *
 * 设计目标：
 *   - 模块解耦：飞控、导航、浮力、安全各模块不直接调用，通过总线通信
 *   - 类型安全：每条消息通道绑定具体类型，编译期检查
 *   - 零拷贝：发布者和订阅者共享同一份消息内存
 *   - 多订阅者：一条消息可被多个模块订阅
 *
 * 用法：
 *   // 发布
 *   FlyteBus::publish<ImuData>(imu);
 *
 *   // 订阅
 *   auto sub = FlyteBus::subscribe<ImuData>();
 *   auto* data = sub.get();  // 获取最新数据，可能为nullptr
 */

#include "../../include/flyteos_types.hpp"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <functional>
#include <typeindex>
#include <memory>
#include <cstdio>

namespace FlyteOS::Bus {

// ════════════════════════════════════════════════════════════════
//  订阅令牌
// ════════════════════════════════════════════════════════════════

/// 订阅句柄，用于读取最新消息
template<typename T>
class Subscription {
public:
    Subscription() : ptr_(nullptr) {}
    explicit Subscription(const T* ptr) : ptr_(ptr) {}

    /// 获取最新消息指针（可能为nullptr如果尚无发布）
    const T* get() const { return ptr_; }

    /// 是否有数据
    bool has() const { return ptr_ != nullptr; }

    /// 获取数据或默认值
    const T& value_or(const T& def) const {
        return ptr_ ? *ptr_ : def;
    }

private:
    const T* ptr_;
};

// ════════════════════════════════════════════════════════════════
//  消息通道内部存储
// ════════════════════════════════════════════════════════════════

/// 单个消息通道：存储一条最新消息 + 更新时间戳
struct ChannelBase {
    virtual ~ChannelBase() = default;
    virtual std::type_index type() const = 0;
    TimeUs last_update = 0;
    u32    pub_count   = 0;   // 发布次数统计
};

template<typename T>
struct Channel : public ChannelBase {
    T data;
    bool has_data = false;

    std::type_index type() const override {
        return std::type_index(typeid(T));
    }
};

// ════════════════════════════════════════════════════════════════
//  FlyteBus 核心
// ════════════════════════════════════════════════════════════════

class FlyteBus {
public:
    /// 发布消息（全特化）
    template<typename T>
    static void publish(const T& msg) {
        auto& bus = instance();
        std::lock_guard<std::mutex> lock(bus.mutex_);

        auto key = std::type_index(typeid(T));
        auto it = bus.channels_.find(key);
        if (it == bus.channels_.end()) {
            auto ch = std::make_unique<Channel<T>>();
            ch->data = msg;
            ch->has_data = true;
            ch->last_update = now_us();
            ch->pub_count = 1;
            bus.channels_[key] = std::move(ch);
        } else {
            auto* ch = static_cast<Channel<T>*>(it->second.get());
            ch->data = msg;
            ch->has_data = true;
            ch->last_update = now_us();
            ch->pub_count++;
        }
    }

    /// 订阅消息
    template<typename T>
    static Subscription<T> subscribe() {
        auto& bus = instance();
        std::lock_guard<std::mutex> lock(bus.mutex_);

        auto key = std::type_index(typeid(T));
        auto it = bus.channels_.find(key);
        if (it == bus.channels_.end()) {
            // 通道不存在，创建空通道
            bus.channels_[key] = std::make_unique<Channel<T>>();
            return Subscription<T>(nullptr);
        }
        auto* ch = static_cast<Channel<T>*>(it->second.get());
        return Subscription<T>(ch->has_data ? &ch->data : nullptr);
    }

    /// 获取通道统计信息
    template<typename T>
    static bool hasData() {
        auto& bus = instance();
        std::lock_guard<std::mutex> lock(bus.mutex_);
        auto key = std::type_index(typeid(T));
        auto it = bus.channels_.find(key);
        if (it == bus.channels_.end()) return false;
        return static_cast<Channel<T>*>(it->second.get())->has_data;
    }

    /// 清除所有通道
    static void reset() {
        auto& bus = instance();
        std::lock_guard<std::mutex> lock(bus.mutex_);
        bus.channels_.clear();
    }

    /// 获取通道数量
    static size_t channelCount() {
        auto& bus = instance();
        std::lock_guard<std::mutex> lock(bus.mutex_);
        return bus.channels_.size();
    }

    /// 打印所有通道状态（调试用）
    static void printStatus() {
        auto& bus = instance();
        std::lock_guard<std::mutex> lock(bus.mutex_);
        printf("[FlyteBus] %zu channels active\n", bus.channels_.size());
        for (auto& [key, ch] : bus.channels_) {
            printf("  %s: pub=%u, last_update=%llu\n",
                   key.name(), ch->pub_count,
                   (unsigned long long)ch->last_update);
        }
    }

private:
    FlyteBus() = default;

    static FlyteBus& instance() {
        static FlyteBus bus;
        return bus;
    }

    std::unordered_map<std::type_index, std::unique_ptr<ChannelBase>> channels_;
    std::mutex mutex_;
};

// ════════════════════════════════════════════════════════════════
//  标准消息类型定义（FlyteOS各模块间通信协议）
// ════════════════════════════════════════════════════════════════

/// IMU原始数据
struct MsgImuRaw {
    f32 gyro[3]   = {};   // rad/s
    f32 accel[3]  = {};   // m/s²
    f32 mag[3]    = {};   // μT
    TimeUs ts     = 0;
};

/// GPS原始数据
struct MsgGpsRaw {
    f64 lat      = 0;    // deg
    f64 lon      = 0;    // deg
    f32 alt_msl  = 0;    // m
    f32 vel_ned[3] = {}; // m/s
    f32 hdop      = 0;
    u8  fix       = 0;
    TimeUs ts     = 0;
};

/// 气压计数据
struct MsgBaro {
    f32 altitude_m  = 0;
    f32 pressure_pa = 101325;
    f32 temperature_k = 288.15;
    TimeUs ts = 0;
};

/// 姿态估计结果
struct MsgAttitudeEstimate {
    f32 roll_rad   = 0;
    f32 pitch_rad  = 0;
    f32 yaw_rad    = 0;
    f32 roll_rate  = 0;
    f32 pitch_rate = 0;
    f32 yaw_rate   = 0;
    f32 altitude_m = 0;
    TimeUs ts = 0;
};

/// 位置估计结果
struct MsgPositionEstimate {
    f32 north_m = 0;
    f32 east_m  = 0;
    f32 down_m  = 0;
    f32 vn_ms   = 0;
    f32 ve_ms   = 0;
    f32 vd_ms   = 0;
    TimeUs ts = 0;
};

/// 飞控姿态指令（速度控制器输出）
struct MsgAttitudeCmd {
    f32 roll_rad   = 0;
    f32 pitch_rad  = 0;
    f32 yaw_rad    = 0;
    f32 thrust_01  = 0;  // 0~1
    TimeUs ts = 0;
};

/// 执行器输出（混控后）
struct MsgActuatorOutput {
    f32 motor[4]  = {};  // 0~1
    f32 vent_open = 0;    // 放气阀 0~1
    f32 pump_01   = 0;    // 压气机 0~1
    f32 thrust_n  = 0;
    TimeUs ts = 0;
};

/// 气囊状态
struct MsgGasCellState {
    f64 volume_m3    = 0;
    f64 pressure_pa  = 0;
    f64 temperature_k = 0;
    f64 buoyancy_n   = 0;
    f64 net_lift_kg  = 0;
    f64 buoyancy_ratio = 0;  // B/W
    f64 vent_open    = 0;
    f64 pump_active  = 0;
    TimeUs ts = 0;
};

/// 环境数据
struct MsgEnvironment {
    f64 altitude_m       = 0;
    f64 air_temp_k       = 288.15;
    f64 air_pressure_pa = 101325;
    f64 wind_n_ms        = 0;
    f64 wind_e_ms        = 0;
    f64 wind_d_ms        = 0;
    f64 solar_elevation_rad = 0;
    f64 solar_irradiance_wm2 = 0;
    TimeUs ts = 0;
};

/// 飞行状态
struct MsgFlightState {
    FlightState state = FlightState::DISARMED;
    TimeUs ts = 0;
};

/// 安全状态
struct MsgSafetyStatus {
    bool safe = true;
    f32 max_altitude_m = 300;
    f32 geofence_radius_m = 5000;
    std::vector<std::string> warnings;
    TimeUs ts = 0;
};

/// 电池状态
struct MsgBattery {
    f32 voltage_v = 22.2f;
    f32 current_a = 0;
    f32 remaining_pct = 100;
    TimeUs ts = 0;
};

/// 遥控指令（操作员输入）
struct MsgRcInput {
    f32 roll     = 0;   // -1 ~ 1
    f32 pitch    = 0;
    f32 yaw      = 0;
    f32 throttle = 0;   // 0 ~ 1
    bool arm     = false;
    bool takeoff = false;
    bool land    = false;
    bool emergency = false;
    TimeUs ts = 0;
};

} // namespace FlyteOS::Bus
