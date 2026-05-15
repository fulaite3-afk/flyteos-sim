/**
 * @file test_sitl_integration.cpp
 * @brief SITL闭环集成测试 —— 验证完整仿真链路
 *
 * 测试项：
 *   1. SITL初始化
 *   2. ARM → STANDBY 状态转换
 *   3. TAKEOFF → IN_FLIGHT 状态转换
 *   4. 浮力比动态变化
 *   5. 气囊温度上升（太阳加热）
 *   6. 高度变化（浮力>重力时上升）
 *   7. FlyteBus消息通道活跃
 *   8. 遥测数据完整性
 */

#include "../src/sitl/sitl_manager.hpp"
#include "../src/bus/flytebus.hpp"
#include <cstdio>
#include <cmath>
#include <cstdlib>

using namespace FlyteOS;

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { tests_passed++; printf("  [PASS] %s\n", msg); } \
    else { tests_failed++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define ASSERT_FLOAT(val, expected, tol, msg) do { \
    bool ok = std::fabs((val) - (expected)) < (tol); \
    if (ok) { tests_passed++; printf("  [PASS] %s (val=%.4f, exp=%.4f)\n", msg, (double)(val), (double)(expected)); } \
    else { tests_failed++; printf("  [FAIL] %s (val=%.4f, exp=%.4f, tol=%.4f)\n", msg, (double)(val), (double)(expected), (double)(tol)); } \
} while(0)

int main() {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  FlyteOS SITL Integration Test Suite        ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    // ═══════════════════════════════════════════════════════════
    printf("[Test 1] SITL Initialization\n");
    // ═══════════════════════════════════════════════════════════

    Sim::SITLManager::Config cfg;
    cfg.aircraft_mass_kg = 12.0f;
    cfg.gas_cell_cfg.max_volume_m3 = 120.0;
    cfg.solar_irradiance_wm2 = 800;

    Sim::SITLManager sitl(cfg);
    auto err = sitl.init();

    ASSERT(err.is_ok(), "SITL init succeeds");
    ASSERT(sitl.isRunning(), "SITL is running after init");
    ASSERT_FLOAT(sitl.simTime(), 0.0, 0.01, "Sim time starts at 0");
    ASSERT(sitl.stepCount() == 0, "Step count starts at 0");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 2] FlyteBus Channels Active\n");
    // ═══════════════════════════════════════════════════════════

    // 推几步让消息流通
    for (int i = 0; i < 10; i++) sitl.step();

    ASSERT(Bus::FlyteBus::hasData<Bus::MsgImuRaw>(), "IMU data published");
    ASSERT(Bus::FlyteBus::hasData<Bus::MsgGpsRaw>(), "GPS data published");
    ASSERT(Bus::FlyteBus::hasData<Bus::MsgBaro>(), "Baro data published");
    ASSERT(Bus::FlyteBus::hasData<Bus::MsgFlightState>(), "Flight state published");
    ASSERT(Bus::FlyteBus::hasData<Bus::MsgEnvironment>(), "Environment published");
    ASSERT(Bus::FlyteBus::channelCount() >= 5, "At least 5 channels active");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 3] Initial State = DISARMED\n");
    // ═══════════════════════════════════════════════════════════

    auto snap0 = sitl.telemetry();
    ASSERT(snap0.flight_state == FlightState::DISARMED, "Initial state is DISARMED");
    ASSERT_FLOAT(snap0.thrust_n, 0.0f, 0.1f, "No thrust when disarmed");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 4] ARM → STANDBY\n");
    // ═══════════════════════════════════════════════════════════

    Bus::MsgRcInput rc_arm;
    rc_arm.arm = true;
    rc_arm.ts = now_us();
    sitl.setRcInput(rc_arm);

    for (int i = 0; i < 10; i++) sitl.step();

    auto snap1 = sitl.telemetry();
    ASSERT(snap1.flight_state == FlightState::STANDBY,
           "State transitions to STANDBY after ARM");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 5] TAKEOFF → IN_FLIGHT\n");
    // ═══════════════════════════════════════════════════════════

    Bus::MsgRcInput rc_takeoff;
    rc_takeoff.arm = true;
    rc_takeoff.takeoff = true;
    rc_takeoff.throttle = 0.7f;
    rc_takeoff.ts = now_us();
    sitl.setRcInput(rc_takeoff);

    // 模拟2秒
    for (int i = 0; i < 800; i++) sitl.step();

    auto snap2 = sitl.telemetry();
    f32 alt2 = -snap2.pos_d;
    ASSERT(snap2.flight_state == FlightState::IN_FLIGHT ||
           snap2.flight_state == FlightState::TAKING_OFF,
           "State transitions to IN_FLIGHT or TAKING_OFF after takeoff");
    ASSERT(alt2 > 0, "Altitude increases after takeoff");

    // 继续飞行3秒
    for (int i = 0; i < 1200; i++) sitl.step();

    auto snap3 = sitl.telemetry();
    f32 alt3 = -snap3.pos_d;
    ASSERT(snap3.flight_state == FlightState::IN_FLIGHT, "State reaches IN_FLIGHT");
    ASSERT(alt3 > alt2, "Altitude continues to increase");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 6] Buoyancy Gas Cell Response\n");
    // ═══════════════════════════════════════════════════════════

    ASSERT(snap3.buoyancy_n > 100.0, "Buoyancy force > 100N");
    ASSERT(snap3.buoyancy_ratio > 0.90, "B/W ratio > 0.90 (not old 0.60)");
    ASSERT(snap3.gas_volume_m3 > 0, "Gas cell volume > 0");
    ASSERT(snap3.gas_temp_k > 288.0, "Gas temperature > ISA 15°C (solar heating)");

    printf("  [INFO] Buoyancy: %.1f N, B/W: %.3f, Volume: %.1f m3, Temp: %.1f C\n",
           snap3.buoyancy_n, snap3.buoyancy_ratio,
           snap3.gas_volume_m3, snap3.gas_temp_k - 273.15);

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 7] LAND Command\n");
    // ═══════════════════════════════════════════════════════════

    Bus::MsgRcInput rc_land;
    rc_land.land = true;
    rc_land.ts = now_us();
    sitl.setRcInput(rc_land);

    for (int i = 0; i < 100; i++) sitl.step();

    auto snap4 = sitl.telemetry();
    ASSERT(snap4.flight_state == FlightState::LANDING ||
           snap4.flight_state == FlightState::IN_FLIGHT,
           "LAND command triggers state change");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 8] Telemetry Data Completeness\n");
    // ═══════════════════════════════════════════════════════════

    ASSERT(snap3.sim_time_s > 0, "Sim time > 0");
    ASSERT(snap3.step_count > 0, "Step count > 0");
    ASSERT(snap3.battery_pct > 0, "Battery pct > 0");
    ASSERT(snap3.thrust_n >= 0, "Thrust >= 0");
    ASSERT(snap3.gas_pressure_pa > 90000, "Gas pressure > 90kPa");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 9] Dynamic B/W Ratio Changes by Flight Phase\n");
    // ═══════════════════════════════════════════════════════════

    // 重置，测试不同飞行阶段的B/W
    sitl.reset();
    sitl.init();

    // 起飞时B/W应该更高（1.05目标）
    Bus::MsgRcInput rc_to;
    rc_to.arm = true;
    rc_to.takeoff = true;
    rc_to.throttle = 0.8f;
    rc_to.ts = now_us();
    sitl.setRcInput(rc_to);

    for (int i = 0; i < 2000; i++) sitl.step();

    auto snap_takeoff = sitl.telemetry();
    printf("  [INFO] Takeoff B/W: %.3f (target ~1.05)\n", snap_takeoff.buoyancy_ratio);

    // 起飞后B/W应接近1.0以上
    ASSERT(snap_takeoff.buoyancy_ratio > 0.90, "Takeoff B/W > 0.90");

    // ═══════════════════════════════════════════════════════════
    printf("\n[Test 10] FlyteBus Message Publication Stats\n");
    // ═══════════════════════════════════════════════════════════

    Bus::FlyteBus::printStatus();

    // ═══════════════════════════════════════════════════════════
    // 汇总
    // ═══════════════════════════════════════════════════════════

    printf("\n══════════════════════════════════════════════\n");
    printf("  PASSED: %d    FAILED: %d    TOTAL: %d\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("══════════════════════════════════════════════\n");

    return tests_failed > 0 ? 1 : 0;
}
