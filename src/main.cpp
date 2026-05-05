/**
 * @file main.cpp
 * @brief FlyteOS 嵌入式主程序入口（STM32/嵌入式目标）
 *        桌面模拟器请使用 simulator/main_simulator.cpp
 */
#include "core/system_manager.hpp"
#include <cstdio>

using namespace FlyteOS;

int main() {
    printf("[FlyteOS] Initializing embedded system...\n");

    Core::SystemManager sys;
    auto err = sys.init();
    if (err.is_err()) {
        printf("[FATAL] System init failed: %s\n", err.msg.c_str());
        return 1;
    }

    printf("[FlyteOS] System ready, state: %s\n", sys.stateStr());

    // 嵌入式环境下，主循环由RTOS调度
    // 此处为裸机简化示例
    const f32 dt = 1.0f / 400.0f;
    while (true) {
        err = sys.step(dt);
        if (err.is_err()) {
            printf("[ERROR] Control loop error: %s\n", err.msg.c_str());
            break;
        }
    }

    sys.shutdown();
    return 0;
}
