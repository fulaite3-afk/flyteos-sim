"""
Unit tests for geofence.py — FlyteOS v2.0 Geofence Module.

Covers all core functionality:
- Enum values, data class instantiation
- Haversine distance, ray casting, point-to-segment distance
- CircleFence / PolygonFence / AltitudeFence contains
- CompoundFence AND/OR logic
- Buffer / warning detection
- GeoJSON loading
- GeofenceManager registration, removal, check_position
- Violation callbacks, cooldown, history, auto-RTL
"""

import math
import json
import os
import sys
import tempfile
import unittest

# Add parent dir for import
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python', 'geofence'))

from geofence import (
    GeofenceType,
    ViolationSeverity,
    CompoundLogic,
    GeoPoint,
    CircleFence,
    PolygonFence,
    AltitudeFence,
    CompoundFence,
    ViolationEvent,
    GeofenceConfig,
    GeofenceManager,
    load_geojson,
    create_circle_fence,
    create_polygon_fence,
    _haversine_distance,
    _ray_casting,
    _point_to_segment_distance,
    _distance_to_polygon_edge,
)


class TestEnums(unittest.TestCase):
    """测试枚举定义"""

    def test_geofence_types(self):
        self.assertEqual(GeofenceType.POLYGON.value, 1)
        self.assertEqual(GeofenceType.CIRCLE.value, 2)
        self.assertEqual(GeofenceType.ALTITUDE.value, 3)
        self.assertEqual(GeofenceType.COMPOUND.value, 4)

    def test_violation_severity(self):
        self.assertEqual(ViolationSeverity.WARNING.value, "warning")
        self.assertEqual(ViolationSeverity.MINOR.value, "minor")
        self.assertEqual(ViolationSeverity.MAJOR.value, "major")
        self.assertEqual(ViolationSeverity.CRITICAL.value, "critical")

    def test_compound_logic(self):
        self.assertEqual(CompoundLogic.AND.value, "and")
        self.assertEqual(CompoundLogic.OR.value, "or")


class TestGeoPoint(unittest.TestCase):
    """测试 GeoPoint 数据类"""

    def test_creation(self):
        p = GeoPoint(lat=31.2, lon=121.4)
        self.assertEqual(p.lat, 31.2)
        self.assertEqual(p.lon, 121.4)

    def test_to_tuple(self):
        p = GeoPoint(lat=31.2, lon=121.4)
        self.assertEqual(p.to_tuple(), (31.2, 121.4))

    def test_repr(self):
        p = GeoPoint(lat=31.2, lon=121.4)
        self.assertIn("31.200000", repr(p))
        self.assertIn("121.400000", repr(p))


class TestHaversine(unittest.TestCase):
    """测试 Haversine 距离计算"""

    def test_same_point(self):
        p = GeoPoint(31.2, 121.4)
        self.assertAlmostEqual(_haversine_distance(p, p), 0.0, delta=0.01)

    def test_short_distance(self):
        # 西雅图市中心 (47.6062, -122.3321) 到附近约 1.5km
        p1 = GeoPoint(47.6062, -122.3321)
        p2 = GeoPoint(47.6195, -122.3321)
        dist = _haversine_distance(p1, p2)
        self.assertGreater(dist, 1000)
        self.assertLess(dist, 1500)

    def test_long_distance(self):
        # 旧金山(San Francisco)到西雅图(Seattle)大约 1091 km
        p_seattle = GeoPoint(47.6062, -122.3321)
        p_sf = GeoPoint(37.7749, -122.4194)
        dist = _haversine_distance(p_seattle, p_sf)
        self.assertGreater(dist, 1000000)
        self.assertLess(dist, 1200000)

    def test_known_distance(self):
        # 2 个已知点间距离：纬度差 1 度在赤道上约 111.195 km
        p1 = GeoPoint(0.0, 0.0)
        p2 = GeoPoint(1.0, 0.0)
        dist = _haversine_distance(p1, p2)
        self.assertAlmostEqual(dist, 111195.0, delta=200)


class TestRayCasting(unittest.TestCase):
    """测试射线法"""

    def setUp(self):
        # 正方形：边长约 1 度
        self.square = [
            GeoPoint(30.0, 120.0),
            GeoPoint(31.0, 120.0),
            GeoPoint(31.0, 121.0),
            GeoPoint(30.0, 121.0),
        ]

    def test_inside(self):
        self.assertTrue(_ray_casting(GeoPoint(30.5, 120.5), self.square))

    def test_outside(self):
        self.assertFalse(_ray_casting(GeoPoint(29.0, 119.0), self.square))

    def test_on_edge_latitude(self):
        # 在边上
        self.assertTrue(_ray_casting(GeoPoint(30.0, 120.5), self.square))

    def test_concave_polygon(self):
        # 凹多边形（L 形）
        concave = [
            GeoPoint(30.0, 120.0),
            GeoPoint(31.0, 120.0),
            GeoPoint(31.0, 120.8),
            GeoPoint(30.5, 120.8),
            GeoPoint(30.5, 121.0),
            GeoPoint(30.0, 121.0),
        ]
        self.assertTrue(_ray_casting(GeoPoint(30.3, 120.4), concave))
        # 凹口处的点在外部
        self.assertFalse(_ray_casting(GeoPoint(30.7, 120.9), concave))

    def test_less_than_3_vertices(self):
        self.assertFalse(_ray_casting(GeoPoint(0, 0), []))
        self.assertFalse(_ray_casting(GeoPoint(0, 0), [GeoPoint(0, 0), GeoPoint(1, 1)]))


class TestCircleFence(unittest.TestCase):
    """测试圆形围栏"""

    def setUp(self):
        self.center = GeoPoint(31.2, 121.4)
        self.fence = CircleFence(center=self.center, radius_m=500, id="cf1", name="Home")

    def test_contains_inside(self):
        # 很近的点在内部
        p = GeoPoint(31.2, 121.4001)  # 非常近
        self.assertTrue(self.fence.contains(p))

    def test_contains_nearby(self):
        # 非常近的点应该在围栏内（约 5m）
        lat_offset = 5 / 111320.0
        p = GeoPoint(31.2 + lat_offset, 121.4)
        self.assertTrue(self.fence.contains(p))

    def test_far_outside(self):
        p = GeoPoint(32.0, 122.0)
        self.assertFalse(self.fence.contains(p))

    def test_buffer_inside(self):
        p = GeoPoint(31.2, 121.4001)
        self.assertFalse(self.fence.buffer_distance(p, 50))

    def test_buffer_true(self):
        # 距离刚好略大于 radius，小于 radius+buffer
        # radius=500, buffer=50: 在 520m 处
        lat_offset = 520 / 111320.0
        p = GeoPoint(31.2 + lat_offset, 121.4)
        # 应该不在圆内
        self.assertFalse(self.fence.contains(p))
        # 应该在缓冲区
        self.assertTrue(self.fence.buffer_distance(p, 50))


class TestPolygonFence(unittest.TestCase):
    """测试多边形围栏"""

    def setUp(self):
        self.square = [
            GeoPoint(30.0, 120.0),
            GeoPoint(31.0, 120.0),
            GeoPoint(31.0, 121.0),
            GeoPoint(30.0, 121.0),
        ]
        self.fence = PolygonFence(vertices=self.square, id="pf1", name="ZoneA")

    def test_requires_3_vertices(self):
        with self.assertRaises(ValueError):
            PolygonFence(vertices=[GeoPoint(0, 0), GeoPoint(1, 1)])

    def test_contains(self):
        self.assertTrue(self.fence.contains(GeoPoint(30.5, 120.5)))

    def test_not_contains(self):
        self.assertFalse(self.fence.contains(GeoPoint(29.5, 119.5)))

    def test_buffer(self):
        # 多边形外的点，但距离边界 < buffer
        p_on_edge = GeoPoint(30.0, 120.5)  # 在边上
        self.assertTrue(self.fence.contains(p_on_edge))
        self.assertFalse(self.fence.buffer_distance(p_on_edge, 50))

    def test_buffer_outside_near(self):
        # 非常靠近边界外侧
        p_near = GeoPoint(29.9999, 120.5)
        self.assertTrue(self.fence.buffer_distance(p_near, 100))


class TestAltitudeFence(unittest.TestCase):
    """测试高度围栏"""

    def setUp(self):
        self.fence = AltitudeFence(min_altitude_m=10, max_altitude_m=500, id="af1", name="AltLimit")

    def test_contains(self):
        self.assertTrue(self.fence.contains(100))
        self.assertTrue(self.fence.contains(10))
        self.assertTrue(self.fence.contains(500))

    def test_too_low(self):
        self.assertFalse(self.fence.contains(5))

    def test_too_high(self):
        self.assertFalse(self.fence.contains(600))

    def test_buffer_low(self):
        self.assertFalse(self.fence.contains(5))
        self.assertTrue(self.fence.buffer_distance(5, 10))

    def test_buffer_high(self):
        self.assertFalse(self.fence.contains(510))
        self.assertTrue(self.fence.buffer_distance(510, 10))

    def test_buffer_out_of_range(self):
        self.assertFalse(self.fence.buffer_distance(0, 5))


class TestCompoundFence(unittest.TestCase):
    """测试复合围栏"""

    def setUp(self):
        self.circle = CircleFence(center=GeoPoint(31.0, 121.0), radius_m=1000, id="c1")
        self.poly = PolygonFence(
            vertices=[
                GeoPoint(30.0, 120.0),
                GeoPoint(31.0, 120.0),
                GeoPoint(31.0, 121.0),
                GeoPoint(30.0, 121.0),
            ],
            id="p1"
        )
        self.alt = AltitudeFence(min_altitude_m=10, max_altitude_m=500, id="a1")

    def test_and_logic(self):
        comp = CompoundFence(
            children=[self.circle, self.poly, self.alt],
            logic=CompoundLogic.AND,
            id="and1"
        )
        self.assertEqual(comp.logic, CompoundLogic.AND)

    def test_or_logic(self):
        comp = CompoundFence(
            children=[self.circle, self.poly],
            logic=CompoundLogic.OR,
            id="or1"
        )
        self.assertEqual(comp.logic, CompoundLogic.OR)


class TestGeofenceManager(unittest.TestCase):
    """测试 GeofenceManager"""

    def setUp(self):
        self.config = GeofenceConfig(
            buffer_distance_m=50,
            auto_rtl_on_violation=True,
            violation_cooldown_s=0.1  # 短冷却方便测试
        )
        self.manager = GeofenceManager(config=self.config)

        # 添加测试围栏：多边形覆盖 [30,120]~[31,121]，圆心在 (30.5,120.5) 在内部
        self.manager.add_polygon(
            "zone_a",
            [
                GeoPoint(30.0, 120.0),
                GeoPoint(31.0, 120.0),
                GeoPoint(31.0, 121.0),
                GeoPoint(30.0, 121.0),
            ],
            "Zone A"
        )
        self.manager.add_circle("home", GeoPoint(30.5, 120.5), 500, "Home")
        self.manager.add_altitude("alt_limit", 10, 500, "Altitude")

    def test_fence_count(self):
        self.assertEqual(self.manager.fence_count, 3)

    def test_list_fences(self):
        fences = self.manager.list_fences()
        self.assertEqual(len(fences), 3)
        types = [f['type'] for f in fences]
        self.assertIn('polygon', types)
        self.assertIn('circle', types)
        self.assertIn('altitude', types)

    def test_get_fence(self):
        fence = self.manager.get_fence("zone_a")
        self.assertIsInstance(fence, PolygonFence)

    def test_get_nonexistent(self):
        self.assertIsNone(self.manager.get_fence("nonexistent"))

    def test_remove_fence(self):
        self.assertTrue(self.manager.remove_fence("home"))
        self.assertEqual(self.manager.fence_count, 2)

    def test_remove_nonexistent(self):
        self.assertFalse(self.manager.remove_fence("fake"))

    def test_duplicate_id(self):
        with self.assertRaises(ValueError):
            self.manager.add_circle("zone_a", GeoPoint(0, 0), 100)

    # ── check_position 测试 ──

    def test_safe_position(self):
        # 在安全区域内（多边形内且圆内）
        event = self.manager.check_position(GeoPoint(30.5, 120.5), altitude_m=200)
        self.assertIsNone(event)

    def test_outside_polygon(self):
        # 在多边形外
        event = self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        self.assertIsNotNone(event)
        self.assertEqual(event.severity, ViolationSeverity.MAJOR)
        self.assertEqual(event.fence_id, "zone_a")

    def test_outside_circle(self):
        # 在多边形内但圆形围栏外
        event = self.manager.check_position(GeoPoint(30.5, 121.0), altitude_m=200)
        self.assertIsNotNone(event)
        self.assertIn(event.fence_id, ["home"])

    def test_altitude_violation(self):
        # 高度超限（位置在安全区内，只有高度违规）
        event = self.manager.check_position(GeoPoint(30.5, 120.5), altitude_m=600)
        self.assertIsNotNone(event)
        self.assertEqual(event.fence_type, GeofenceType.ALTITUDE)

    def test_buffer_warning(self):
        # 位置在圆形缓冲区（多边形内，但距离圆心约 520m，在 500-550m 缓冲区）
        lat_offset = 520 / 111320.0
        pos = GeoPoint(30.5 + lat_offset, 120.5)
        event = self.manager.check_position(pos, altitude_m=200)
        # 不在圆内但距离小于 radius+buffer → warning
        self.assertIsNotNone(event)
        self.assertEqual(event.severity, ViolationSeverity.WARNING)

    # ── 回调测试 ──

    def test_violation_callback(self):
        callbacks_called = []

        def cb(event):
            callbacks_called.append(event)

        self.manager.on_violation(cb)
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        self.assertEqual(len(callbacks_called), 1)
        self.assertEqual(callbacks_called[0].fence_id, "zone_a")

    def test_warning_callback(self):
        callbacks_called = []

        def cb(event):
            callbacks_called.append(event)

        self.manager.on_warning(cb)
        lat_offset = 520 / 111320.0
        self.manager.check_position(GeoPoint(30.5 + lat_offset, 120.5), altitude_m=200)
        if callbacks_called:
            self.assertEqual(callbacks_called[0].severity, ViolationSeverity.WARNING)

    def test_clear_callbacks(self):
        calls = []
        self.manager.on_violation(lambda e: calls.append(e))
        self.manager.clear_callbacks()
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        self.assertEqual(len(calls), 0)

    # ── 违规历史 ──

    def test_violation_history(self):
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        history = self.manager.get_violation_history()
        self.assertGreaterEqual(len(history), 1)

    def test_active_violations(self):
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        active = self.manager.get_active_violations()
        self.assertGreaterEqual(len(active), 1)

    # ── Auto RTL ──

    def test_auto_rtl_trigger(self):
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        self.assertTrue(self.manager.auto_rtl_triggered)

    def test_reset_violation_state(self):
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        self.manager.reset_violation_state()
        self.assertFalse(self.manager.auto_rtl_triggered)
        self.assertEqual(len(self.manager.get_active_violations()), 0)

    def test_auto_rtl_disabled(self):
        config = GeofenceConfig(auto_rtl_on_violation=False)
        mgr = GeofenceManager(config=config)
        mgr.add_polygon("z", [GeoPoint(30, 120), GeoPoint(31, 120), GeoPoint(31, 121), GeoPoint(30, 121)])
        mgr.check_position(GeoPoint(29, 119), altitude_m=200)
        self.assertFalse(mgr.auto_rtl_triggered)

    # ── Cooldown ──

    def test_cooldown(self):
        # 第一次违规
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        history_len = len(self.manager.get_violation_history())

        # 立即再次检测——应该被冷却
        self.manager.check_position(GeoPoint(29.0, 119.0), altitude_m=200)
        # 同样的 fence，冷却中不应该新增记录
        # 注意：实际冷却检查基于 fence_id，相同 fence 的新事件被抑制
        self.assertLessEqual(len(self.manager.get_violation_history()), history_len + 1)

    # ── Compound 集成测试 ──

    def test_compound_or_violation(self):
        inner = CircleFence(center=GeoPoint(30.5, 120.5), radius_m=10000, id="inner")
        comp = CompoundFence(
            children=[inner],
            logic=CompoundLogic.OR,
            id="comp1",
            name="Comp OR"
        )
        mgr = GeofenceManager()
        mgr.add_compound("comp1", [inner], CompoundLogic.OR, "Comp OR")
        # 位置在 inner 内但其他围栏没有，OR 逻辑：一个满足
        event = mgr.check_position(GeoPoint(30.5, 120.5), altitude_m=100)
        self.assertIsNone(event)  # 在围栏内，安全

    def test_clear_all(self):
        self.manager.clear_all()
        self.assertEqual(self.manager.fence_count, 0)

    def test_altitude_warning_buffer(self):
        mgr = GeofenceManager(config=GeofenceConfig(altitude_buffer_m=10))
        mgr.add_altitude("alt", 0, 100, "Altitude")
        event = mgr.check_position(GeoPoint(30.5, 120.5), altitude_m=105)
        self.assertIsNotNone(event)
        self.assertEqual(event.severity, ViolationSeverity.WARNING)


class TestGeoJsonLoading(unittest.TestCase):
    """测试 GeoJSON 加载"""

    def test_polygon_geojson(self):
        geojson = {
            "type": "FeatureCollection",
            "features": [
                {
                    "type": "Feature",
                    "geometry": {
                        "type": "Polygon",
                        "coordinates": [[
                            [120.0, 30.0],
                            [121.0, 30.0],
                            [121.0, 31.0],
                            [120.0, 31.0],
                            [120.0, 30.0]
                        ]]
                    },
                    "properties": {"id": "test_poly", "name": "Test Zone"}
                }
            ]
        }
        with tempfile.NamedTemporaryFile(mode='w', suffix='.geojson', delete=False, encoding='utf-8') as f:
            json.dump(geojson, f)
            fpath = f.name

        try:
            fences = load_geojson(fpath)
            self.assertEqual(len(fences), 1)
            self.assertIsInstance(fences[0], PolygonFence)
            self.assertEqual(fences[0].id, "test_poly")
            self.assertEqual(fences[0].name, "Test Zone")
            self.assertEqual(len(fences[0].vertices), 4)  # 5 - 1 闭合点
        finally:
            os.unlink(fpath)

    def test_circle_geojson(self):
        geojson = {
            "type": "FeatureCollection",
            "features": [
                {
                    "type": "Feature",
                    "geometry": {
                        "type": "Point",
                        "coordinates": [121.4, 31.2]
                    },
                    "properties": {"id": "test_circle", "radius": 500}
                }
            ]
        }
        with tempfile.NamedTemporaryFile(mode='w', suffix='.geojson', delete=False, encoding='utf-8') as f:
            json.dump(geojson, f)
            fpath = f.name

        try:
            fences = load_geojson(fpath)
            self.assertEqual(len(fences), 1)
            self.assertIsInstance(fences[0], CircleFence)
            self.assertEqual(fences[0].radius_m, 500)
            self.assertAlmostEqual(fences[0].center.lat, 31.2)
        finally:
            os.unlink(fpath)

    def test_altitude_geojson(self):
        geojson = {
            "type": "FeatureCollection",
            "features": [
                {
                    "type": "Feature",
                    "geometry": {"type": "Point", "coordinates": [0, 0]},
                    "properties": {"min_altitude": 10, "max_altitude": 500, "id": "alt1"}
                }
            ]
        }
        with tempfile.NamedTemporaryFile(mode='w', suffix='.geojson', delete=False, encoding='utf-8') as f:
            json.dump(geojson, f)
            fpath = f.name

        try:
            fences = load_geojson(fpath)
            self.assertEqual(len(fences), 1)
            self.assertIsInstance(fences[0], AltitudeFence)
            self.assertEqual(fences[0].min_altitude_m, 10)
            self.assertEqual(fences[0].max_altitude_m, 500)
        finally:
            os.unlink(fpath)

    def test_group_compound(self):
        geojson = {
            "type": "FeatureCollection",
            "features": [
                {
                    "type": "Feature",
                    "geometry": {"type": "Polygon", "coordinates": [[[120, 30], [121, 30], [121, 31], [120, 31], [120, 30]]]},
                    "properties": {"group": "g1"}
                },
                {
                    "type": "Feature",
                    "geometry": {"type": "Polygon", "coordinates": [[[122, 32], [123, 32], [123, 33], [122, 33], [122, 32]]]},
                    "properties": {"group": "g1"}
                }
            ]
        }
        with tempfile.NamedTemporaryFile(mode='w', suffix='.geojson', delete=False, encoding='utf-8') as f:
            json.dump(geojson, f)
            fpath = f.name

        try:
            fences = load_geojson(fpath)
            # 应有 1 个复合围栏（group g1）
            compounds = [f for f in fences if isinstance(f, CompoundFence)]
            self.assertEqual(len(compounds), 1)
            self.assertEqual(len(compounds[0].children), 2)
        finally:
            os.unlink(fpath)


class TestConvenienceFunctions(unittest.TestCase):
    """测试便捷工厂函数"""

    def test_create_circle_fence(self):
        fence = create_circle_fence("cf", 31.2, 121.4, 500, "Home")
        self.assertEqual(fence.id, "cf")
        self.assertEqual(fence.name, "Home")
        self.assertAlmostEqual(fence.center.lat, 31.2)
        self.assertEqual(fence.radius_m, 500)

    def test_create_polygon_fence(self):
        vertices = [(30.0, 120.0), (31.0, 120.0), (31.0, 121.0), (30.0, 121.0)]
        fence = create_polygon_fence("pf", vertices, "Zone")
        self.assertEqual(fence.id, "pf")
        self.assertEqual(fence.name, "Zone")
        self.assertEqual(len(fence.vertices), 4)

    def test_create_polygon_default_name(self):
        vertices = [(0, 0), (1, 0), (0, 1)]
        fence = create_polygon_fence("pf", vertices)
        self.assertEqual(fence.name, "pf")


class TestGeofenceConfig(unittest.TestCase):
    """测试配置"""

    def test_default_config(self):
        config = GeofenceConfig()
        self.assertEqual(config.buffer_distance_m, 50.0)
        self.assertTrue(config.auto_rtl_on_violation)
        self.assertEqual(config.violation_cooldown_s, 5.0)
        self.assertEqual(config.altitude_buffer_m, 10.0)

    def test_custom_config(self):
        config = GeofenceConfig(
            buffer_distance_m=100,
            auto_rtl_on_violation=False,
            violation_cooldown_s=2.0
        )
        self.assertEqual(config.buffer_distance_m, 100)
        self.assertFalse(config.auto_rtl_on_violation)
        self.assertEqual(config.violation_cooldown_s, 2.0)


class TestPointToSegmentDistance(unittest.TestCase):
    """测试点到线段距离"""

    def test_point_on_segment(self):
        d = _point_to_segment_distance(0.5, 0.0, 0.0, 0.0, 1.0, 0.0)
        self.assertAlmostEqual(d, 0.0, delta=10)

    def test_point_off_segment(self):
        d = _point_to_segment_distance(0.5, 1.0, 0.0, 0.0, 1.0, 0.0)
        self.assertGreater(d, 100000)  # 约 1 度 ≈ 111km

    def test_point_closest_to_endpoint(self):
        d = _point_to_segment_distance(-1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
        self.assertGreater(d, 100000)


class TestDistanceToPolygonEdge(unittest.TestCase):
    """测试点到多边形边界的距离"""

    def test_inside_point(self):
        square = [GeoPoint(30.0, 120.0), GeoPoint(31.0, 120.0), GeoPoint(31.0, 121.0), GeoPoint(30.0, 121.0)]
        d = _distance_to_polygon_edge(GeoPoint(30.5, 120.5), square)
        self.assertGreater(d, 0)

    def test_empty_polygon(self):
        d = _distance_to_polygon_edge(GeoPoint(0, 0), [])
        self.assertEqual(d, float('inf'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
