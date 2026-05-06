/**
 * @file test_pid_controller.cpp
 * @brief PID 姿态控制器单元测试
 * @company 武汉福莱特航空科技有限公司
 * @author  ASUS
 * @date    2026-05-06
 */
#include "../src/flight_control/pid_controller.h"

#include <cstdio>
#include <cmath>
#include <cassert>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace FlyteOS;
using namespace FlyteOS::Control;

#define ASSERT_NEAR(a, b, tol) do {                                         \
    f32 _a = (a), _b = (b), _t = (tol);                                    \
    if (std::fabs(_a - _b) > _t) {                                         \
        printf("FAIL: %s (line %d): %.6f != %.6f (tol=%.6f)\n",           \
               #a, __LINE__, _a, _b, _t);                                  \
        failures++;                                                        \
    } else { passes++; }                                                   \
} while(0)

#define ASSERT_TRUE(cond) do {                                             \
    if (!(cond)) {                                                         \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__);                   \
        failures++;                                                        \
    } else { passes++; }                                                   \
} while(0)

static int passes = 0;
static int failures = 0;

// ─── PIDAxis Tests ───

void test_pid_proportional() {
    printf("  [TEST] PID proportional response...\n");
    PIDConfig cfg; cfg.kp=2.0f; cfg.ki=0; cfg.kd=0; cfg.out_limit=10.0f;
    PIDAxis pid(cfg);
    f32 out = pid.update(1.0f, 0.01f);
    ASSERT_NEAR(out, 2.0f, 0.01f);
}

void test_pid_integral() {
    printf("  [TEST] PID integral eliminates steady-state error...\n");
    PIDConfig cfg; cfg.kp=1.0f; cfg.ki=5.0f; cfg.kd=0; cfg.i_limit=10.0f; cfg.out_limit=100.0f;
    PIDAxis pid(cfg);
    f32 out_first=0, out_last=0;
    for (int i=0;i<100;i++) { f32 o=pid.update(0.1f,0.01f); if(i==0)out_first=o; out_last=o; }
    ASSERT_TRUE(out_last > out_first);
}

void test_pid_anti_windup() {
    printf("  [TEST] PID anti-windup (integral clamping)...\n");
    PIDConfig cfg; cfg.kp=0; cfg.ki=10.0f; cfg.kd=0; cfg.i_limit=0.5f; cfg.out_limit=1.0f;
    PIDAxis pid(cfg);
    for(int i=0;i<1000;i++) pid.update(10.0f,0.01f);
    ASSERT_NEAR(pid.getIntegral(), 0.5f, 0.01f);
    f32 out = pid.update(10.0f,0.01f);
    ASSERT_NEAR(out, cfg.out_limit, 0.01f);
}

void test_pid_output_limit() {
    printf("  [TEST] PID output limit...\n");
    PIDConfig cfg; cfg.kp=100.0f; cfg.ki=0; cfg.kd=0; cfg.out_limit=1.0f;
    PIDAxis pid(cfg);
    ASSERT_NEAR(pid.update(1.0f,0.01f), 1.0f, 0.01f);
    ASSERT_NEAR(pid.update(-1.0f,0.01f), -1.0f, 0.01f);
}

void test_pid_zero_dt() {
    printf("  [TEST] PID returns zero when dt <= 0...\n");
    PIDAxis pid;
    ASSERT_NEAR(pid.update(1.0f, 0.0f),   0.0f, 0.001f);
    ASSERT_NEAR(pid.update(1.0f, -0.01f), 0.0f, 0.001f);
}

void test_pid_reset() {
    printf("  [TEST] PID reset clears state...\n");
    PIDConfig cfg; cfg.ki=10.0f; cfg.kp=0; cfg.kd=0; cfg.i_limit=5.0f; cfg.out_limit=100.0f;
    PIDAxis pid(cfg);
    for(int i=0;i<10;i++) pid.update(1.0f,0.01f);
    f32 out_before = pid.update(1.0f,0.01f);
    pid.reset();
    f32 out_after = pid.update(1.0f,0.01f);
    ASSERT_TRUE(out_after < out_before);
    ASSERT_NEAR(pid.getIntegral(), 0.01f, 0.011f);
}

void test_pid_derivative_filter() {
    printf("  [TEST] PID derivative low-pass filter...\n");
    PIDConfig cfg; cfg.kp=0; cfg.ki=0; cfg.kd=1.0f; cfg.d_filter=0.5f; cfg.out_limit=1000.0f;
    PIDAxis pid(cfg);
    f32 d1 = pid.update(1.0f,0.01f);
    f32 d2 = pid.update(1.0f,0.01f);
    ASSERT_TRUE(std::fabs(d2) < std::fabs(d1));
}

// ─── PIDAttitudeController Tests ───

void test_attitude_basic() {
    printf("  [TEST] Attitude controller basic 3-axis control...\n");
    PIDAttitudeController ctrl;
    Vec3 target{0.1f,0.05f,0.0f}, current{0,0,0}, ang_vel{0,0,0};
    auto out = ctrl.update(target,current,ang_vel,0.0025f);
    ASSERT_TRUE(std::fabs(out.roll_cmd)>0);
    ASSERT_TRUE(std::fabs(out.pitch_cmd)>0);
}

void test_attitude_yaw_normalization() {
    printf("  [TEST] Yaw error normalization across ±π boundary...\n");
    PIDAttitudeController ctrl;
    Vec3 target{0,0,3.0f}, current{0,0,-3.0f}, ang_vel{0,0,0};
    auto out = ctrl.update(target,current,ang_vel,0.0025f);
    ASSERT_TRUE(std::fabs(out.yaw_cmd) < 1.0f);
}

void test_attitude_zero_error() {
    printf("  [TEST] Zero attitude error → near-zero output...\n");
    PIDAttitudeController ctrl;
    Vec3 t{0.1f,0.05f,0.3f}, c{0.1f,0.05f,0.3f}, v{0,0,0};
    auto out = ctrl.update(t,c,v,0.0025f);
    ASSERT_NEAR(out.roll_cmd,0,0.01f);
    ASSERT_NEAR(out.pitch_cmd,0,0.01f);
    ASSERT_NEAR(out.yaw_cmd,0,0.01f);
}

void test_mix_quadx_range() {
    printf("  [TEST] QuadX mixer output range [0, 1]...\n");
    f32 motor[4];
    PIDAttitudeController::mixQuadX(1,1,1,1,motor);
    for(int i=0;i<4;i++) ASSERT_TRUE(motor[i]>=0 && motor[i]<=1);
    PIDAttitudeController::mixQuadX(-1,-1,-1,0,motor);
    for(int i=0;i<4;i++) ASSERT_TRUE(motor[i]>=0 && motor[i]<=1);
    PIDAttitudeController::mixQuadX(0.5f,-0.3f,0.2f,0.6f,motor);
    for(int i=0;i<4;i++) ASSERT_TRUE(motor[i]>=0 && motor[i]<=1);
}

void test_attitude_default_config() {
    printf("  [TEST] Default configuration values...\n");
    PIDAttitudeController::Config cfg;
    ASSERT_TRUE(cfg.roll.kp>0 && cfg.pitch.kp>0 && cfg.yaw.kp>0);
}

void test_attitude_reset() {
    printf("  [TEST] Attitude controller reset...\n");
    PIDAttitudeController ctrl;
    Vec3 t{0.1f,0.1f,0.1f}, c{0,0,0}, v{0,0,0};
    for(int i=0;i<50;i++) ctrl.update(t,c,v,0.0025f);
    ctrl.reset();
    Vec3 z{0,0,0};
    auto out = ctrl.update(z,z,v,0.0025f);
    ASSERT_NEAR(out.roll_cmd,0,0.01f);
    ASSERT_NEAR(out.pitch_cmd,0,0.01f);
    ASSERT_NEAR(out.yaw_cmd,0,0.01f);
}

void test_attitude_set_config() {
    printf("  [TEST] Dynamic config change...\n");
    PIDAttitudeController ctrl;
    PIDAttitudeController::Config nc;
    nc.roll.kp=10; nc.pitch.kp=10; nc.yaw.kp=10;
    ctrl.setConfig(nc);
    const auto& a = ctrl.getConfig();
    ASSERT_NEAR(a.roll.kp,10,0.01f);
    ASSERT_NEAR(a.pitch.kp,10,0.01f);
    ASSERT_NEAR(a.yaw.kp,10,0.01f);
}

void test_hover_stability() {
    printf("  [TEST] Simulated hover stability (300 steps)...\n");
    PIDAttitudeController ctrl;
    Vec3 target{0.1f,0.05f,0.0f}, current{0,0,0}, ang_vel{0,0,0};
    f32 dt = 0.0025f;
    for(int i=0;i<300;i++){
        auto out = ctrl.update(target,current,ang_vel,dt);
        ang_vel.x += out.roll_cmd*5.0f*dt;
        ang_vel.y += out.pitch_cmd*5.0f*dt;
        ang_vel.z += out.yaw_cmd*5.0f*dt;
        ang_vel.x *= 0.95f; ang_vel.y *= 0.95f; ang_vel.z *= 0.95f;
        current.x += ang_vel.x*dt; current.y += ang_vel.y*dt; current.z += ang_vel.z*dt;
    }
    ASSERT_TRUE(std::fabs(target.x-current.x)<0.05f);
    ASSERT_TRUE(std::fabs(target.y-current.y)<0.05f);
    ASSERT_TRUE(std::fabs(target.z-current.z)<0.05f);
}

// ─── Main ───
int main() {
    printf("===========================================\n");
    printf("  FlyteOS PID Controller Unit Tests\n");
    printf("  Author: ASUS\n");
    printf("===========================================\n\n");

    printf("─── PIDAxis Tests ───\n");
    test_pid_proportional(); test_pid_integral(); test_pid_anti_windup();
    test_pid_output_limit(); test_pid_zero_dt(); test_pid_reset();
    test_pid_derivative_filter();

    printf("\n─── PIDAttitudeController Tests ───\n");
    test_attitude_basic(); test_attitude_yaw_normalization();
    test_attitude_zero_error(); test_mix_quadx_range();
    test_attitude_default_config(); test_attitude_reset();
    test_attitude_set_config(); test_hover_stability();

    printf("\n===========================================\n");
    printf("  Results: %d passed, %d failed\n", passes, failures);
    printf("===========================================\n");
    return failures > 0 ? 1 : 0;
}
