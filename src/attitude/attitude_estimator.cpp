/**
 * @file attitude_estimator.cpp
 * @brief 姿态估计器实现（Mahony 互补滤波 + 简化 EKF）
 */
#include "attitude_estimator.hpp"
#include <cmath>
#include <cstring>

namespace FlyteOS::Attitude {

AttitudeEstimator::AttitudeEstimator(NoiseParams np) : np_(np) {
    memset(P_, 0, sizeof(P_));
    // 初始协方差
    for (int i = 0; i < 7; i++) P_[i][i] = (i < 4) ? 1.0f : 0.01f;
}

void AttitudeEstimator::reset() {
    X_[0] = 1; X_[1] = X_[2] = X_[3] = 0;
    X_[4] = X_[5] = X_[6] = 0;
    for (int i = 0; i < 7; i++) for (int j = 0; j < 7; j++) P_[i][j] = 0;
    for (int i = 0; i < 7; i++) P_[i][i] = (i < 4) ? 1.0f : 0.01f;
    initialized_ = false;
    last_ts_     = 0;
}

void AttitudeEstimator::setInitialAttitude(f32 roll, f32 pitch, f32 yaw) {
    // 欧拉角 → 四元数
    f32 cr = cosf(roll*0.5f),  sr = sinf(roll*0.5f);
    f32 cp = cosf(pitch*0.5f), sp = sinf(pitch*0.5f);
    f32 cy = cosf(yaw*0.5f),   sy = sinf(yaw*0.5f);

    X_[0] = cr*cp*cy + sr*sp*sy;
    X_[1] = sr*cp*cy - cr*sp*sy;
    X_[2] = cr*sp*cy + sr*cp*sy;
    X_[3] = cr*cp*sy - sr*sp*cy;
    initialized_ = true;
}

void AttitudeEstimator::predict(const IMURaw& imu, f32 dt) {
    // 陀螺仪去偏差
    f32 wx = imu.gyro[0] - X_[4];
    f32 wy = imu.gyro[1] - X_[5];
    f32 wz = imu.gyro[2] - X_[6];

    // 四元数积分（一阶近似）
    f32 q0 = X_[0], q1 = X_[1], q2 = X_[2], q3 = X_[3];
    X_[0] += 0.5f * dt * (-q1*wx - q2*wy - q3*wz);
    X_[1] += 0.5f * dt * ( q0*wx + q2*wz - q3*wy);
    X_[2] += 0.5f * dt * ( q0*wy - q1*wz + q3*wx);
    X_[3] += 0.5f * dt * ( q0*wz + q1*wy - q2*wx);
    qNormalize(X_);

    // 偏差随机游走
    for (int i = 4; i < 7; i++) P_[i][i] += np_.bias_rw * dt;
}

void AttitudeEstimator::updateAccel(const f32 accel[3]) {
    f32 ax = accel[0], ay = accel[1], az = accel[2];
    f32 norm = sqrtf(ax*ax + ay*ay + az*az);
    if (norm < 0.1f) return;
    ax /= norm; ay /= norm; az /= norm;

    // 从当前四元数计算重力方向
    f32 q0=X_[0],q1=X_[1],q2=X_[2],q3=X_[3];
    f32 gx = 2*(q1*q3 - q0*q2);
    f32 gy = 2*(q0*q1 + q2*q3);
    f32 gz = q0*q0 - q1*q1 - q2*q2 + q3*q3;

    // 误差（交叉乘积）
    f32 ex = ay*gz - az*gy;
    f32 ey = az*gx - ax*gz;
    f32 ez = ax*gy - ay*gx;

    // 比例积分修正（Mahony）
    const f32 Kp = 2.0f, Ki = 0.005f;
    static f32 ix=0,iy=0,iz=0;
    ix += Ki * ex; iy += Ki * ey; iz += Ki * ez;

    f32 gx2 = gx + Kp*ex + ix;
    f32 gy2 = gy + Kp*ey + iy;
    f32 gz2 = gz + Kp*ez + iz;

    // 用修正后的角速率更新四元数（极小时间步）
    const f32 half_dt = 0.005f;
    X_[0] += (-q1*gx2 - q2*gy2 - q3*gz2) * half_dt;
    X_[1] += ( q0*gx2 + q2*gz2 - q3*gy2) * half_dt;
    X_[2] += ( q0*gy2 - q1*gz2 + q3*gx2) * half_dt;
    X_[3] += ( q0*gz2 + q1*gy2 - q2*gx2) * half_dt;
    qNormalize(X_);
}

AttitudeEstimator::Estimate AttitudeEstimator::update(const IMURaw& imu) {
    if (!initialized_) {
        setInitialAttitude(0, 0, 0);
        last_ts_ = imu.ts;
    }

    f32 dt = (imu.ts > last_ts_) ? (imu.ts - last_ts_) * 1e-6f : 0.01f;
    dt = std::min(dt, 0.1f);
    last_ts_ = imu.ts;

    predict(imu, dt);
    updateAccel(imu.accel);
    if (imu.mag[0] != 0 || imu.mag[1] != 0) updateMag(imu.mag);

    last_.attitude = Quat{X_[0], X_[1], X_[2], X_[3]};
    last_.euler_rad = qToEuler(X_);
    last_.gyro_bias = Vec3{X_[4], X_[5], X_[6]};
    last_.confidence = 0.95f;
    last_.ts = imu.ts;
    return last_;
}

AttitudeEstimator::Estimate AttitudeEstimator::updateWithGPS(const IMURaw& imu, const GPSRaw& gps) {
    auto est = update(imu);
    // GPS 辅助修正偏航（若有磁力计可用则以磁力计为主）
    if (gps.fix >= 3 && gps.hdop < 2.0f) {
        f32 gps_yaw = atan2f(gps.vel_ned[1], gps.vel_ned[0]);
        f32 yaw_err = gps_yaw - est.euler_rad.z;
        // 微小偏航修正（1% 融合）
        X_[3] += 0.01f * sinf(yaw_err * 0.5f);
        qNormalize(X_);
        last_.euler_rad = qToEuler(X_);
    }
    return last_;
}

void AttitudeEstimator::updateMag(const f32 mag[3]) {
    f32 mx=mag[0], my=mag[1], mz=mag[2];
    f32 nm = sqrtf(mx*mx+my*my+mz*mz);
    if (nm < 1.0f) return;
    mx/=nm; my/=nm; mz/=nm;

    // 磁偏角修正偏航（简化）
    f32 yaw_mag = atan2f(-my, mx);
    f32 yaw_cur = qToEuler(X_).z;
    f32 err = yaw_mag - yaw_cur;
    const f32 Km = 0.01f;
    X_[3] += Km * sinf(err * 0.5f);
    qNormalize(X_);
}

void AttitudeEstimator::qMultiply(const f32 a[4], const f32 b[4], f32 c[4]) {
    c[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
    c[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
    c[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
    c[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
}

void AttitudeEstimator::qNormalize(f32 q[4]) {
    f32 n = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (n < 1e-6f) { q[0]=1;q[1]=q[2]=q[3]=0; return; }
    for(int i=0;i<4;i++) q[i]/=n;
}

Vec3 AttitudeEstimator::qToEuler(const f32 q[4]) {
    Vec3 e;
    // roll
    e.x = atan2f(2*(q[0]*q[1]+q[2]*q[3]), 1-2*(q[1]*q[1]+q[2]*q[2]));
    // pitch
    f32 sinp = 2*(q[0]*q[2]-q[3]*q[1]);
    e.y = (fabsf(sinp) >= 1) ? copysignf(M_PI/2, sinp) : asinf(sinp);
    // yaw
    e.z = atan2f(2*(q[0]*q[3]+q[1]*q[2]), 1-2*(q[2]*q[2]+q[3]*q[3]));
    return e;
}

} // namespace FlyteOS::Attitude
