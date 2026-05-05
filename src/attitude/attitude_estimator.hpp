#pragma once
/**
 * @file attitude_estimator.hpp
 * @brief 姿态估计器：IMU + GPS 扩展卡尔曼滤波（EKF）
 */
#include "../../include/flyteos_types.hpp"

namespace FlyteOS::Attitude {

class AttitudeEstimator {
public:
    struct IMURaw {
        f32    gyro[3]  = {};    // rad/s  (x,y,z)
        f32    accel[3] = {};    // m/s²
        f32    mag[3]   = {};    // μT
        TimeUs ts       = 0;
    };

    struct GPSRaw {
        f64    lat = 0, lon = 0;
        f32    alt = 0;
        f32    vel_ned[3] = {};  // m/s
        f32    hdop = 99.9f;
        u8     fix  = 0;
        TimeUs ts   = 0;
    };

    struct Estimate {
        Quat   attitude;          // 姿态四元数
        Vec3   euler_rad;         // roll/pitch/yaw (rad)
        Vec3   gyro_bias;         // 陀螺仪偏差 rad/s
        f32    confidence = 0;    // 0~1
        TimeUs ts = 0;
    };

    struct NoiseParams {
        f32 gyro_noise   = 0.001f;
        f32 accel_noise  = 0.1f;
        f32 mag_noise    = 0.01f;
        f32 bias_rw      = 0.0001f;  // 偏差随机游走
    };

    explicit AttitudeEstimator(NoiseParams np);
    AttitudeEstimator() : AttitudeEstimator(NoiseParams{}) {}

    Estimate update(const IMURaw& imu);
    Estimate updateWithGPS(const IMURaw& imu, const GPSRaw& gps);
    void reset();
    void setInitialAttitude(f32 roll, f32 pitch, f32 yaw);
    const Estimate& last() const { return last_; }

private:
    NoiseParams np_;
    Estimate    last_;

    // 状态向量 [q0,q1,q2,q3, bx,by,bz]  7维
    f32 X_[7]    = {1,0,0,0,0,0,0};
    f32 P_[7][7] = {};  // 协方差矩阵

    bool initialized_ = false;
    TimeUs last_ts_   = 0;

    void predict(const IMURaw& imu, f32 dt);
    void updateAccel(const f32 accel[3]);
    void updateMag(const f32 mag[3]);

    // 四元数工具
    static void qMultiply(const f32 a[4], const f32 b[4], f32 c[4]);
    static void qNormalize(f32 q[4]);
    static Vec3 qToEuler(const f32 q[4]);
};

} // namespace FlyteOS::Attitude
