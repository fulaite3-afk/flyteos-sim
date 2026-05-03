/**
 * @file sensor_mock.cpp
 * @brief 传感器模拟器：生成仿真IMU/GPS/气压计数据
 */
#include "sensor_mock.hpp"
#include <cmath>

namespace FlyteOS::Simulator {

SensorMock::SensorMock(Config cfg) : cfg_(cfg) {}

Attitude::AttitudeEstimator::IMURaw SensorMock::generateIMU(const PhysicsState& state, f32 dt) {
    Attitude::AttitudeEstimator::IMURaw imu;

    // 从姿态变化率推导角速度
    Vec3 euler = state.attitude.toEuler();
    imu.gyro[0] = (euler.x - prev_euler_.x) / std::max(dt, 0.001f);
    imu.gyro[1] = (euler.y - prev_euler_.y) / std::max(dt, 0.001f);
    imu.gyro[2] = (euler.z - prev_euler_.z) / std::max(dt, 0.001f);

    // 加速度（简化：从速度变化率推导 + 重力补偿）
    imu.accel[0] = (state.velocity.vn - prev_vel_.vn) / std::max(dt, 0.001f);
    imu.accel[1] = (state.velocity.ve - prev_vel_.ve) / std::max(dt, 0.001f);
    imu.accel[2] = (state.velocity.vd - prev_vel_.vd) / std::max(dt, 0.001f) - 9.81f;

    // 磁力计（简化：指北）
    imu.mag[0] = 20.0f; // μT
    imu.mag[1] = 0.0f;
    imu.mag[2] = -40.0f;

    // 添加噪声
    addNoise(imu.gyro, 3, cfg_.gyro_noise_rads);
    addNoise(imu.accel, 3, cfg_.accel_noise_ms2);
    addNoise(imu.mag, 3, cfg_.mag_noise_ut);

    imu.ts = now_us();
    prev_euler_ = euler;
    prev_vel_ = state.velocity;
    return imu;
}

Attitude::AttitudeEstimator::GPSRaw SensorMock::generateGPS(const PhysicsState& state) {
    Attitude::AttitudeEstimator::GPSRaw gps;

    // NED → 经纬度转换
    f32 ref_lat = 30.5023f;
    f32 ref_lon = 114.4047f;
    gps.lat = ref_lat + state.position.north / 111320.0;
    gps.lon = ref_lon + state.position.east  / (111320.0 * cosf(ref_lat * M_PI / 180.0f));
    gps.alt = -state.position.down;

    gps.vel_ned[0] = state.velocity.vn;
    gps.vel_ned[1] = state.velocity.ve;
    gps.vel_ned[2] = state.velocity.vd;

    gps.hdop = 0.8f + (rand() % 100) / 100.0f * 0.4f;
    gps.fix = 5; // RTK Fixed

    gps.ts = now_us();
    return gps;
}

f32 SensorMock::generateBaroAlt(const PhysicsState& state) {
    f32 noise = ((rand() % 1000) - 500) / 1000.0f * cfg_.baro_noise_m;
    return -state.position.down + noise;
}

void SensorMock::addNoise(f32* data, int count, f32 sigma) {
    for (int i = 0; i < count; i++) {
        // 简单高斯噪声近似
        f32 u1 = (rand() + 1) / (f32)(RAND_MAX + 1);
        f32 u2 = (rand() + 1) / (f32)(RAND_MAX + 1);
        f32 z0 = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * M_PI * u2);
        data[i] += z0 * sigma;
    }
}

} // namespace FlyteOS::Simulator
