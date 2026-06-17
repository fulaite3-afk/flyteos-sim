"""
test_mission_manager.py — mission_manager 单元测试

测试覆盖：
- MissionState 枚举
- MissionConfig 配置
- Waypoint / WaypointMission 航点管理
- MissionManager 状态机
- Telemetry 解析
- 回调机制
- 超时检测
- 线程安全
"""

import json
import sys
import time
import unittest
from unittest.mock import MagicMock, patch
from pathlib import Path

# Ensure module is importable
sys.path.insert(0, str(Path(__file__).resolve().parent))

from mission_manager import (
    MissionState,
    RcCommand,
    MissionConfig,
    Waypoint,
    WaypointMission,
    Telemetry,
    MissionManager,
    DEFAULT_WAYPOINT_RADIUS,
    DEFAULT_TAKEOFF_ALTITUDE,
)


# ===================================================================
# 测试：枚举
# ===================================================================

class TestEnums(unittest.TestCase):
    """MissionState 和 RcCommand 枚举测试"""

    def test_mission_state_values(self):
        """所有状态值正确"""
        expected = ["DISARMED", "ARMED", "TAKEOFF", "MISSION", "HOLD", "RTL", "LAND", "ERROR"]
        self.assertEqual([s.value for s in MissionState], expected)

    def test_rc_command_values(self):
        """所有 RC 指令值正确"""
        self.assertEqual(RcCommand.ARM.value, "arm")
        self.assertEqual(RcCommand.DISARM.value, "disarm")
        self.assertEqual(RcCommand.TAKEOFF.value, "takeoff")
        self.assertEqual(RcCommand.RTL.value, "rtl")
        self.assertEqual(RcCommand.HOLD.value, "hold")
        self.assertEqual(RcCommand.LAND.value, "land")
        self.assertEqual(RcCommand.EMERGENCY.value, "emergency")


# ===================================================================
# 测试：MissionConfig
# ===================================================================

class TestMissionConfig(unittest.TestCase):
    """MissionConfig 配置类测试"""

    def test_default_config(self):
        """默认配置有合理的值"""
        cfg = MissionConfig()
        self.assertEqual(cfg.poll_interval, 0.5)
        self.assertEqual(cfg.waypoint_radius, DEFAULT_WAYPOINT_RADIUS)
        self.assertEqual(cfg.takeoff_altitude, DEFAULT_TAKEOFF_ALTITUDE)
        self.assertIn("pos_n", cfg.telemetry_field_mapping)

    def test_custom_config(self):
        """自定义配置覆盖默认值"""
        cfg = MissionConfig(
            telemetry_url="http://192.168.1.1:9000/api/telemetry",
            rc_url="http://192.168.1.1:9000/api/rc",
            poll_interval=0.1,
            waypoint_radius=10.0,
            arm_timeout=3.0,
        )
        self.assertEqual(cfg.telemetry_url, "http://192.168.1.1:9000/api/telemetry")
        self.assertEqual(cfg.rc_url, "http://192.168.1.1:9000/api/rc")
        self.assertEqual(cfg.poll_interval, 0.1)
        self.assertEqual(cfg.waypoint_radius, 10.0)
        self.assertEqual(cfg.arm_timeout, 3.0)


# ===================================================================
# 测试：Telemetry
# ===================================================================

class TestTelemetry(unittest.TestCase):
    """Telemetry 数据解析测试"""

    def test_from_dict(self):
        """从 JSON 字典构造 Telemetry"""
        raw = {
            "pos_n": 10.0,
            "pos_e": 20.0,
            "pos_d": -30.0,
            "vel_n": 1.0,
            "vel_e": 0.5,
            "vel_d": -0.2,
            "roll_rad": 0.1,
            "pitch_rad": 0.05,
            "yaw_rad": 1.57,
            "flight_state": "ARMED",
            "thrust_n": 5.0,
            "buoyancy_n": 120.0,
            "buoyancy_ratio": 0.95,
            "battery_pct": 87.5,
            "sim_time_s": 123.45,
            "step_count": 50000,
        }
        cfg = MissionConfig()
        telem = Telemetry.from_dict(raw, cfg)
        self.assertEqual(telem.pos_n, 10.0)
        self.assertEqual(telem.pos_e, 20.0)
        self.assertEqual(telem.pos_d, -30.0)
        self.assertEqual(telem.flight_state, "ARMED")
        self.assertEqual(telem.battery_pct, 87.5)
        self.assertEqual(telem.raw, raw)
        self.assertGreater(telem.timestamp, 0)

    def test_from_dict_partial(self):
        """部分字段缺失时使用默认值"""
        raw = {"pos_n": 5.0, "pos_e": 3.0}
        cfg = MissionConfig()
        telem = Telemetry.from_dict(raw, cfg)
        self.assertEqual(telem.pos_n, 5.0)
        self.assertEqual(telem.pos_e, 3.0)
        self.assertEqual(telem.pos_d, 0.0)  # 默认
        self.assertEqual(telem.vel_n, 0.0)

    def test_from_dict_empty(self):
        """空字典时全部默认"""
        cfg = MissionConfig()
        telem = Telemetry.from_dict({}, cfg)
        self.assertEqual(telem.pos_n, 0.0)
        self.assertEqual(telem.battery_pct, 100.0)


# ===================================================================
# 测试：Waypoint 和 WaypointMission
# ===================================================================

class TestWaypoint(unittest.TestCase):
    """Waypoint 数据类测试"""

    def test_default_waypoint(self):
        wp = Waypoint(lat=37.77, lon=-122.41, alt=100.0)
        self.assertEqual(wp.lat, 37.77)
        self.assertEqual(wp.lon, -122.41)
        self.assertEqual(wp.alt, 100.0)
        self.assertEqual(wp.hold_time, 0.0)
        self.assertIsNone(wp.acceptance_radius)
        self.assertEqual(wp.label, "")

    def test_waypoint_with_hold(self):
        wp = Waypoint(lat=37.77, lon=-122.41, alt=50.0, hold_time=5.0, label="WP1")
        self.assertEqual(wp.hold_time, 5.0)
        self.assertEqual(wp.label, "WP1")


class TestWaypointMission(unittest.TestCase):
    """WaypointMission 航点任务管理测试"""

    def setUp(self):
        self.wp1 = Waypoint(31.0, 121.0, 100.0, label="A")
        self.wp2 = Waypoint(31.1, 121.1, 120.0, label="B")
        self.wp3 = Waypoint(31.2, 121.2, 80.0, label="C", hold_time=2.0)

    def test_empty_mission(self):
        mission = WaypointMission()
        self.assertEqual(mission.total_count, 0)
        self.assertEqual(mission.remaining_count, 0)
        self.assertIsNone(mission.current_waypoint)
        self.assertFalse(mission.completed)

    def test_add_waypoints(self):
        mission = WaypointMission()
        mission.add_waypoint(self.wp1)
        mission.add_waypoints([self.wp2, self.wp3])
        self.assertEqual(mission.total_count, 3)
        self.assertEqual(mission.current_index, 0)
        self.assertEqual(mission.current_waypoint.label, "A")

    def test_reset(self):
        mission = WaypointMission([self.wp1, self.wp2, self.wp3])
        # 推进一个
        mission._advance()
        self.assertEqual(mission.current_index, 1)
        mission.reset()
        self.assertEqual(mission.current_index, 0)
        self.assertFalse(mission.completed)

    def test_clear(self):
        mission = WaypointMission([self.wp1, self.wp2])
        mission.clear()
        self.assertEqual(mission.total_count, 0)
        self.assertEqual(mission.current_index, 0)

    def test_completed_after_all_waypoints(self):
        mission = WaypointMission([self.wp1, self.wp2])
        mission._advance()
        self.assertEqual(mission.current_index, 1)
        mission._advance()
        self.assertEqual(mission.current_index, 2)
        self.assertTrue(mission.completed)
        self.assertEqual(mission.remaining_count, 0)

    def test_distance_3d(self):
        """NED 三维距离计算"""
        d = WaypointMission.distance_3d(0, 0, 0, 3, 4, 0)
        self.assertAlmostEqual(d, 5.0)
        d = WaypointMission.distance_3d(1, 2, 3, 4, 6, 3)
        self.assertAlmostEqual(d, 5.0)

    def test_check_arrival_within_radius(self):
        """到达判定：在半径内"""
        mission = WaypointMission([Waypoint(0, 0, 100.0)], default_radius=5.0)
        telem = Telemetry(pos_n=0.0, pos_e=0.0, pos_d=-100.0)
        # 模拟 check_arrival 内部的距离判定 — 需要 set up NED坐标系一致
        # 航点 (0,0,100m alt) → wp_d = -100
        # 遥测 pos_n=0, pos_e=0, pos_d=-100 → 距离 0
        self.assertTrue(mission.check_arrival(telem))
        self.assertTrue(mission.completed)

    def test_check_arrival_outside_radius(self):
        """到达判定：在半径外"""
        mission = WaypointMission([Waypoint(0, 0, 100.0)], default_radius=2.0)
        telem = Telemetry(pos_n=10.0, pos_e=10.0, pos_d=-100.0)
        # 水平距离 ≈ 14.14m > 2m
        self.assertFalse(mission.check_arrival(telem))
        self.assertFalse(mission.completed)

    def test_waypoints_list_immutable(self):
        """waypoints 属性返回副本"""
        mission = WaypointMission([self.wp1])
        wps = mission.waypoints
        wps.append(self.wp2)
        self.assertEqual(mission.total_count, 1)


# ===================================================================
# 测试：MissionManager 状态机
# ===================================================================

class TestMissionManager(unittest.TestCase):
    """MissionManager 状态机测试"""

    def setUp(self):
        self.mgr = MissionManager()

    def test_initial_state(self):
        """初始状态为 DISARMED"""
        self.assertEqual(self.mgr.state, MissionState.DISARMED)

    def test_state_transition_arm_disarm(self):
        """解锁→锁定"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.assertTrue(self.mgr.arm())
            self.assertEqual(self.mgr.state, MissionState.ARMED)
            self.assertTrue(self.mgr.disarm())
            self.assertEqual(self.mgr.state, MissionState.DISARMED)

    def test_cannot_arm_twice(self):
        """不能重复解锁"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.assertTrue(self.mgr.arm())
            self.assertFalse(self.mgr.arm())  # 已在 ARMED

    def test_takeoff_from_armed(self):
        """从 ARMED 起飞"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr._set_state(MissionState.ARMED)
            self.assertTrue(self.mgr.takeoff())
            self.assertEqual(self.mgr.state, MissionState.TAKEOFF)

    def test_takeoff_from_disarmed_fails(self):
        """不能从未解锁状态起飞"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.assertFalse(self.mgr.takeoff())
            self.assertEqual(self.mgr.state, MissionState.DISARMED)

    def test_rtl_sequence(self):
        """在 MISSION 状态触发 RTL"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr._set_state(MissionState.MISSION)
            self.assertTrue(self.mgr.rtl())
            self.assertEqual(self.mgr.state, MissionState.RTL)

    def test_hold_from_mission(self):
        """在 MISSION 状态悬停"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr._set_state(MissionState.MISSION)
            self.assertTrue(self.mgr.hold())
            self.assertEqual(self.mgr.state, MissionState.HOLD)

    def test_land_from_mission(self):
        """在 MISSION 状态降落"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr._set_state(MissionState.MISSION)
            self.assertTrue(self.mgr.land())
            self.assertEqual(self.mgr.state, MissionState.LAND)

    def test_emergency_any_state(self):
        """任意状态均可触发紧急停机"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr._set_state(MissionState.MISSION)
            self.assertTrue(self.mgr.emergency())
            self.assertEqual(self.mgr.state, MissionState.ERROR)
            self.assertIn("Emergency", self.mgr.last_error)

    def test_cannot_arm_from_error_without_manual(self):
        """ERROR 状态不允许直接 arm（需要先 disarm 重置）"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr._set_state(MissionState.ERROR)
            self.assertFalse(self.mgr.arm())

    def test_custom_takeoff_altitude(self):
        """自定义起飞高度"""
        with patch.object(self.mgr, '_send_rc', return_value=True) as mock_rc:
            self.mgr._set_state(MissionState.ARMED)
            self.mgr.takeoff(altitude=50.0)
            call_args = mock_rc.call_args
            self.assertIsNotNone(call_args)
            # call_args = (args, kwargs) where args = (RcCommand.TAKEOFF, {"altitude": 50.0})
            pos_args, _ = call_args
            self.assertEqual(pos_args[1].get("altitude"), 50.0)

    def test_start_mission_from_takeoff(self):
        """TAKEOFF 状态可以加载任务并转为 MISSION"""
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr._set_state(MissionState.TAKEOFF)
            mission = WaypointMission([Waypoint(31.0, 121.0, 100.0)])
            self.assertTrue(self.mgr.start_mission(mission))
            self.assertEqual(self.mgr.state, MissionState.MISSION)


# ===================================================================
# 测试：回调机制
# ===================================================================

class TestCallbacks(unittest.TestCase):
    """状态回调测试"""

    def setUp(self):
        self.mgr = MissionManager()

    def test_state_callback_fires(self):
        """状态变更时回调触发"""
        self.called_state = None

        def callback(state):
            self.called_state = state

        self.mgr.on_state_change(MissionState.ARMED, callback)
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr.arm()
        self.assertEqual(self.called_state, MissionState.ARMED)

    def test_telemetry_callback_fires(self):
        """遥测更新时回调触发"""
        self.received = None

        def callback(telem):
            self.received = telem

        self.mgr.on_telemetry(callback)
        raw = {"pos_n": 100.0, "pos_e": 50.0}
        self.mgr._update_telemetry(raw)
        self.assertIsNotNone(self.received)
        self.assertEqual(self.received.pos_n, 100.0)
        self.assertEqual(self.received.pos_e, 50.0)

    def test_clear_callbacks(self):
        """清除回调后不再触发"""
        self.called = False

        def callback(state):
            self.called = True  # noqa

        self.mgr.on_state_change(MissionState.ARMED, callback)
        self.mgr.clear_callbacks()
        with patch.object(self.mgr, '_send_rc', return_value=True):
            self.mgr.arm()
        self.assertFalse(self.called)


# ===================================================================
# 测试：超时检测
# ===================================================================

class TestTimeout(unittest.TestCase):
    """超时检测测试"""

    def test_timeout_transitions_to_error(self):
        """超时后进入 ERROR 状态"""
        mgr = MissionManager(MissionConfig(arm_timeout=0.01, takeoff_timeout=0.01))
        mgr._set_state(MissionState.TAKEOFF)
        mgr._state_enter_time = time.time() - 0.5  # 模拟已过0.5s
        mgr._timeout = 0.01
        mgr._check_timeout()
        self.assertEqual(mgr.state, MissionState.ERROR)

    def test_no_timeout_when_in_time(self):
        """超时时间内不触发"""
        mgr = MissionManager(MissionConfig(arm_timeout=999.0))
        mgr._set_state(MissionState.TAKEOFF)
        mgr._check_timeout()
        self.assertEqual(mgr.state, MissionState.TAKEOFF)


# ===================================================================
# 测试：线程安全
# ===================================================================

class TestThreadSafety(unittest.TestCase):
    """线程安全性测试"""

    def test_concurrent_state_reads(self):
        """多线程读取状态不会报错"""
        import threading

        mgr = MissionManager()
        errors = []

        def reader():
            try:
                for _ in range(100):
                    _ = mgr.state
                    _ = mgr.telemetry
                    _ = mgr.last_error
            except Exception as e:
                errors.append(e)

        threads = [threading.Thread(target=reader) for _ in range(10)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(len(errors), 0, f"Thread errors: {errors}")


# ===================================================================
# 测试：遥测处理逻辑（模拟）
# ===================================================================

class TestTelemetryProcessing(unittest.TestCase):
    """遥测处理逻辑测试"""

    def test_telem_snapshot(self):
        """get_telemetry_snapshot 返回最新遥测"""
        mgr = MissionManager()
        raw = {"pos_n": 42.0, "pos_e": -7.0, "pos_d": -15.0}
        mgr._update_telemetry(raw)
        snap = mgr.get_telemetry_snapshot()
        self.assertIsNotNone(snap)
        self.assertEqual(snap.pos_n, 42.0)
        self.assertEqual(snap.pos_e, -7.0)

    def test_get_position(self):
        """get_position 返回 (pos_n, pos_e, pos_d)"""
        mgr = MissionManager()
        raw = {"pos_n": 1.0, "pos_e": 2.0, "pos_d": -3.0}
        mgr._update_telemetry(raw)
        self.assertEqual(mgr.get_position(), (1.0, 2.0, -3.0))

    def test_get_altitude(self):
        """get_altitude 返回 -pos_d"""
        mgr = MissionManager()
        raw = {"pos_d": -25.0}
        mgr._update_telemetry(raw)
        self.assertEqual(mgr.get_altitude(), 25.0)

    def test_process_state_takeoff_completes(self):
        """TAKEOFF 达到高度后转为 HOLD（无任务）"""
        mgr = MissionManager()
        raw = {"pos_d": -DEFAULT_TAKEOFF_ALTITUDE}  # 已达起飞高度
        mgr._set_state(MissionState.TAKEOFF)
        telem = Telemetry.from_dict(raw, mgr.config)
        mgr._process_state(telem)
        self.assertEqual(mgr.state, MissionState.HOLD)

    def test_process_state_land_completes(self):
        """LAND 状态检测着陆完成"""
        mgr = MissionManager()
        raw = {"pos_d": 0.0, "vel_d": 0.0}
        mgr._set_state(MissionState.LAND)
        telem = Telemetry.from_dict(raw, mgr.config)
        mgr._process_state(telem)
        self.assertEqual(mgr.state, MissionState.DISARMED)


# ===================================================================
# 运行
# ===================================================================

if __name__ == "__main__":
    unittest.main(verbosity=2)
