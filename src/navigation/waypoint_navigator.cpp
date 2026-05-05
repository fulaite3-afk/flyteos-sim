/**
 * @file waypoint_navigator.cpp
 * @brief 航点导航器实现
 */
#include "waypoint_navigator.hpp"
#include <cmath>
#include <algorithm>

namespace FlyteOS::Navigation {

WaypointNavigator::WaypointNavigator(Config cfg) : cfg_(cfg) {}

void WaypointNavigator::setWaypoints(const std::vector<Waypoint>& wps) {
    waypoints_ = wps;
    wp_idx_ = 0;
    state_ = NavState::IDLE;
}

void WaypointNavigator::addWaypoint(const Waypoint& wp) {
    waypoints_.push_back(wp);
}

void WaypointNavigator::clearWaypoints() {
    waypoints_.clear();
    wp_idx_ = 0;
    state_ = NavState::IDLE;
}

NavState WaypointNavigator::startNavigation() {
    if (waypoints_.empty()) return NavState::IDLE;
    wp_idx_ = 0;
    state_ = NavState::NAVIGATING;
    return state_;
}

NavState WaypointNavigator::startRTH(const NEDPosition& home) {
    home_pos_ = home;
    state_ = NavState::RTH;
    return state_;
}

void WaypointNavigator::abort() {
    state_ = NavState::ABORTED;
}

WaypointNavigator::NavOutput WaypointNavigator::update(const NEDPosition& current, f32 dt) {
    NavOutput out;
    out.state = state_;
    out.current_wp_idx = wp_idx_;
    out.desired_speed = cfg_.default_max_speed;

    if (state_ != NavState::NAVIGATING && state_ != NavState::RTH && state_ != NavState::LOITERING) {
        out.target_pos = current;
        out.dist_to_wp = 0;
        out.bearing_rad = 0;
        return out;
    }

    // RTH模式：飞回home
    if (state_ == NavState::RTH) {
        out.target_pos = home_pos_;
        f32 dn = home_pos_.north - current.north;
        f32 de = home_pos_.east  - current.east;
        out.dist_to_wp = sqrtf(dn*dn + de*de);
        out.bearing_rad = atan2f(de, dn);

        if (out.dist_to_wp < cfg_.default_accept_radius) {
            state_ = NavState::COMPLETED;
            out.state = state_;
        }
        return out;
    }

    // 盘旋等待
    if (state_ == NavState::LOITERING) {
        loiter_timer_ -= dt;
        if (loiter_timer_ <= 0) {
            state_ = NavState::NAVIGATING;
            wp_idx_++;
            if (wp_idx_ >= waypoints_.size()) {
                state_ = NavState::COMPLETED;
                out.state = state_;
                return out;
            }
        }
        out.target_pos = current;
        return out;
    }

    // 正常导航
    if (wp_idx_ >= waypoints_.size()) {
        state_ = NavState::COMPLETED;
        out.state = state_;
        return out;
    }

    const auto& wp = waypoints_[wp_idx_];
    // 简化：直接用NED目标（实际应从GPS转NED）
    NEDPosition target;
    target.north = static_cast<f32>((wp.pos.lat - 28.0000) * 111320.0f);
    target.east  = static_cast<f32>((wp.pos.lon - 112.0000) * 111320.0f * cosf(wp.pos.lat * M_PI / 180.0f));
    target.down  = -wp.pos.alt;

    f32 dn = target.north - current.north;
    f32 de = target.east  - current.east;
    out.target_pos = target;
    out.dist_to_wp = sqrtf(dn*dn + de*de);
    out.bearing_rad = atan2f(de, dn);

    // 到达减速
    if (out.dist_to_wp < cfg_.arrival_slowdown_dist) {
        f32 ratio = out.dist_to_wp / cfg_.arrival_slowdown_dist;
        out.desired_speed = wp.max_speed * std::max(ratio, 0.2f);
    }

    // 判断到达
    f32 radius = wp.accept_radius > 0 ? wp.accept_radius : cfg_.default_accept_radius;
    if (out.dist_to_wp < radius) {
        if (wp.loiter_time_s > 0) {
            state_ = NavState::LOITERING;
            loiter_timer_ = wp.loiter_time_s;
        } else {
            wp_idx_++;
            if (wp_idx_ >= waypoints_.size()) {
                state_ = NavState::COMPLETED;
            }
        }
    }

    out.state = state_;
    out.current_wp_idx = wp_idx_;
    return out;
}

const Waypoint& WaypointNavigator::currentWaypoint() const {
    return waypoints_[wp_idx_];
}

f32 WaypointNavigator::totalDistance() const {
    f32 dist = 0;
    for (u32 i = 1; i < waypoints_.size(); i++) {
        f32 dn = static_cast<f32>((waypoints_[i].pos.lat - waypoints_[i-1].pos.lat) * 111320.0f);
        f32 de = static_cast<f32>((waypoints_[i].pos.lon - waypoints_[i-1].pos.lon) * 111320.0f);
        dist += sqrtf(dn*dn + de*de);
    }
    return dist;
}

} // namespace FlyteOS::Navigation
