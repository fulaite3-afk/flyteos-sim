"""
mission_manager.py - FlyteOS v2.0 飞行任务管理器

核心功能：
1. 通过 HTTP GET 从 /api/telemetry 获取实时遥测 JSON
2. 通过 HTTP POST 发送遥控指令到 /api/rc
3. 任务状态机：DISARMED → ARMED → TAKEOFF → MISSION → RTL → LAND
4. 航点任务的加载、执行和监控
5. 紧急返航（RTL）、悬停（HOLD）、降落（LAND）命令
6. 任务状态回调机制

与 C++ 后端接口：
- GET  /api/telemetry  → 返回 JSON 遥测数据
- POST /api/rc         → 发送遥控指令（JSON body）
"""

from __future__ import annotations

import enum
import json
import logging
import math
import threading
import time
import urllib.request
import urllib.error
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

DEFAULT_TELEMETRY_URL = "http://localhost:8080/api/telemetry"
DEFAULT_RC_URL = "http://localhost:8080/api/rc"
DEFAULT_POLL_INTERVAL = 0.5          # 遥测轮询间隔（秒）
DEFAULT_WAYPOINT_RADIUS = 5.0        # 航点到达判定半径（米）
DEFAULT_TAKEOFF_ALTITUDE = 30.0      # 默认起飞高度（米）
DEFAULT_RTL_ALTITUDE = 50.0          # 默认返航高度（米）
DEFAULT_LAND_VELOCITY = -0.5         # 着陆下降速率（m/s，NED下为正）
DEFAULT_ARM_TIMEOUT = 5.0            # 解锁超时（秒）
DEFAULT_TAKEOFF_TIMEOUT = 30.0       # 起飞超时（秒）
DEFAULT_WAYPOINT_TIMEOUT = 120.0     # 单航点超时（秒）
DEFAULT_RTL_TIMEOUT = 300.0          # 返航超时（秒）
DEFAULT_LAND_TIMEOUT = 60.0          # 着陆超时（秒）

EARTH_RADIUS_M = 6378137.0


# ---------------------------------------------------------------------------
# 枚举
# ---------------------------------------------------------------------------

class MissionState(enum.Enum):
    """任务状态机状态"""
    DISARMED = "DISARMED"    # 未解锁
    ARMED = "ARMED"          # 已解锁，地面待命
    TAKEOFF = "TAKEOFF"      # 起飞中
    MISSION = "MISSION"      # 执行航点任务
    HOLD = "HOLD"            # 悬停保持
    RTL = "RTL"              # 返航中
    LAND = "LAND"            # 降落中
    ERROR = "ERROR"          # 异常状态


class RcCommand(enum.Enum):
    """遥控指令类型"""
    ARM = "arm"
    DISARM = "disarm"
    TAKEOFF = "takeoff"
    RTL = "rtl"
    HOLD = "hold"
    LAND = "land"
    SET_WAYPOINT = "set_waypoint"
    CLEAR_MISSION = "clear_mission"
    EMERGENCY = "emergency"


# ---------------------------------------------------------------------------
# 数据类
# ---------------------------------------------------------------------------

@dataclass
class MissionConfig:
    """任务配置"""
    telemetry_url: str = DEFAULT_TELEMETRY_URL
    rc_url: str = DEFAULT_RC_URL
    poll_interval: float = DEFAULT_POLL_INTERVAL
    waypoint_radius: float = DEFAULT_WAYPOINT_RADIUS
    takeoff_altitude: float = DEFAULT_TAKEOFF_ALTITUDE
    rtl_altitude: float = DEFAULT_RTL_ALTITUDE
    land_velocity: float = DEFAULT_LAND_VELOCITY

    # 超时配置
    arm_timeout: float = DEFAULT_ARM_TIMEOUT
    takeoff_timeout: float = DEFAULT_TAKEOFF_TIMEOUT
    waypoint_timeout: float = DEFAULT_WAYPOINT_TIMEOUT
    rtl_timeout: float = DEFAULT_RTL_TIMEOUT
    land_timeout: float = DEFAULT_LAND_TIMEOUT

    # 遥测字段映射（C++ SITL TelemetrySnapshot → Python 字段）
    telemetry_field_mapping: Dict[str, str] = field(default_factory=lambda: {
        "pos_n": "pos_n",
        "pos_e": "pos_e",
        "pos_d": "pos_d",
        "vel_n": "vel_n",
        "vel_e": "vel_e",
        "vel_d": "vel_d",
        "roll_rad": "roll_rad",
        "pitch_rad": "pitch_rad",
        "yaw_rad": "yaw_rad",
        "flight_state": "flight_state",
        "thrust_n": "thrust_n",
        "buoyancy_n": "buoyancy_n",
        "buoyancy_ratio": "buoyancy_ratio",
        "battery_pct": "battery_pct",
        "sim_time_s": "sim_time_s",
        "step_count": "step_count",
    })


@dataclass
class Waypoint:
    """单个航点"""
    lat: float                     # 纬度 (deg)
    lon: float                     # 经度 (deg)
    alt: float                     # 高度 (m, NED down 为负)
    hold_time: float = 0.0         # 到达后悬停时间（秒）
    acceptance_radius: Optional[float] = None  # 到达半径（None 则用全局配置）
    label: str = ""


@dataclass
class Telemetry:
    """遥测数据快照"""
    pos_n: float = 0.0
    pos_e: float = 0.0
    pos_d: float = 0.0
    vel_n: float = 0.0
    vel_e: float = 0.0
    vel_d: float = 0.0
    roll_rad: float = 0.0
    pitch_rad: float = 0.0
    yaw_rad: float = 0.0
    flight_state: str = "DISARMED"
    thrust_n: float = 0.0
    buoyancy_n: float = 0.0
    buoyancy_ratio: float = 0.0
    battery_pct: float = 100.0
    sim_time_s: float = 0.0
    step_count: int = 0
    raw: Dict[str, Any] = field(default_factory=dict)
    timestamp: float = 0.0

    @classmethod
    def from_dict(cls, data: Dict[str, Any], config: MissionConfig) -> Telemetry:
        """从遥测 JSON 字典构造 Telemetry 对象"""
        mapping = config.telemetry_field_mapping
        kwargs: Dict[str, Any] = {"raw": data, "timestamp": time.time()}
        for src_key, dst_key in mapping.items():
            if src_key in data:
                kwargs[dst_key] = data[src_key]
        return cls(**kwargs)


# ---------------------------------------------------------------------------
# WaypointMission - 航点任务管理
# ---------------------------------------------------------------------------

class WaypointMission:
    """航点任务，包含航点列表管理和到达判定"""

    def __init__(
        self,
        waypoints: Optional[List[Waypoint]] = None,
        default_radius: float = DEFAULT_WAYPOINT_RADIUS,
    ):
        self._waypoints: List[Waypoint] = list(waypoints) if waypoints else []
        self._default_radius = default_radius
        self._current_index: int = 0
        self._completed: bool = False
        self._arrival_time: Optional[float] = None
        self._mission_start_time: Optional[float] = None

    # ---- 属性 ----

    @property
    def waypoints(self) -> List[Waypoint]:
        return list(self._waypoints)

    @property
    def current_index(self) -> int:
        return self._current_index

    @property
    def current_waypoint(self) -> Optional[Waypoint]:
        if 0 <= self._current_index < len(self._waypoints):
            return self._waypoints[self._current_index]
        return None

    @property
    def completed(self) -> bool:
        return self._completed

    @property
    def total_count(self) -> int:
        return len(self._waypoints)

    @property
    def remaining_count(self) -> int:
        if self._completed:
            return 0
        return max(0, len(self._waypoints) - self._current_index)

    # ---- 航点管理 ----

    def add_waypoint(self, wp: Waypoint) -> None:
        """添加航点"""
        self._waypoints.append(wp)
        self._completed = False

    def add_waypoints(self, wps: List[Waypoint]) -> None:
        """批量添加航点"""
        self._waypoints.extend(wps)
        self._completed = False

    def clear(self) -> None:
        """清空航点并重置"""
        self._waypoints.clear()
        self.reset()

    def reset(self) -> None:
        """重置任务进度"""
        self._current_index = 0
        self._completed = False
        self._arrival_time = None
        self._mission_start_time = None

    def start(self) -> None:
        """标记任务开始（记录开始时间）"""
        self._mission_start_time = time.time()

    # ---- 距离计算 ----

    @staticmethod
    def _haversine_distance(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
        """Haversine 公式计算两点间水平距离（米）"""
        dlat = math.radians(lat2 - lat1)
        dlon = math.radians(lon2 - lon1)
        a = (
            math.sin(dlat / 2) ** 2
            + math.cos(math.radians(lat1))
            * math.cos(math.radians(lat2))
            * math.sin(dlon / 2) ** 2
        )
        return 2 * EARTH_RADIUS_M * math.atan2(math.sqrt(a), math.sqrt(1 - a))

    @staticmethod
    def distance_3d(
        pos_n: float, pos_e: float, pos_d: float,
        wp_n: float, wp_e: float, wp_d: float,
    ) -> float:
        """NED 坐标系下三维距离"""
        dn = wp_n - pos_n
        de = wp_e - pos_e
        dd = wp_d - pos_d
        return math.sqrt(dn * dn + de * de + dd * dd)

    # ---- 到达判定 ----

    def check_arrival(self, telemetry: Telemetry) -> bool:
        """
        检查是否到达当前航点（基于 NED 坐标的三维距离判定）

        返回 True 表示已到达，自动推进到下一航点。
        """
        wp = self.current_waypoint
        if wp is None:
            self._completed = True
            return False

        radius = wp.acceptance_radius if wp.acceptance_radius is not None else self._default_radius

        dist = self.distance_3d(
            telemetry.pos_n, telemetry.pos_e, telemetry.pos_d,
            wp_n=0.0, wp_e=0.0, wp_d=-wp.alt,  # 简化：航点 lat/lon 需要 NED 转换
        )
        # 注意：航点使用 lat/lon/alt，遥测使用 pos_n/pos_e/pos_d (NED)。
        # 实际使用中需要坐标转换。这里提供基于 lat/lon 的水平距离判定 +
        # 高度差判定的简化实现。

        # 水平距离（Haversine，需要 origin 位置做参考）
        # 为支持纯 NED 航点，同时提供 NED 距离判定
        h_dist = self._haversine_distance(
            telemetry.pos_n / (EARTH_RADIUS_M * math.pi / 180),
            telemetry.pos_e / (EARTH_RADIUS_M * math.pi / 180 * math.cos(
                math.radians(telemetry.pos_n / (EARTH_RADIUS_M * math.pi / 180))
            )),
            wp.lat, wp.lon,
        ) if abs(wp.lat) > 1e-9 or abs(wp.lon) > 1e-9 else dist

        alt_diff = abs(-telemetry.pos_d - wp.alt)

        if h_dist <= radius and alt_diff <= radius:
            if wp.hold_time > 0:
                if self._arrival_time is None:
                    self._arrival_time = time.time()
                elif time.time() - self._arrival_time >= wp.hold_time:
                    self._advance()
            else:
                self._advance()
            return True

        # 重置悬停计时（如果离开航点）
        if h_dist > radius * 1.5:
            self._arrival_time = None

        return False

    def _advance(self) -> None:
        """推进到下一航点"""
        self._arrival_time = None
        self._current_index += 1
        if self._current_index >= len(self._waypoints):
            self._completed = True


# ---------------------------------------------------------------------------
# MissionManager - 任务管理器
# ---------------------------------------------------------------------------

class MissionManager:
    """
    飞行任务管理器 — 飞行器任务状态机核心

    状态转换图：
        DISARMED → ARMED → TAKEOFF → MISSION → RTL → LAND
                      ↑         ↓         ↓       ↓
                      └─── 任意状态可触发 HOLD ──────┘
                      └─── 任意状态可触发 EMERGENCY ─┘
    """

    def __init__(self, config: Optional[MissionConfig] = None):
        self._config = config or MissionConfig()
        self._state = MissionState.DISARMED
        self._mission: Optional[WaypointMission] = None
        self._telemetry: Optional[Telemetry] = None

        # 状态进入时间 & 超时
        self._state_enter_time: float = time.time()
        self._timeout: float = 0.0

        # 回调
        self._state_callbacks: Dict[MissionState, List[Callable[[MissionState], None]]] = {
            s: [] for s in MissionState
        }
        self._telemetry_callbacks: List[Callable[[Telemetry], None]] = []

        # 线程控制
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()

        # 错误信息
        self._last_error: str = ""

    # ---- 属性 ----

    @property
    def state(self) -> MissionState:
        with self._lock:
            return self._state

    @property
    def telemetry(self) -> Optional[Telemetry]:
        with self._lock:
            return self._telemetry

    @property
    def mission(self) -> Optional[WaypointMission]:
        with self._lock:
            return self._mission

    @property
    def config(self) -> MissionConfig:
        return self._config

    @property
    def is_running(self) -> bool:
        with self._lock:
            return self._running

    @property
    def last_error(self) -> str:
        with self._lock:
            return self._last_error

    # ---- 回调注册 ----

    def on_state_change(self, state: MissionState, callback: Callable[[MissionState], None]) -> None:
        """注册状态变更回调"""
        if state in self._state_callbacks:
            self._state_callbacks[state].append(callback)

    def on_telemetry(self, callback: Callable[[Telemetry], None]) -> None:
        """注册遥测数据回调"""
        self._telemetry_callbacks.append(callback)

    def clear_callbacks(self) -> None:
        """清除所有回调"""
        for lst in self._state_callbacks.values():
            lst.clear()
        self._telemetry_callbacks.clear()

    # ---- HTTP 通信 ----

    def _fetch_telemetry(self) -> Optional[Dict[str, Any]]:
        """从 /api/telemetry 获取遥测 JSON"""
        try:
            req = urllib.request.Request(
                self._config.telemetry_url,
                headers={"Accept": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                if resp.status == 200:
                    return json.loads(resp.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError,
                json.JSONDecodeError, OSError, TimeoutError) as e:
            logger.debug("Telemetry fetch failed: %s", e)
        return None

    def _send_rc(self, command: RcCommand, params: Optional[Dict[str, Any]] = None) -> bool:
        """通过 POST /api/rc 发送遥控指令"""
        payload: Dict[str, Any] = {"command": command.value}
        if params:
            payload["params"] = params
        try:
            data = json.dumps(payload).encode("utf-8")
            req = urllib.request.Request(
                self._config.rc_url,
                data=data,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                return resp.status in (200, 201, 204)
        except (urllib.error.URLError, urllib.error.HTTPError,
                OSError, TimeoutError) as e:
            logger.warning("RC command %s failed: %s", command.value, e)
            return False

    # ---- 状态机 ----

    def _set_state(self, new_state: MissionState, error_msg: str = "") -> None:
        """切换状态并触发回调"""
        old_state = self._state
        if old_state == new_state:
            return

        self._state = new_state
        self._state_enter_time = time.time()
        self._timeout = self._get_timeout_for_state(new_state)

        if error_msg:
            self._last_error = error_msg

        logger.info("State transition: %s → %s", old_state.value, new_state.value)

        for cb in self._state_callbacks.get(new_state, []):
            try:
                cb(new_state)
            except Exception as e:
                logger.error("State callback error: %s", e)

        # 如果进入 ERROR，也会触发通用 ERROR 回调
        if new_state == MissionState.ERROR:
            for cb in self._state_callbacks.get(MissionState.ERROR, []):
                try:
                    cb(new_state)
                except Exception as e:
                    logger.error("Error callback error: %s", e)

    def _get_timeout_for_state(self, state: MissionState) -> float:
        return {
            MissionState.ARMED: self._config.arm_timeout,
            MissionState.TAKEOFF: self._config.takeoff_timeout,
            MissionState.MISSION: self._config.waypoint_timeout,
            MissionState.RTL: self._config.rtl_timeout,
            MissionState.LAND: self._config.land_timeout,
        }.get(state, 0.0)

    def _check_timeout(self) -> None:
        """检查当前状态是否超时"""
        if self._timeout <= 0:
            return
        elapsed = time.time() - self._state_enter_time
        if elapsed > self._timeout:
            logger.warning("State %s timed out (%.1fs > %.1fs)", self._state.value, elapsed, self._timeout)
            self._set_state(MissionState.ERROR, f"Timeout in state {self._state.value}")

    # ---- 任务控制命令 ----

    def arm(self) -> bool:
        """解锁飞行器"""
        if self._state != MissionState.DISARMED:
            logger.warning("Cannot arm from state %s", self._state.value)
            return False
        if self._send_rc(RcCommand.ARM):
            self._set_state(MissionState.ARMED)
            return True
        return False

    def disarm(self) -> bool:
        """锁定飞行器"""
        if self._send_rc(RcCommand.DISARM):
            self._set_state(MissionState.DISARMED)
            return True
        return False

    def takeoff(self, altitude: Optional[float] = None) -> bool:
        """起飞到指定高度"""
        if self._state != MissionState.ARMED:
            logger.warning("Cannot takeoff from state %s", self._state.value)
            return False
        alt = altitude if altitude is not None else self._config.takeoff_altitude
        if self._send_rc(RcCommand.TAKEOFF, {"altitude": alt}):
            self._set_state(MissionState.TAKEOFF)
            return True
        return False

    def start_mission(self, mission: WaypointMission) -> bool:
        """加载并开始航点任务"""
        if self._state != MissionState.MISSION and self._state != MissionState.TAKEOFF:
            logger.warning("Cannot start mission from state %s", self._state.value)
            return False

        with self._lock:
            self._mission = mission
            self._mission.start()

        if self._send_rc(RcCommand.SET_WAYPOINT, {
            "index": 0,
            "total": mission.total_count,
        }):
            self._set_state(MissionState.MISSION)
            return True
        return False

    def rtl(self) -> bool:
        """触发返航"""
        if self._state in (MissionState.DISARMED, MissionState.LAND, MissionState.ERROR):
            logger.warning("Cannot RTL from state %s", self._state.value)
            return False
        if self._send_rc(RcCommand.RTL, {"altitude": self._config.rtl_altitude}):
            self._set_state(MissionState.RTL)
            return True
        return False

    def hold(self) -> bool:
        """悬停保持当前位置"""
        if self._state in (MissionState.DISARMED, MissionState.ERROR):
            logger.warning("Cannot hold from state %s", self._state.value)
            return False
        if self._send_rc(RcCommand.HOLD):
            self._set_state(MissionState.HOLD)
            return True
        return False

    def land(self) -> bool:
        """降落"""
        if self._state in (MissionState.DISARMED, MissionState.LAND, MissionState.ERROR):
            logger.warning("Cannot land from state %s", self._state.value)
            return False
        if self._send_rc(RcCommand.LAND, {"velocity": self._config.land_velocity}):
            self._set_state(MissionState.LAND)
            return True
        return False

    def emergency(self) -> bool:
        """紧急停机"""
        if self._send_rc(RcCommand.EMERGENCY):
            self._set_state(MissionState.ERROR, "Emergency stop triggered")
            return True
        return False

    # ---- 遥测监控循环 ----

    def _update_telemetry(self, raw: Dict[str, Any]) -> None:
        """更新遥测数据并通知回调"""
        telem = Telemetry.from_dict(raw, self._config)
        with self._lock:
            self._telemetry = telem

        for cb in self._telemetry_callbacks:
            try:
                cb(telem)
            except Exception as e:
                logger.error("Telemetry callback error: %s", e)

    def _process_state(self, telem: Telemetry) -> None:
        """基于遥测数据执行状态机逻辑"""

        # 超时检查
        self._check_timeout()

        # 状态机转换逻辑
        if self._state == MissionState.TAKEOFF:
            # 检测是否达到起飞高度
            if abs(telem.pos_d) >= self._config.takeoff_altitude * 0.95:
                if self._mission is not None:
                    self._set_state(MissionState.MISSION)
                else:
                    self._set_state(MissionState.HOLD)

        elif self._state == MissionState.MISSION:
            if self._mission is None:
                self._set_state(MissionState.HOLD)
                return
            # 到达判定
            wp = self._mission.current_waypoint
            if wp is not None:
                self._mission.check_arrival(telem)
                if self._mission.completed:
                    logger.info("All waypoints completed, transitioning to HOLD")
                    self._set_state(MissionState.HOLD)

        elif self._state == MissionState.RTL:
            # 检测是否到达返航点（home = NED origin）
            dist_to_home = math.sqrt(
                telem.pos_n ** 2 + telem.pos_e ** 2 + telem.pos_d ** 2
            )
            if dist_to_home < self._config.waypoint_radius:
                logger.info("RTL complete, transitioning to LAND")
                self.land()

        elif self._state == MissionState.LAND:
            # 检测是否已着陆（高度接近0且速度很小）
            if abs(telem.pos_d) < 0.5 and abs(telem.vel_d) < 0.3:
                logger.info("Landing complete, transitioning to DISARMED")
                self._set_state(MissionState.DISARMED)

    # ---- 主循环 ----

    def _run_loop(self) -> None:
        """遥测轮询主循环"""
        while self._running:
            raw = self._fetch_telemetry()
            if raw is not None:
                self._update_telemetry(raw)
                telem = self._telemetry
                if telem is not None:
                    self._process_state(telem)
            time.sleep(self._config.poll_interval)

    def start(self) -> None:
        """启动遥测监控线程"""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run_loop, daemon=True, name="MissionManager")
        self._thread.start()
        logger.info("MissionManager started (poll interval: %.1fs)", self._config.poll_interval)

    def stop(self) -> None:
        """停止遥测监控线程"""
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5.0)
        logger.info("MissionManager stopped")

    def wait_for_state(self, state: MissionState, timeout: float = 30.0) -> bool:
        """阻塞等待直到进入指定状态"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._state == state:
                return True
            time.sleep(0.1)
        return False

    # ---- 遥测快照 ----

    def get_telemetry_snapshot(self) -> Optional[Telemetry]:
        """获取当前遥测快照"""
        with self._lock:
            return self._telemetry

    def get_position(self) -> Tuple[float, float, float]:
        """获取当前位置 (pos_n, pos_e, pos_d)"""
        telem = self._telemetry
        if telem is None:
            return (0.0, 0.0, 0.0)
        return (telem.pos_n, telem.pos_e, telem.pos_d)

    def get_altitude(self) -> float:
        """获取当前高度（-pos_d，向上为正）"""
        telem = self._telemetry
        if telem is None:
            return 0.0
        return -telem.pos_d


# ---------------------------------------------------------------------------
# __main__ 入口
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
    print("FlyteOS MissionManager v2.0 — module loaded successfully")
    print(f"  Supported states: {[s.value for s in MissionState]}")
    print(f"  Supported commands: {[c.value for c in RcCommand]}")
