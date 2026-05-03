/**
 * @file main_simulator.cpp
 * @brief FlyteOS 模拟器主程序：桌面仿真环境入口
 */
#include "../src/core/system_manager.hpp"
#include "../src/flight_control/flight_controller.hpp"
#include <cstdio>
#include <csignal>
#include <chrono>
#include <thread>

using namespace FlyteOS;

static volatile bool g_running = true;

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║       FlyteOS Simulator - 武汉福莱特航空科技       ║\n");
    printf("╠═══════════════════════════════════════════════════╣\n");
    printf("║  Version: 1.0.0                                  ║\n");
    printf("║  Mode:    Desktop Simulation                     ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n\n");

    signal(SIGINT, signalHandler);

    Core::SystemManager sys;
    auto err = sys.init();
    if (err.is_err()) {
        printf("[FATAL] System init failed: %s\n", err.msg.c_str());
        return 1;
    }
    printf("[OK] System initialized, state: %s\n", sys.stateStr());

    // 主循环
    const f32 dt = 1.0f / 400.0f; // 400Hz
    u32 loop_count = 0;

    printf("[OK] Entering main loop (Ctrl+C to exit)\n\n");

    while (g_running) {
        err = sys.step(dt);
        if (err.is_err()) {
            printf("[ERROR] Step failed: %s\n", err.msg.c_str());
            break;
        }

        loop_count++;
        // 每400步(1秒)打印一次状态
        if (loop_count % 400 == 0) {
            printf("[INFO] Loop: %u, State: %s\n", loop_count, sys.stateStr());
        }

        std::this_thread::sleep_for(std::chrono::microseconds(2500)); // ~400Hz
    }

    printf("\n[OK] Shutting down...\n");
    sys.shutdown();
    printf("[OK] FlyteOS Simulator exited cleanly\n");

    (void)argc;
    (void)argv;
    return 0;
}
