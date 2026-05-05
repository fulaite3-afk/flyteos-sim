#pragma once
/**
 * @file sensor_mock.hpp
 * @brief 传感器模拟器：生成仿真IMU/GPS/气压计数据
 */
#include "../include/flyteos_types.hpp"
#include "../src/attitude/attitude_estimator.hpp"
#include "physics_engine.hpp"

namespace FlyteOS::Simulator {

class SensorMock {
public:
    struct Config {
        f32 gyro_noise_rads  = 0.01f;
        f32 accel_noise_ms2  = 0.05f;
        f32 mag_noise_ut     = 0.1f;
        f32 baro_noise_m     = 0.5f;
        f32 gps_noise_m      = 2.0f;
    };

    explicit SensorMock(Config cfg);
    SensorMock() : SensorMock(Config{}) {}

    Attitude::AttitudeEstimator::IMURaw  generateIMU(const PhysicsState& state, f32 dt);
    Attitude::AttitudeEstimator::GPSRaw  generateGPS(const PhysicsState& state);
    f32 generateBaroAlt(const PhysicsState& state);

private:
    Config cfg_;
    Vec3 prev_euler_ = {};
    NEDVelocity prev_vel_ = {};

    void addNoise(f32* data, int count, f32 sigma);
};

} // namespace FlyteOS::Simulator
