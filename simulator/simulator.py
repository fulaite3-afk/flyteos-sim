"""
FlyteOS 浮空飞行器样品飞行模拟器
武汉福莱特航空科技有限公司

功能：
  - 模拟浮空飞行器物理特性（氦气浮力 + 四旋翼推力 + 太阳能）
  - 三环PID 姿态/速度/位置控制
  - 实时可视化（2D 俯视 + 侧视 + 仪表盘）
  - 航点自动飞行演示

依赖：
  pip install matplotlib numpy

运行：
  python simulator.py
"""

import math
import time
import threading
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyArrowPatch
from matplotlib.gridspec import GridSpec
from collections import deque
from dataclasses import dataclass, field
from typing import List, Tuple

# ═══════════════════════════════════════════════════════════════════
#  物理常数
# ═══════════════════════════════════════════════════════════════════
G        = 9.81       # 重力加速度 m/s²
RHO_AIR  = 1.225      # 海平面空气密度 kg/m³
RHO_HE   = 0.1786     # 氦气密度 kg/m³ (STP)
DT       = 0.02       # 仿真步长 s (50 Hz)

# ═══════════════════════════════════════════════════════════════════
#  数据类
# ═══════════════════════════════════════════════════════════════════
@dataclass
class Vec3:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

    def __add__(self, o): return Vec3(self.x+o.x, self.y+o.y, self.z+o.z)
    def __sub__(self, o): return Vec3(self.x-o.x, self.y-o.y, self.z-o.z)
    def __mul__(self, s): return Vec3(self.x*s,   self.y*s,   self.z*s)
    def norm(self): return math.sqrt(self.x**2 + self.y**2 + self.z**2)
    def to_np(self): return np.array([self.x, self.y, self.z])


@dataclass
class AircraftState:
    # 位置 (NED m)
    pos: Vec3 = field(default_factory=Vec3)
    # 速度 (NED m/s)
    vel: Vec3 = field(default_factory=Vec3)
    # 姿态欧拉角 (rad)
    roll:  float = 0.0
    pitch: float = 0.0
    yaw:   float = 0.0
    # 角速度 (rad/s)
    p: float = 0.0
    q: float = 0.0
    r: float = 0.0

    # 电源
    solar_power_w:   float = 0.0
    turbine_power_w: float = 0.0
    battery_pct:     float = 100.0
    buoyancy_ratio:  float = 0.60    # 氦气浮力占比

    # 飞行状态
    flight_state: str = "DISARMED"
    motors: List[float] = field(default_factory=lambda: [0.0]*4)
    time_s: float = 0.0


# ═══════════════════════════════════════════════════════════════════
#  简单 PID
# ═══════════════════════════════════════════════════════════════════
class PID:
    def __init__(self, kp, ki, kd, i_lim=10.0, out_lim=1.0):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.i_lim, self.out_lim = i_lim, out_lim
        self.integral = 0.0
        self.prev_err = 0.0

    def update(self, error, dt):
        self.integral = np.clip(self.integral + error * dt, -self.i_lim, self.i_lim)
        deriv = (error - self.prev_err) / max(dt, 1e-6)
        self.prev_err = error
        out = self.kp * error + self.ki * self.integral + self.kd * deriv
        return float(np.clip(out, -self.out_lim, self.out_lim))

    def reset(self):
        self.integral = self.prev_err = 0.0


# ═══════════════════════════════════════════════════════════════════
#  浮空飞行器物理引擎
# ═══════════════════════════════════════════════════════════════════
class PhysicsEngine:
    def __init__(self):
        self.mass_kg        = 12.0     # 飞行器总质量 kg
        self.envelope_m3    = 2.5      # 氦气囊体积 m³
        self.max_thrust_n   = 60.0     # 最大推力 N
        self.drag_coeff     = 0.4      # 气动阻力系数
        self.inertia        = Vec3(0.3, 0.3, 0.5)  # 转动惯量 kg·m²

    def buoyancy_force(self, altitude_m: float) -> float:
        """高度修正的氦气浮力 N"""
        rho_alt = RHO_AIR * math.exp(-altitude_m / 8500.0)
        return (rho_alt - RHO_HE) * self.envelope_m3 * G

    def step(self, state: AircraftState, motors: List[float]) -> AircraftState:
        """推进一步物理仿真"""
        s = state
        dt = DT

        # ── 推力计算 ─────────────────────────────────
        total_motor = sum(np.clip(motors, 0, 1)) / 4.0
        thrust_n    = total_motor * self.max_thrust_n

        # 浮力（向上为负，因为NED坐标系Down为正）
        buoy_n = self.buoyancy_force(-s.pos.z)

        # ── 力/力矩分解 ──────────────────────────────
        # 混控（X型四旋翼）
        m0, m1, m2, m3 = [np.clip(m, 0, 1) for m in motors]
        roll_torque  = (m0 - m1 - m2 + m3) * self.max_thrust_n * 0.15
        pitch_torque = (-m0 - m1 + m2 + m3) * self.max_thrust_n * 0.15
        yaw_torque   = (m0 - m1 + m2 - m3) * self.max_thrust_n * 0.02

        # ── 姿态动力学（欧拉方程简化）────────────────
        p_dot = roll_torque  / self.inertia.x - s.q * s.r * (self.inertia.z - self.inertia.y) / self.inertia.x
        q_dot = pitch_torque / self.inertia.y - s.p * s.r * (self.inertia.x - self.inertia.z) / self.inertia.y
        r_dot = yaw_torque   / self.inertia.z

        s.p += p_dot * dt
        s.q += q_dot * dt
        s.r += r_dot * dt

        # 阻尼
        s.p *= 0.97
        s.q *= 0.97
        s.r *= 0.97

        # 欧拉角积分
        s.roll  += s.p * dt
        s.pitch += s.q * dt
        s.yaw   += s.r * dt

        # 角度限制（浮空器）
        s.roll  = np.clip(s.roll,  -0.4, 0.4)
        s.pitch = np.clip(s.pitch, -0.4, 0.4)

        # ── 平移动力学 ────────────────────────────────
        cr, sr = math.cos(s.roll),  math.sin(s.roll)
        cp, sp = math.cos(s.pitch), math.sin(s.pitch)
        cy, sy = math.cos(s.yaw),   math.sin(s.yaw)

        # 推力在NED坐标中的分量
        fn = (thrust_n + buoy_n * 0.5) * (-sp * cy) / self.max_thrust_n * self.max_thrust_n
        fe = (thrust_n + buoy_n * 0.5) * (-sp * sy)
        fd = -(thrust_n * cp * cr + buoy_n)  # 负z = 向上

        # 重力（NED down方向为正）
        fd_gravity = self.mass_kg * G

        # 气动阻力
        spd = s.vel.norm()
        drag = self.drag_coeff * spd * spd * 0.1
        drag_n = -s.vel.x * drag / max(spd, 0.01)
        drag_e = -s.vel.y * drag / max(spd, 0.01)
        drag_d = -s.vel.z * drag / max(spd, 0.01)

        ax = (fn + drag_n) / self.mass_kg
        ay = (fe + drag_e) / self.mass_kg
        az = (fd + fd_gravity + drag_d) / self.mass_kg

        s.vel.x += ax * dt
        s.vel.y += ay * dt
        s.vel.z += az * dt

        # 速度限制
        s.vel.x = np.clip(s.vel.x, -15, 15)
        s.vel.y = np.clip(s.vel.y, -15, 15)
        s.vel.z = np.clip(s.vel.z, -8, 8)

        # 位置积分（NED，z轴down方向为正，负z=高度）
        s.pos.x += s.vel.x * dt
        s.pos.y += s.vel.y * dt
        s.pos.z += s.vel.z * dt

        # 地面约束（z<=0 即高度>=0）
        if s.pos.z > 0:
            s.pos.z = 0.0
            s.vel.z = min(s.vel.z, 0)

        # ── 能源仿真 ──────────────────────────────────
        irradiance = 800 * max(0, math.cos(s.time_s * 0.0001))
        s.solar_power_w   = 2.5 * 0.22 * irradiance
        s.turbine_power_w = total_motor * 80.0

        power_draw = total_motor * 150.0
        net = (s.solar_power_w + s.turbine_power_w - power_draw) * dt
        s.battery_pct = np.clip(s.battery_pct + net / 200.0, 0, 100)

        s.buoyancy_ratio = buoy_n / (self.mass_kg * G + 1e-3)
        s.motors = motors[:]
        s.time_s += dt
        return s


# ═══════════════════════════════════════════════════════════════════
#  三环飞行控制器
# ═══════════════════════════════════════════════════════════════════
class FlyteController:
    def __init__(self):
        # 位置环
        self.pid_n  = PID(1.5, 0.02, 0.3, i_lim=5, out_lim=6)
        self.pid_e  = PID(1.5, 0.02, 0.3, i_lim=5, out_lim=6)
        self.pid_d  = PID(2.0, 0.05, 0.4, i_lim=3, out_lim=3)
        # 速度环
        self.pid_vn = PID(2.5, 0.05, 0.2, i_lim=8, out_lim=0.35)
        self.pid_ve = PID(2.5, 0.05, 0.2, i_lim=8, out_lim=0.35)
        self.pid_vd = PID(3.0, 0.10, 0.3, i_lim=5, out_lim=0.8)
        # 姿态环
        self.pid_roll  = PID(5.0, 0.1, 0.4, i_lim=3, out_lim=0.5)
        self.pid_pitch = PID(5.0, 0.1, 0.4, i_lim=3, out_lim=0.5)
        self.pid_yaw   = PID(3.0, 0.05, 0.2, i_lim=2, out_lim=0.4)

        self.target_pos = Vec3(0, 0, 0)
        self.target_yaw = 0.0
        self.armed      = False

    def set_target(self, pos: Vec3, yaw: float = 0.0):
        self.target_pos = pos
        self.target_yaw = yaw

    def arm(self): self.armed = True
    def disarm(self): self.armed = False

    def compute(self, s: AircraftState) -> List[float]:
        if not self.armed:
            return [0.0] * 4

        dt = DT

        # ── 位置环 → 速度指令 ────────────────────────
        vn_cmd = self.pid_n.update(self.target_pos.x - s.pos.x, dt)
        ve_cmd = self.pid_e.update(self.target_pos.y - s.pos.y, dt)
        vd_cmd = self.pid_d.update(self.target_pos.z - s.pos.z, dt)

        # ── 速度环 → 姿态指令 ────────────────────────
        cy, sy = math.cos(s.yaw), math.sin(s.yaw)
        an = self.pid_vn.update(vn_cmd - s.vel.x, dt)
        ae = self.pid_ve.update(ve_cmd - s.vel.y, dt)

        roll_cmd  = np.clip( ae * cy - an * sy,    -0.35, 0.35)
        pitch_cmd = np.clip(-(an * cy + ae * sy),   -0.35, 0.35)
        yaw_err   = self.target_yaw - s.yaw
        while yaw_err >  math.pi: yaw_err -= 2 * math.pi
        while yaw_err < -math.pi: yaw_err += 2 * math.pi

        thrust = np.clip(0.45 + self.pid_vd.update(vd_cmd - s.vel.z, dt), 0.1, 0.9)

        # 浮空器浮力补偿（氦气承担约60%，减少电机推力）
        thrust *= (1.0 - s.buoyancy_ratio * 0.6)
        thrust = np.clip(thrust, 0.05, 0.9)

        # ── 姿态环 → 电机指令 ────────────────────────
        r_out = self.pid_roll.update(roll_cmd   - s.roll,  dt)
        p_out = self.pid_pitch.update(pitch_cmd - s.pitch, dt)
        y_out = self.pid_yaw.update(yaw_err,                dt)

        # X型混控
        m0 = thrust + r_out - p_out + y_out
        m1 = thrust - r_out - p_out - y_out
        m2 = thrust - r_out + p_out + y_out
        m3 = thrust + r_out + p_out - y_out
        return [np.clip(m, 0.0, 1.0) for m in [m0, m1, m2, m3]]


# ═══════════════════════════════════════════════════════════════════
#  任务规划（演示航点）
# ═══════════════════════════════════════════════════════════════════
DEMO_MISSION = [
    # (north, east, altitude, hover_s, label)
    ( 0,   0,  -30,  3.0, "起飞悬停"),
    ( 40,  0,  -40,  2.0, "WP1 北飞"),
    ( 40,  40, -50,  2.0, "WP2 东飞"),
    ( 0,   40, -40,  2.0, "WP3 南飞"),
    ( 0,   0,  -40,  2.0, "WP4 返回"),
    ( 0,   0,  -15,  1.0, "开始降落"),
    ( 0,   0,   0,   2.0, "着陆完成"),
]


# ═══════════════════════════════════════════════════════════════════
#  主仿真器
# ═══════════════════════════════════════════════════════════════════
class FlyteSimulator:
    def __init__(self):
        self.physics  = PhysicsEngine()
        self.ctrl     = FlyteController()
        self.state    = AircraftState()
        self.state.pos = Vec3(0, 0, 0)

        # 历史数据（绘图用）
        self.hist_len = 500
        self.hist_pos   = deque(maxlen=self.hist_len)
        self.hist_alt   = deque(maxlen=self.hist_len)
        self.hist_solar = deque(maxlen=self.hist_len)
        self.hist_bat   = deque(maxlen=self.hist_len)
        self.hist_roll  = deque(maxlen=self.hist_len)
        self.hist_pitch = deque(maxlen=self.hist_len)
        self.hist_t     = deque(maxlen=self.hist_len)

        # 任务
        self.mission_idx   = 0
        self.hover_timer   = 0.0
        self.mission_phase = "IDLE"
        self.wp_label      = "待机"

        # 统计
        self.total_dist = 0.0
        self.prev_xy    = (0, 0)

        # 仿真控制
        self.running = True
        self.speed   = 1.0   # 仿真倍速

    def _mission_manager(self):
        """执行演示航点任务"""
        if self.mission_idx >= len(DEMO_MISSION):
            self.ctrl.disarm()
            self.state.flight_state = "LANDED"
            return

        wp = DEMO_MISSION[self.mission_idx]
        n, e, alt_neg, hover_s, label = wp
        target = Vec3(n, e, alt_neg)
        self.ctrl.set_target(target)
        self.wp_label = label

        # 判断是否到达
        dist = math.sqrt((self.state.pos.x - n)**2 +
                         (self.state.pos.y - e)**2 +
                         (self.state.pos.z - alt_neg)**2)

        if dist < 3.0:
            self.hover_timer += DT
            if self.hover_timer >= hover_s:
                self.hover_timer = 0.0
                self.mission_idx += 1

        # 状态标注
        alt_m = -self.state.pos.z
        if alt_m < 1.0:
            self.state.flight_state = "LANDED" if not self.ctrl.armed else "STANDBY"
        elif self.mission_idx >= len(DEMO_MISSION):
            self.state.flight_state = "HOVERING"
        else:
            self.state.flight_state = "WAYPOINT_NAV"

    def start_mission(self):
        self.ctrl.arm()
        self.mission_idx = 0
        self.state.flight_state = "TAKING_OFF"

    def run(self):
        """主仿真循环（独立线程）"""
        self.start_mission()
        while self.running:
            self._mission_manager()
            motors = self.ctrl.compute(self.state)
            self.state = self.physics.step(self.state, motors)

            # 记录历史
            self.hist_pos.append((self.state.pos.x, self.state.pos.y))
            self.hist_alt.append(-self.state.pos.z)
            self.hist_solar.append(self.state.solar_power_w)
            self.hist_bat.append(self.state.battery_pct)
            self.hist_roll.append(math.degrees(self.state.roll))
            self.hist_pitch.append(math.degrees(self.state.pitch))
            self.hist_t.append(self.state.time_s)

            # 飞行距离统计
            cx, cy = self.state.pos.x, self.state.pos.y
            dx, dy = cx - self.prev_xy[0], cy - self.prev_xy[1]
            self.total_dist += math.sqrt(dx*dx + dy*dy)
            self.prev_xy = (cx, cy)

            time.sleep(DT / self.speed)


# ═══════════════════════════════════════════════════════════════════
#  可视化界面
# ═══════════════════════════════════════════════════════════════════
def launch_visualizer(sim: FlyteSimulator):
    plt.rcParams['font.sans-serif'] = ['SimHei', 'Microsoft YaHei', 'DejaVu Sans']
    plt.rcParams['axes.unicode_minus'] = False

    fig = plt.figure(figsize=(16, 10), facecolor='#0a0a1a')
    fig.suptitle('FlyteOS 浮空飞行器飞行模拟器  -  武汉福莱特航空科技有限公司',
                 fontsize=14, color='#00d4ff', fontweight='bold', y=0.98)

    gs = GridSpec(3, 4, figure=fig, hspace=0.45, wspace=0.4)
    ax_top  = fig.add_subplot(gs[0:2, 0:2])  # 俯视轨迹
    ax_side = fig.add_subplot(gs[0:2, 2:4])  # 侧视高度
    ax_att  = fig.add_subplot(gs[2, 0])      # 姿态
    ax_pwr  = fig.add_subplot(gs[2, 1])      # 电源
    ax_bat  = fig.add_subplot(gs[2, 2])      # 电池
    ax_info = fig.add_subplot(gs[2, 3])      # 状态信息

    style = {'facecolor': '#0d1117', 'text_color': '#e0e0e0',
             'grid': '#1f2937', 'accent': '#00d4ff', 'warn': '#ff6b35'}

    for ax in [ax_top, ax_side, ax_att, ax_pwr, ax_bat, ax_info]:
        ax.set_facecolor(style['facecolor'])
        ax.tick_params(colors=style['text_color'], labelsize=8)
        for sp in ax.spines.values(): sp.set_color(style['grid'])

    # 航点标注
    for n, e, alt_neg, _, label in DEMO_MISSION:
        ax_top.plot(e, n, 'o', color='#ffaa00', markersize=5, alpha=0.7)
        ax_top.annotate(label, (e, n), textcoords="offset points",
                        xytext=(5, 5), fontsize=7, color='#ffaa00')

    def update(_):
        s = sim.state
        t_arr = list(sim.hist_t)
        pos_arr = list(sim.hist_pos)

        # ── 俯视轨迹 ────────────────────────────────────
        ax_top.cla()
        ax_top.set_facecolor(style['facecolor'])
        ax_top.tick_params(colors=style['text_color'], labelsize=8)
        for sp in ax_top.spines.values(): sp.set_color(style['grid'])
        ax_top.set_title('俯视轨迹 (NED)', color=style['accent'], fontsize=9)
        ax_top.set_xlabel('East (m)', color=style['text_color'], fontsize=8)
        ax_top.set_ylabel('North (m)', color=style['text_color'], fontsize=8)

        for n, e, _, _, label in DEMO_MISSION:
            ax_top.plot(e, n, 'o', color='#ffaa00', markersize=6, alpha=0.8)
            ax_top.annotate(label, (e, n), textcoords="offset points",
                            xytext=(4, 4), fontsize=7, color='#ffaa00')

        if pos_arr:
            xs = [p[1] for p in pos_arr]
            ys = [p[0] for p in pos_arr]
            ax_top.plot(xs, ys, '-', color='#00d4ff', linewidth=1.2, alpha=0.8)
            ax_top.plot(xs[-1], ys[-1], 's', color='#ff4455', markersize=10, label='飞行器')
            # 机头方向箭头
            hy = math.cos(s.yaw) * 5
            hx = math.sin(s.yaw) * 5
            ax_top.annotate('', xy=(xs[-1]+hx, ys[-1]+hy), xytext=(xs[-1], ys[-1]),
                            arrowprops=dict(arrowstyle='->', color='#ff4455', lw=2))
        ax_top.grid(True, color=style['grid'], alpha=0.5)
        ax_top.legend(fontsize=8, facecolor='#1f2937', labelcolor='white', loc='upper left')

        # ── 侧视高度 ────────────────────────────────────
        ax_side.cla()
        ax_side.set_facecolor(style['facecolor'])
        ax_side.tick_params(colors=style['text_color'], labelsize=8)
        for sp in ax_side.spines.values(): sp.set_color(style['grid'])
        ax_side.set_title('高度曲线 (m)', color=style['accent'], fontsize=9)
        ax_side.set_xlabel('时间 (s)', color=style['text_color'], fontsize=8)
        ax_side.set_ylabel('高度 (m)', color=style['text_color'], fontsize=8)

        if t_arr:
            alt_arr = list(sim.hist_alt)
            ax_side.fill_between(t_arr, 0, alt_arr, alpha=0.3, color='#00d4ff')
            ax_side.plot(t_arr, alt_arr, '-', color='#00d4ff', linewidth=1.5)
            ax_side.axhline(y=-DEMO_MISSION[min(sim.mission_idx, len(DEMO_MISSION)-1)][2],
                            color='#ffaa00', linestyle='--', linewidth=1, alpha=0.7, label='目标高度')
        ax_side.set_ylim(bottom=0)
        ax_side.grid(True, color=style['grid'], alpha=0.5)
        ax_side.legend(fontsize=7, facecolor='#1f2937', labelcolor='white')

        # ── 姿态角 ─────────────────────────────────────
        ax_att.cla()
        ax_att.set_facecolor(style['facecolor'])
        ax_att.tick_params(colors=style['text_color'], labelsize=7)
        for sp in ax_att.spines.values(): sp.set_color(style['grid'])
        ax_att.set_title('姿态角 (°)', color=style['accent'], fontsize=9)
        if t_arr:
            ax_att.plot(t_arr, list(sim.hist_roll),  color='#ff6b6b', linewidth=1.2, label='横滚')
            ax_att.plot(t_arr, list(sim.hist_pitch), color='#ffd93d', linewidth=1.2, label='俯仰')
        ax_att.axhline(0, color=style['grid'], linestyle='-', linewidth=0.5)
        ax_att.set_ylim(-25, 25)
        ax_att.grid(True, color=style['grid'], alpha=0.5)
        ax_att.legend(fontsize=7, facecolor='#1f2937', labelcolor='white')

        # ── 太阳能功率 ─────────────────────────────────
        ax_pwr.cla()
        ax_pwr.set_facecolor(style['facecolor'])
        ax_pwr.tick_params(colors=style['text_color'], labelsize=7)
        for sp in ax_pwr.spines.values(): sp.set_color(style['grid'])
        ax_pwr.set_title('太阳能输出 (W)', color=style['accent'], fontsize=9)
        if t_arr:
            ax_pwr.fill_between(t_arr, list(sim.hist_solar), alpha=0.4, color='#ffd93d')
            ax_pwr.plot(t_arr, list(sim.hist_solar), color='#ffd93d', linewidth=1.2)
        ax_pwr.set_ylim(bottom=0)
        ax_pwr.grid(True, color=style['grid'], alpha=0.5)

        # ── 电池电量 ───────────────────────────────────
        ax_bat.cla()
        ax_bat.set_facecolor(style['facecolor'])
        ax_bat.tick_params(colors=style['text_color'], labelsize=7)
        for sp in ax_bat.spines.values(): sp.set_color(style['grid'])
        ax_bat.set_title('电池电量 (%)', color=style['accent'], fontsize=9)
        if t_arr:
            bat_arr = list(sim.hist_bat)
            clr = '#00ff88' if bat_arr[-1] > 30 else style['warn']
            ax_bat.fill_between(t_arr, bat_arr, alpha=0.4, color=clr)
            ax_bat.plot(t_arr, bat_arr, color=clr, linewidth=1.2)
        ax_bat.set_ylim(0, 105)
        ax_bat.grid(True, color=style['grid'], alpha=0.5)

        # ── 状态信息板 ─────────────────────────────────
        ax_info.cla()
        ax_info.set_facecolor('#0d1117')
        ax_info.axis('off')
        for sp in ax_info.spines.values(): sp.set_color(style['grid'])

        alt_m  = -s.pos.z
        spd    = math.sqrt(s.vel.x**2 + s.vel.y**2)
        buoy_p = s.buoyancy_ratio * 100

        state_color = {
            'LANDED': '#aaaaaa', 'STANDBY': '#ffaa00', 'TAKING_OFF': '#00d4ff',
            'WAYPOINT_NAV': '#00ff88', 'HOVERING': '#00d4ff', 'LANDING': '#ffd93d',
        }.get(s.flight_state, '#ffffff')

        lines = [
            ("系统状态",        s.flight_state,          state_color),
            ("当前任务",        sim.wp_label,             '#ffaa00'),
            ("",               "",                       '#333333'),
            ("高度",           f"{alt_m:6.1f} m",        '#00d4ff'),
            ("水平速度",        f"{spd:6.2f} m/s",        '#00d4ff'),
            ("垂向速度",        f"{-s.vel.z:+6.2f} m/s",  '#00d4ff'),
            ("",               "",                       '#333333'),
            ("氦气浮力",        f"{buoy_p:5.1f} %",       '#88ffcc'),
            ("太阳能功率",      f"{s.solar_power_w:5.0f} W", '#ffd93d'),
            ("电池",           f"{s.battery_pct:5.1f} %", '#00ff88' if s.battery_pct>30 else '#ff6b35'),
            ("",               "",                       '#333333'),
            ("飞行距离",        f"{sim.total_dist:6.0f} m", '#aaaaaa'),
            ("仿真时间",        f"{s.time_s:6.1f} s",     '#aaaaaa'),
        ]
        for i, (key, val, color) in enumerate(lines):
            if key:
                ax_info.text(0.02, 0.96 - i*0.072, f"{key}:", fontsize=8.5,
                             color='#888888', transform=ax_info.transAxes, va='top')
                ax_info.text(0.55, 0.96 - i*0.072, val, fontsize=8.5,
                             color=color, transform=ax_info.transAxes, va='top', fontweight='bold')

    from matplotlib.animation import FuncAnimation
    ani = FuncAnimation(fig, update, interval=100, cache_frame_data=False)
    plt.show()
    sim.running = False


# ═══════════════════════════════════════════════════════════════════
#  入口
# ═══════════════════════════════════════════════════════════════════
if __name__ == '__main__':
    print("=" * 60)
    print("  FlyteOS 浮空飞行器飞行模拟器")
    print("  武汉福莱特航空科技有限公司")
    print("=" * 60)
    print("  启动飞行仿真...")
    print("  自动执行演示航点任务（起飞 → 四边形航线 → 返航降落）")
    print("  关闭窗口即可退出")
    print("=" * 60)

    sim = FlyteSimulator()
    sim.speed = 3.0   # 3倍速仿真

    # 仿真线程
    t = threading.Thread(target=sim.run, daemon=True)
    t.start()

    # 主线程运行可视化（必须在主线程）
    time.sleep(0.3)  # 等待仿真预热
    launch_visualizer(sim)
