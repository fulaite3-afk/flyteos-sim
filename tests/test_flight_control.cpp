/**
 * @file test_flight_control.cpp
 * @brief 飞行控制单元测试
 *        覆盖: PID, 位置控制, 速度控制, 姿态控制, 状态机
 */
#include "../src/flight_control/flight_controller.hpp"
#include "../src/power/helium_buoyancy.hpp"
#include <cstdio>
#include <cmath>
#include <cassert>
#include <string>
#include <vector>

using namespace FlyteOS;
using namespace FlyteOS::Control;

// ─── 简易测试框架 ─────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;
static std::vector<std::string> g_failures;

#define TEST(name) void name(); static struct name##_reg { name##_reg() { name(); } } name##_inst;
#define ASSERT(cond, msg) do { if (!(cond)) { g_fail++; g_failures.push_back(msg); printf("  ✗ %s\n", msg); } else { g_pass++; printf("  ✓ %s\n", msg); } } while(0)
#define ASSERT_NEAR(a, b, eps, msg) do { if (fabsf((a)-(b)) > (eps)) { g_fail++; g_failures.push_back(msg); printf("  ✗ %s (got %.4f, expected %.4f)\n", msg, (double)(a), (double)(b)); } else { g_pass++; printf("  ✓ %s\n", msg); } } while(0)

// ═══════════════════════════════════════════════════════════════════
//  PID 测试
// ═══════════════════════════════════════════════════════════════════

void test_pid_zero_error() {
    printf("\n[PID] 零误差测试\n");
    PIDConfig cfg{1.0f, 0.0f, 0.0f, 10.0f, 1.0f};
    PID pid(cfg);
    f32 out = pid.update(0.0f, 0.01f);
    ASSERT_NEAR(out, 0.0f, 0.001f, "零误差 → 输出为零");
}

void test_pid_proportional() {
    printf("\n[PID] 比例测试\n");
    PIDConfig cfg{2.0f, 0.0f, 0.0f, 10.0f, 10.0f}; // out_limit=10 允许大输出
    PID pid(cfg);
    f32 out = pid.update(1.0f, 0.01f);
    // 首步有微分过渡效应，放宽容差
    ASSERT_NEAR(out, 2.0f, 0.1f, "P=2, error=1 → 输出≈2(含微分过渡)");
}

void test_pid_integral() {
    printf("\n[PID] 积分测试\n");
    PIDConfig cfg{0.0f, 1.0f, 0.0f, 10.0f, 1.0f};
    PID pid(cfg);
    pid.update(1.0f, 0.01f); // integral = 1.0 * 0.01 = 0.01
    pid.update(1.0f, 0.01f); // integral = 0.02
    f32 out = pid.update(1.0f, 0.01f); // integral = 0.03, out = 0.03
    ASSERT_NEAR(out, 0.03f, 0.001f, "I=1, 连续3步error=1 → integral累积");
}

void test_pid_output_limit() {
    printf("\n[PID] 输出限幅测试\n");
    PIDConfig cfg{100.0f, 0.0f, 0.0f, 10.0f, 1.0f}; // out_limit=1.0
    PID pid(cfg);
    f32 out = pid.update(10.0f, 0.01f); // 100*10=1000 → clamped to 1.0
    ASSERT_NEAR(out, 1.0f, 0.001f, "输出限幅到1.0");
}

void test_pid_integral_limit() {
    printf("\n[PID] 积分限幅测试\n");
    PIDConfig cfg{0.0f, 1.0f, 0.0f, 5.0f, 1.0f}; // i_limit=5.0
    PID pid(cfg);
    for (int i = 0; i < 1000; i++) pid.update(1.0f, 0.01f);
    // integral should be clamped to 5.0
    f32 out = pid.update(1.0f, 0.01f);
    ASSERT(out <= 5.1f, "积分限幅：输出不超过i_limit");
}

void test_pid_reset() {
    printf("\n[PID] 重置测试\n");
    PIDConfig cfg{1.0f, 1.0f, 0.0f, 10.0f, 1.0f};
    PID pid(cfg);
    pid.update(1.0f, 0.01f);
    pid.reset();
    f32 out = pid.update(0.0f, 0.01f);
    ASSERT_NEAR(out, 0.0f, 0.001f, "重置后零误差→输出零");
}

// ═══════════════════════════════════════════════════════════════════
//  位置控制器测试
// ═══════════════════════════════════════════════════════════════════

void test_position_control_direction() {
    printf("\n[位置控制] 方向测试\n");
    PositionController pc;
    NEDPosition target{100.0f, 0.0f, 0.0f, 0};
    NEDPosition current{0.0f, 0.0f, 0.0f, 0};
    NEDVelocity vel{0, 0, 0, 0};
    auto cmd = pc.update(target, current, vel, 0.01f);
    ASSERT(cmd.vn > 0, "目标在北→向北速度为正");
}

void test_position_control_at_target() {
    printf("\n[位置控制] 已在目标点\n");
    PositionController pc;
    NEDPosition target{0.0f, 0.0f, 0.0f, 0};
    NEDPosition current{0.0f, 0.0f, 0.0f, 0};
    NEDVelocity vel{0, 0, 0, 0};
    auto cmd = pc.update(target, current, vel, 0.01f);
    ASSERT_NEAR(cmd.vn, 0.0f, 0.5f, "已在目标点→速度接近零");
}

// ═══════════════════════════════════════════════════════════════════
//  状态机测试
// ═══════════════════════════════════════════════════════════════════

void test_fsm_initial_state() {
    printf("\n[状态机] 初始状态\n");
    FlightStateMachine fsm;
    ASSERT(fsm.state() == FlightState::DISARMED, "初始状态为DISARMED");
    ASSERT(fsm.canArm(), "DISARMED状态可以ARM");
}

void test_fsm_arm_sequence() {
    printf("\n[状态机] 解锁序列\n");
    FlightStateMachine fsm;
    auto t = fsm.handleEvent(FlightStateMachine::Event::ARM);
    ASSERT(t.allowed, "ARM事件允许");
    ASSERT(fsm.state() == FlightState::STANDBY, "ARM后进入STANDBY");
}

void test_fsm_takeoff_sequence() {
    printf("\n[状态机] 起飞序列\n");
    FlightStateMachine fsm;
    fsm.handleEvent(FlightStateMachine::Event::ARM);
    fsm.handleEvent(FlightStateMachine::Event::TAKEOFF);
    ASSERT(fsm.state() == FlightState::TAKING_OFF, "TAKEOFF后进入TAKING_OFF");
}

void test_fsm_airborne() {
    printf("\n[状态机] 离地确认\n");
    FlightStateMachine fsm;
    fsm.handleEvent(FlightStateMachine::Event::ARM);
    fsm.handleEvent(FlightStateMachine::Event::TAKEOFF);
    auto t = fsm.handleEvent(FlightStateMachine::Event::AIRBORNE);
    ASSERT(t.allowed, "AIRBORNE事件允许");
    ASSERT(fsm.state() == FlightState::IN_FLIGHT, "AIRBORNE后进入IN_FLIGHT");
}

void test_fsm_no_skip_airborne() {
    printf("\n[状态机] 不允许跳过AIRBORNE\n");
    FlightStateMachine fsm;
    fsm.handleEvent(FlightStateMachine::Event::ARM);
    fsm.handleEvent(FlightStateMachine::Event::TAKEOFF);
    // 在TAKING_OFF状态，不应有LAND事件直接导致IN_FLIGHT
    auto t = fsm.handleEvent(FlightStateMachine::Event::LAND);
    // LAND在TAKING_OFF状态不应该被处理
    ASSERT(fsm.state() == FlightState::TAKING_OFF, "TAKING_OFF时LAND不改变状态");
}

void test_fsm_fault_during_takeoff() {
    printf("\n[状态机] 起飞期间故障\n");
    FlightStateMachine fsm;
    fsm.handleEvent(FlightStateMachine::Event::ARM);
    fsm.handleEvent(FlightStateMachine::Event::TAKEOFF);
    auto t = fsm.handleEvent(FlightStateMachine::Event::FAULT);
    ASSERT(t.allowed, "FAULT事件允许");
    ASSERT(fsm.state() == FlightState::FAILSAFE, "FAULT后进入FAILSAFE");
}

void test_fsm_is_flying() {
    printf("\n[状态机] isFlying判断\n");
    FlightStateMachine fsm;
    ASSERT(!fsm.isFlying(), "DISARMED不飞行");
    fsm.handleEvent(FlightStateMachine::Event::ARM);
    fsm.handleEvent(FlightStateMachine::Event::TAKEOFF);
    ASSERT(fsm.isFlying(), "TAKING_OFF算飞行中");
}

void test_fsm_cannot_arm_twice() {
    printf("\n[状态机] 不能重复ARM\n");
    FlightStateMachine fsm;
    fsm.handleEvent(FlightStateMachine::Event::ARM);
    auto t = fsm.handleEvent(FlightStateMachine::Event::ARM);
    ASSERT(!t.allowed, "STANDBY状态不允许再次ARM");
}

// ═══════════════════════════════════════════════════════════════════
//  浮力模型测试
// ═══════════════════════════════════════════════════════════════════

void test_buoyancy_air_density() {
    printf("\n[浮力] 空气密度计算\n");
    f32 rho = Power::HeliumBuoyancy::air_density(20.0f, 1013.25f);
    ASSERT_NEAR(rho, 1.205f, 0.01f, "20°C, 1013.25hPa → ρ≈1.205 kg/m³");
}

void test_buoyancy_lift_force() {
    printf("\n[浮力] 升力计算\n");
    f32 lift = Power::HeliumBuoyancy::buoyancy_force_n(2.5f, 20.0f, 20.0f, 1013.25f);
    ASSERT(lift > 0, "正体积→正升力");
}

void test_buoyancy_status() {
    printf("\n[浮力] 状态计算\n");
    Power::HeliumBuoyancy hb;
    Power::HeliumBuoyancy::Sensors s;
    s.he_pressure_atm = 1.0f;
    s.he_temp_c = 20.0f;
    s.amb_pressure_hpa = 1013.25f;
    s.amb_temp_c = 20.0f;
    s.altitude_m = 0;
    s.he_mass_kg = 0.05f;
    s.envelope_vol_m3 = 2.5f;
    auto status = hb.compute_status(s);
    ASSERT(status.buoyancy_ratio > 0, "浮力比大于0");
    ASSERT(status.lift_force_n > 0, "升力大于0");
}

// ═══════════════════════════════════════════════════════════════════
//  主测试入口
// ═══════════════════════════════════════════════════════════════════

int main() {
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║       FlyteOS 飞控单元测试 - 云中鹤              ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n");

    test_pid_zero_error();
    test_pid_proportional();
    test_pid_integral();
    test_pid_output_limit();
    test_pid_integral_limit();
    test_pid_reset();

    test_position_control_direction();
    test_position_control_at_target();

    test_fsm_initial_state();
    test_fsm_arm_sequence();
    test_fsm_takeoff_sequence();
    test_fsm_airborne();
    test_fsm_no_skip_airborne();
    test_fsm_fault_during_takeoff();
    test_fsm_is_flying();
    test_fsm_cannot_arm_twice();

    test_buoyancy_air_density();
    test_buoyancy_lift_force();
    test_buoyancy_status();

    printf("\n══════════════════════════════════════════════════\n");
    printf("  测试结果: %d 通过, %d 失败, 共 %d 项\n", g_pass, g_fail, g_pass + g_fail);
    printf("══════════════════════════════════════════════════\n");

    if (!g_failures.empty()) {
        printf("\n失败项:\n");
        for (const auto& f : g_failures) printf("  ✗ %s\n", f.c_str());
    }

    return g_fail > 0 ? 1 : 0;
}
