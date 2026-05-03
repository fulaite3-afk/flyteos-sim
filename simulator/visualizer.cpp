/**
 * @file visualizer.cpp
 * @brief 控制台可视化：在终端输出飞行状态信息
 */
#include "visualizer.hpp"
#include <cstdio>
#include <cmath>

namespace FlyteOS::Simulator {

Visualizer::Visualizer() {}

void Visualizer::update(const PhysicsState& physics,
                         const Attitude::AttitudeEstimator::Estimate& attitude,
                         const Safety::SafetyStatus& safety) {
    frame_count_++;

    // 每10帧更新一次（降低刷新频率）
    if (frame_count_ % 10 != 0) return;

    Vec3 euler = attitude.euler_rad;
    f32 alt = -physics.position.down;
    f32 speed = sqrtf(physics.velocity.vn * physics.velocity.vn +
                      physics.velocity.ve * physics.velocity.ve);

    printf("\033[2J\033[H"); // 清屏
    printf("╔══════════════════════════════════════════╗\n");
    printf("║        FlyteOS 飞行仿真终端              ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  高度:  %8.1f m                      ║\n", alt);
    printf("║  速度:  %8.1f m/s                    ║\n", speed);
    printf("║  航向:  %8.1f °                      ║\n", euler.z * 180.0f / M_PI);
    printf("║  横滚:  %8.1f °                      ║\n", euler.x * 180.0f / M_PI);
    printf("║  俯仰:  %8.1f °                      ║\n", euler.y * 180.0f / M_PI);
    printf("║  N:     %8.1f m                      ║\n", physics.position.north);
    printf("║  E:     %8.1f m                      ║\n", physics.position.east);
    printf("║  安全:  %-6s                          ║\n", safety.safe ? "✓ OK" : "✗ ALERT");
    printf("╚══════════════════════════════════════════╝\n");

    if (!safety.warnings.empty()) {
        printf("[WARN] ");
        for (const auto& w : safety.warnings) {
            printf("%s | ", w.c_str());
        }
        printf("\n");
    }
}

void Visualizer::logEvent(const std::string& msg) {
    printf("[LOG] %s\n", msg.c_str());
}

} // namespace FlyteOS::Simulator
