#pragma once
/**
 * @file waypoint_navigator.hpp
 * @brief 航点导航器：航路规划、航点跟踪、自动返航
 */
#include "../../include/flyteos_types.hpp"
#include <vector>

namespace FlyteOS::Navigation {

struct Waypoint {
    GeoPosition pos;
    f32         max_speed     = 5.0f;    // m/s
    f32         accept_radius = 5.0f;    // m
    f32         loiter_time_s = 0.0f;    // 盘旋等待时间
    std::string name;
};

enum class NavState : u8 {
    IDLE,
    NAVIGATING,
    LOITERING,
    APPROACHING,
    RTH,            // Return To Home
    COMPLETED,
    ABORTED
};

class WaypointNavigator {
public:
    struct Config {
        f32 default_accept_radius = 5.0f;   // m
        f32 default_max_speed     = 5.0f;   // m/s
        f32 rth_altitude          = 50.0f;  // m 返航高度
        f32 arrival_slowdown_dist = 30.0f;  // m 到达减速距离
    };

    struct NavOutput {
        NEDPosition target_pos;     // 目标位置
        f32         desired_speed;  // 期望速度 m/s
        NavState    state;
        u32         current_wp_idx;
        f32         dist_to_wp;    // 到当前航点距离 m
        f32         bearing_rad;   // 到当前航点方位 rad
    };

    explicit WaypointNavigator(Config cfg);
    WaypointNavigator() : WaypointNavigator(Config{}) {}

    // 航路管理
    void setWaypoints(const std::vector<Waypoint>& wps);
    void addWaypoint(const Waypoint& wp);
    void clearWaypoints();
    u32  waypointCount() const { return static_cast<u32>(waypoints_.size()); }

    // 导航控制
    NavOutput update(const NEDPosition& current, f32 dt);
    NavState  startNavigation();
    NavState  startRTH(const NEDPosition& home);
    void      abort();

    // 状态查询
    NavState state() const { return state_; }
    const Waypoint& currentWaypoint() const;
    f32 totalDistance() const;

private:
    Config cfg_;
    std::vector<Waypoint> waypoints_;
    NavState state_ = NavState::IDLE;
    u32      wp_idx_ = 0;
    NEDPosition home_pos_;
    f32    loiter_timer_ = 0;
};

} // namespace FlyteOS::Navigation
