/**
 * @file main_sitl.cpp
 * @brief FlyteOS SITL仿真主程序 —— 闭环仿真入口
 *
 * 启动方式：
 *   ./flyteos_sitl                    # 默认参数
 *   ./flyteos_sitl --port 8765        # 指定WebSocket端口
 *   ./flyteos_sitl --wind 3.0         # 设置风速
 *   ./flyteos_sitl --help
 *
 * 访问：
 *   http://localhost:8765/           → 控制台
 *   http://localhost:8765/api/telemetry → JSON遥测
 */

#include "../src/sitl/sitl_manager.hpp"
#include "../src/bridge/ws_bridge.hpp"
#include <cstdio>
#include <csignal>
#include <cstring>
#include <chrono>
#include <thread>

using namespace FlyteOS;

static volatile bool g_running = true;

static void signalHandler(int) {
    g_running = false;
}

static void printHelp() {
    printf("FlyteOS SITL - Software-In-The-Loop Simulator\n");
    printf("武汉福莱特航空科技有限公司\n\n");
    printf("Usage: flyteos_sitl [options]\n\n");
    printf("Options:\n");
    printf("  --port PORT       WebSocket port (default: 8765)\n");
    printf("  --mass KG         Aircraft mass (default: 12.0)\n");
    printf("  --wind SPEED      Wind speed m/s (default: 0)\n");
    printf("  --volume M3       Gas cell volume (default: 120)\n");
    printf("  --bw RATIO        Initial buoyancy ratio (default: 0.95)\n");
    printf("  --help            Show this help\n\n");
    printf("After starting, open http://localhost:PORT/ in browser\n");
}

int main(int argc, char* argv[]) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║           FlyteOS SITL Simulator v2.0                    ║\n");
    printf("║           武汉福莱特航空科技有限公司                      ║\n");
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  Mode:  Software-In-The-Loop (闭环仿真)                  ║\n");
    printf("║  Bus:   FlyteBus (发布-订阅消息总线)                     ║\n");
    printf("║  Bridge: HTTP/SSE + POST (C++ ↔ HTML5)                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    signal(SIGINT, signalHandler);

    // ── 解析参数 ──
    Sim::SITLManager::Config sitl_cfg;
    Bridge::WSBridge::Config ws_cfg;
    f64 wind_speed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            ws_cfg.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mass") == 0 && i + 1 < argc) {
            sitl_cfg.aircraft_mass_kg = atof(argv[++i]);
        } else if (strcmp(argv[i], "--wind") == 0 && i + 1 < argc) {
            wind_speed = atof(argv[++i]);
        } else if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
            sitl_cfg.gas_cell_cfg.max_volume_m3 = atof(argv[++i]);
        } else if (strcmp(argv[i], "--bw") == 0 && i + 1 < argc) {
            // bw will be set in init
        } else if (strcmp(argv[i], "--help") == 0) {
            printHelp();
            return 0;
        }
    }

    if (wind_speed > 0) {
        sitl_cfg.wind_e_ms = wind_speed;  // 东风
    }

    // ── 初始化SITL ──
    Sim::SITLManager sitl(sitl_cfg);
    auto err = sitl.init();
    if (err.is_err()) {
        printf("[FATAL] SITL init failed: %s\n", err.msg.c_str());
        return 1;
    }
    printf("[OK] SITL initialized\n");

    // ── 启动WebSocket桥接 ──
    Bridge::WSBridge bridge(ws_cfg);
    if (!bridge.start(&sitl)) {
        printf("[FATAL] WebSocket bridge failed to start\n");
        return 1;
    }
    printf("[OK] WebSocket bridge on port %d\n", ws_cfg.port);
    printf("[OK] Open http://localhost:%d/ in browser\n\n", ws_cfg.port);

    // ── 主仿真循环 ──
    printf("[OK] SITL loop running (Ctrl+C to stop)\n");
    printf("────────────────────────────────────────────\n");

    u64 last_status = 0;
    u64 step_at_sec = 0;

    while (g_running) {
        // 推进一步SITL（400Hz目标）
        err = sitl.step();
        if (err.is_err()) {
            printf("[ERROR] Step %llu: %s\n",
                   (unsigned long long)sitl.stepCount(), err.msg.c_str());
            break;
        }

        // 每秒打印一次状态
        u64 now_sec = static_cast<u64>(sitl.simTime());
        if (now_sec > last_status) {
            last_status = now_sec;
            auto snap = sitl.telemetry();
            f32 alt = -snap.pos_d;
            f32 spd = sqrtf(snap.vel_n * snap.vel_n + snap.vel_e * snap.vel_e);

            printf("[T=%5.0fs] ALT=%6.1fm  SPD=%4.1fm/s  B/W=%.3f  "
                   "F_VOL=%.0fm³  F_T=%.1f°C  THR=%.1fN  %s\n",
                   sitl.simTime(), alt, spd,
                   snap.buoyancy_ratio,
                   snap.gas_volume_m3,
                   snap.gas_temp_k - 273.15,
                   snap.thrust_n,
                   [](FlightState s) -> const char* {
                       switch(s) {
                           case FlightState::DISARMED: return "DISARMED";
                           case FlightState::STANDBY: return "STANDBY";
                           case FlightState::TAKING_OFF: return "TAKING_OFF";
                           case FlightState::IN_FLIGHT: return "IN_FLIGHT";
                           case FlightState::HOVERING: return "HOVERING";
                           case FlightState::LANDING: return "LANDING";
                           case FlightState::EMERGENCY_LANDING: return "EMERGENCY!";
                           default: return "?";
                       }
                   }(snap.flight_state)
            );
        }

        // 仿真步长控制（1/400s ≈ 2500μs）
        std::this_thread::sleep_for(std::chrono::microseconds(2500));
    }

    // ── 清理 ──
    printf("\n────────────────────────────────────────────\n");
    bridge.stop();
    printf("[OK] FlyteOS SITL exited cleanly after %.1f seconds\n", sitl.simTime());
    printf("[OK] Total steps: %llu\n", (unsigned long long)sitl.stepCount());
    return 0;
}
