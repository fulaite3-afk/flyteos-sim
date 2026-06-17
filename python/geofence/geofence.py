"""
FlyteOS v2.0 - Geofence Module (geofence.py)

地理围栏模块：支持多边形、圆形、高度围栏及复合围栏组合，
使用射线法（Ray Casting Algorithm）进行实时位置违规检测，
支持缓冲区预警、违规回调、GeoJSON 加载。

Author: FlyteOS Team
Version: 2.0.0
"""

import math
import json
import logging
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import List, Optional, Callable, Dict, Any, Tuple, Union, Set

logger = logging.getLogger(__name__)


# ═══════════════════════════════════════════════════════════════════
#  枚举定义
# ═══════════════════════════════════════════════════════════════════

class GeofenceType(Enum):
    """围栏类型枚举"""
    POLYGON = auto()      # 多边形围栏
    CIRCLE = auto()       # 圆形围栏
    ALTITUDE = auto()     # 高度围栏
    COMPOUND = auto()     # 复合围栏


class ViolationSeverity(Enum):
    """违规严重程度"""
    WARNING = "warning"       # 预警（缓冲区）
    MINOR = "minor"           # 轻微违规
    MAJOR = "major"           # 严重违规
    CRITICAL = "critical"     # 致命违规


class CompoundLogic(Enum):
    """复合围栏逻辑运算符"""
    AND = "and"    # 所有子围栏同时满足
    OR = "or"      # 任一子围栏满足


# ═══════════════════════════════════════════════════════════════════
#  数据类
# ═══════════════════════════════════════════════════════════════════

@dataclass
class GeoPoint:
    """地理坐标点"""
    lat: float  # 纬度（度）
    lon: float  # 经度（度）

    def __repr__(self) -> str:
        return f"GeoPoint(lat={self.lat:.6f}, lon={self.lon:.6f})"

    def to_tuple(self) -> Tuple[float, float]:
        return (self.lat, self.lon)


@dataclass
class CircleFence:
    """圆形围栏定义"""
    center: GeoPoint
    radius_m: float       # 半径（米）
    id: str = ""
    name: str = ""

    def contains(self, point: GeoPoint) -> bool:
        """判断点是否在圆形围栏内"""
        dist = _haversine_distance(self.center, point)
        return dist <= self.radius_m

    def buffer_distance(self, point: GeoPoint, buffer_m: float) -> bool:
        """判断点是否在缓冲区范围内（radius_m + buffer_m）"""
        dist = _haversine_distance(self.center, point)
        return self.radius_m < dist <= self.radius_m + buffer_m


@dataclass
class PolygonFence:
    """多边形围栏定义"""
    vertices: List[GeoPoint]  # 顶点列表（按顺序，首尾不必闭合）
    id: str = ""
    name: str = ""

    def __post_init__(self):
        if len(self.vertices) < 3:
            raise ValueError(f"PolygonFence requires at least 3 vertices, got {len(self.vertices)}")

    def contains(self, point: GeoPoint) -> bool:
        """使用射线法（Ray Casting）判断点是否在多边形内"""
        return _ray_casting(point, self.vertices)

    def buffer_distance(self, point: GeoPoint, buffer_m: float) -> bool:
        """判断点是否在缓冲区范围内——即不在多边形内但距离边界 < buffer_m"""
        if self.contains(point):
            return False
        return _distance_to_polygon_edge(point, self.vertices) <= buffer_m


@dataclass
class AltitudeFence:
    """高度围栏定义"""
    min_altitude_m: float   # 最小高度（米），负值表示无下限
    max_altitude_m: float   # 最大高度（米）

    id: str = ""
    name: str = ""

    def contains(self, altitude_m: float) -> bool:
        """判断高度是否在范围内"""
        return self.min_altitude_m <= altitude_m <= self.max_altitude_m

    def buffer_distance(self, altitude_m: float, buffer_m: float) -> bool:
        """判断高度是否在缓冲区范围内"""
        if self.contains(altitude_m):
            return False
        return (
            (altitude_m < self.min_altitude_m and altitude_m >= self.min_altitude_m - buffer_m)
            or (altitude_m > self.max_altitude_m and altitude_m <= self.max_altitude_m + buffer_m)
        )


@dataclass
class CompoundFence:
    """复合围栏：多个子围栏的组合（AND/OR 逻辑）"""
    children: List[Any]   # 子围栏列表（CircleFence | PolygonFence | AltitudeFence | CompoundFence）
    logic: CompoundLogic = CompoundLogic.AND
    id: str = ""
    name: str = ""


@dataclass
class ViolationEvent:
    """违规事件"""
    fence_id: str
    fence_name: str
    fence_type: GeofenceType
    severity: ViolationSeverity
    position: Optional[GeoPoint] = None
    altitude_m: Optional[float] = None
    timestamp: float = 0.0
    message: str = ""


@dataclass
class GeofenceConfig:
    """地理围栏管理器配置"""
    buffer_distance_m: float = 50.0       # 默认缓冲区距离（米）
    auto_rtl_on_violation: bool = True    # 违规时是否自动返航
    violation_cooldown_s: float = 5.0     # 违规冷却时间（秒），避免重复报警
    altitude_buffer_m: float = 10.0       # 高度缓冲区（米）


# ═══════════════════════════════════════════════════════════════════
#  几何工具函数
# ═══════════════════════════════════════════════════════════════════

def _haversine_distance(p1: GeoPoint, p2: GeoPoint) -> float:
    """
    Haversine 公式计算两点间大圆距离（米）。

    Args:
        p1: 第一个点
        p2: 第二个点

    Returns:
        距离（米）
    """
    R = 6371000.0  # 地球半径（米）

    lat1 = math.radians(p1.lat)
    lat2 = math.radians(p2.lat)
    dlat = math.radians(p2.lat - p1.lat)
    dlon = math.radians(p2.lon - p1.lon)

    a = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

    return R * c


def _ray_casting(point: GeoPoint, vertices: List[GeoPoint]) -> bool:
    """
    射线法判断点是否在多边形内。

    从点向右发射一条水平射线，统计与多边形边的交点数量。
    奇数个交点在内部，偶数个在外部。

    Args:
        point: 待检测点
        vertices: 多边形顶点列表

    Returns:
        True 如果点在多边形内
    """
    n = len(vertices)
    if n < 3:
        return False

    inside = False
    px, py = point.lon, point.lat

    j = n - 1
    for i in range(n):
        vi = vertices[i]
        vj = vertices[j]

        # 检查射线是否与边 (vj → vi) 相交
        cond_a = (vi.lat > py) != (vj.lat > py)
        if cond_a:
            # 计算交点 x 坐标
            intersect_x = (vj.lon - vi.lon) * (py - vi.lat) / (vj.lat - vi.lat) + vi.lon
            if px < intersect_x:
                inside = not inside
        j = i

    return inside


def _point_to_segment_distance(
    px: float, py: float,
    x1: float, y1: float,
    x2: float, y2: float
) -> float:
    """
    计算点到线段的最短距离（单位：度，近似）。

    使用向量投影法计算垂足，若垂足落在线段外则取端点的较短距离。
    """
    dx = x2 - x1
    dy = y2 - y1

    if dx == 0 and dy == 0:
        return math.sqrt((px - x1) ** 2 + (py - y1) ** 2)

    # 投影参数 t
    t = max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))

    # 投影点
    proj_x = x1 + t * dx
    proj_y = y1 + t * dy

    # 经纬度差值转近似距离：1 度纬度 ≈ 111320 米，1 度经度 ≈ 111320 * cos(lat)
    lat_mid = math.radians((py + proj_y) / 2)
    m_per_deg_lat = 111320.0
    m_per_deg_lon = 111320.0 * math.cos(lat_mid)

    dlat_m = (py - proj_y) * m_per_deg_lat
    dlon_m = (px - proj_x) * m_per_deg_lon

    return math.sqrt(dlat_m ** 2 + dlon_m ** 2)


def _distance_to_polygon_edge(point: GeoPoint, vertices: List[GeoPoint]) -> float:
    """
    计算点到多边形边界的最近距离（米）。

    Args:
        point: 待检测点
        vertices: 多边形顶点列表

    Returns:
        最近距离（米）
    """
    n = len(vertices)
    if n < 3:
        return float('inf')

    min_dist = float('inf')
    j = n - 1
    for i in range(n):
        d = _point_to_segment_distance(
            point.lon, point.lat,
            vertices[j].lon, vertices[j].lat,
            vertices[i].lon, vertices[i].lat
        )
        if d < min_dist:
            min_dist = d
        j = i

    return min_dist


# ═══════════════════════════════════════════════════════════════════
#  GeoJSON 加载器
# ═══════════════════════════════════════════════════════════════════

def load_geojson(filepath: str) -> List[Union[CircleFence, PolygonFence, AltitudeFence, CompoundFence]]:
    """
    从 GeoJSON 文件加载围栏定义。

    支持的 GeoJSON 几何类型：
    - Polygon → PolygonFence
    - MultiPolygon → 多个 PolygonFence
    - Point + radius 属性 → CircleFence

    Feature 属性中的可选字段：
    - id, name: 围栏标识
    - min_altitude, max_altitude: 高度围栏
    - logic: 复合围栏逻辑（"and"/"or"）
    - group: 复合围栏分组标识
    - radius: 圆形围栏半径（米）

    Args:
        filepath: GeoJSON 文件路径

    Returns:
        围栏对象列表
    """
    with open(filepath, 'r', encoding='utf-8') as f:
        data = json.load(f)

    fences: List[Union[CircleFence, PolygonFence, AltitudeFence, CompoundFence]] = []

    # 支持 FeatureCollection 和单个 Feature/Geometry
    features: List[dict] = []
    if data.get('type') == 'FeatureCollection':
        features = data.get('features', [])
    elif data.get('type') == 'Feature':
        features = [data]
    elif data.get('type') in ('Polygon', 'MultiPolygon', 'Point'):
        features = [{'type': 'Feature', 'geometry': data, 'properties': {}}]
    else:
        raise ValueError(f"Unsupported GeoJSON type: {data.get('type')}")

    # 按 group 收集用于复合围栏
    groups: Dict[str, List[Union[CircleFence, PolygonFence, AltitudeFence]]] = {}

    for feature in features:
        geometry = feature.get('geometry', {})
        properties = feature.get('properties', {})

        geom_type = geometry.get('type', '')
        fence_id = properties.get('id', '')
        fence_name = properties.get('name', '')
        group = properties.get('group', '')

        fences_from_feature = _parse_geometry(
            geom_type, geometry, properties, fence_id, fence_name
        )

        if group:
            groups.setdefault(group, []).extend(fences_from_feature)
        else:
            fences.extend(fences_from_feature)

    # 处理分组复合围栏
    for group_name, group_fences in groups.items():
        # 查找分组属性——在 features 中搜索
        logic = CompoundLogic.AND
        for feature in features:
            props = feature.get('properties', {})
            if props.get('group') == group_name:
                logic_str = props.get('logic', 'and').lower()
                logic = CompoundLogic.AND if logic_str == 'and' else CompoundLogic.OR
                break

        compound = CompoundFence(
            children=group_fences,
            logic=logic,
            id=f"compound_{group_name}",
            name=f"Compound: {group_name}"
        )
        fences.append(compound)

    logger.info(f"Loaded {len(fences)} geofence(s) from {filepath}")
    return fences


def _parse_geometry(
    geom_type: str,
    geometry: dict,
    properties: dict,
    fence_id: str,
    fence_name: str
) -> List[Union[CircleFence, PolygonFence, AltitudeFence]]:
    """解析单个 Geometry 对象为围栏列表"""
    fences: List[Union[CircleFence, PolygonFence, AltitudeFence]] = []

    # 高度围栏
    min_alt = properties.get('min_altitude')
    max_alt = properties.get('max_altitude')
    if min_alt is not None or max_alt is not None:
        fences.append(AltitudeFence(
            min_altitude_m=min_alt if min_alt is not None else float('-inf'),
            max_altitude_m=max_alt if max_alt is not None else float('inf'),
            id=fence_id or f"alt_{len(fences)}",
            name=fence_name or f"Altitude Fence {len(fences)}"
        ))
        return fences

    if geom_type == 'Polygon':
        coords = geometry.get('coordinates', [])
        for ring in coords:
            vertices = [GeoPoint(lat=p[1], lon=p[0]) for p in ring]
            # 去掉闭合点（如果首尾相同）
            if len(vertices) > 1 and vertices[0].lat == vertices[-1].lat and vertices[0].lon == vertices[-1].lon:
                vertices = vertices[:-1]
            if len(vertices) >= 3:
                fences.append(PolygonFence(
                    vertices=vertices,
                    id=fence_id or f"poly_{len(fences)}",
                    name=fence_name or f"Polygon Fence {len(fences)}"
                ))

    elif geom_type == 'MultiPolygon':
        all_coords = geometry.get('coordinates', [])
        for poly_coords in all_coords:
            for ring in poly_coords:
                vertices = [GeoPoint(lat=p[1], lon=p[0]) for p in ring]
                if len(vertices) > 1 and vertices[0].lat == vertices[-1].lat and vertices[0].lon == vertices[-1].lon:
                    vertices = vertices[:-1]
                if len(vertices) >= 3:
                    fences.append(PolygonFence(
                        vertices=vertices,
                        id=fence_id or f"poly_{len(fences)}",
                        name=fence_name or f"Polygon Fence {len(fences)}"
                    ))

    elif geom_type == 'Point':
        coords = geometry.get('coordinates', [0, 0])
        radius = properties.get('radius', 100.0)
        fences.append(CircleFence(
            center=GeoPoint(lat=coords[1], lon=coords[0]),
            radius_m=radius,
            id=fence_id or f"circle_{len(fences)}",
            name=fence_name or f"Circle Fence {len(fences)}"
        ))

    return fences


# ═══════════════════════════════════════════════════════════════════
#  GeofenceManager
# ═══════════════════════════════════════════════════════════════════

# 类型别名
FenceType = Union[CircleFence, PolygonFence, AltitudeFence, CompoundFence]
ViolationCallback = Callable[[ViolationEvent], None]
WarningCallback = Callable[[ViolationEvent], None]


class GeofenceManager:
    """
    地理围栏管理器。

    负责所有围栏的注册、实时位置检测、违规判定与响应。

    用法:
        manager = GeofenceManager(config)

        # 添加围栏
        manager.add_polygon("zone_a", vertices, ...)
        manager.add_circle("home", center, radius_m, ...)

        # 实时检测
        event = manager.check_position(GeoPoint(31.2, 121.4), altitude_m=100)
        if event:
            print(f"Violation: {event.message}")
    """

    def __init__(self, config: Optional[GeofenceConfig] = None):
        """
        初始化围栏管理器。

        Args:
            config: 配置参数，None 则使用默认值
        """
        self.config = config or GeofenceConfig()

        # 围栏存储：按类型分桶
        self._polygon_fences: Dict[str, PolygonFence] = {}
        self._circle_fences: Dict[str, CircleFence] = {}
        self._altitude_fences: Dict[str, AltitudeFence] = {}
        self._compound_fences: Dict[str, CompoundFence] = {}

        # 所有围栏的统一 ID 索引
        self._all_fences: Dict[str, FenceType] = {}

        # 回调
        self._violation_callbacks: List[ViolationCallback] = []
        self._warning_callbacks: List[WarningCallback] = []

        # 违规记录
        self._violation_history: List[ViolationEvent] = []
        self._active_violations: Dict[str, ViolationEvent] = {}
        self._last_violation_time: Dict[str, float] = {}

        # 响应动作
        self._auto_rtl_triggered: bool = False
        self._auto_hover_triggered: bool = False

    # ── 围栏注册 ──

    def add_polygon(
        self,
        fence_id: str,
        vertices: List[GeoPoint],
        name: str = ""
    ) -> PolygonFence:
        """添加多边形围栏"""
        if fence_id in self._all_fences:
            raise ValueError(f"Fence ID '{fence_id}' already exists")
        fence = PolygonFence(vertices=vertices, id=fence_id, name=name or fence_id)
        self._polygon_fences[fence_id] = fence
        self._all_fences[fence_id] = fence
        logger.info(f"Added polygon fence '{fence_id}' with {len(vertices)} vertices")
        return fence

    def add_circle(
        self,
        fence_id: str,
        center: GeoPoint,
        radius_m: float,
        name: str = ""
    ) -> CircleFence:
        """添加圆形围栏"""
        if fence_id in self._all_fences:
            raise ValueError(f"Fence ID '{fence_id}' already exists")
        fence = CircleFence(center=center, radius_m=radius_m, id=fence_id, name=name or fence_id)
        self._circle_fences[fence_id] = fence
        self._all_fences[fence_id] = fence
        logger.info(f"Added circle fence '{fence_id}' radius={radius_m}m")
        return fence

    def add_altitude(
        self,
        fence_id: str,
        min_altitude_m: float,
        max_altitude_m: float,
        name: str = ""
    ) -> AltitudeFence:
        """添加高度围栏"""
        if fence_id in self._all_fences:
            raise ValueError(f"Fence ID '{fence_id}' already exists")
        fence = AltitudeFence(
            min_altitude_m=min_altitude_m,
            max_altitude_m=max_altitude_m,
            id=fence_id,
            name=name or fence_id
        )
        self._altitude_fences[fence_id] = fence
        self._all_fences[fence_id] = fence
        logger.info(f"Added altitude fence '{fence_id}' [{min_altitude_m}, {max_altitude_m}]m")
        return fence

    def add_compound(
        self,
        fence_id: str,
        children: List[FenceType],
        logic: CompoundLogic = CompoundLogic.AND,
        name: str = ""
    ) -> CompoundFence:
        """添加复合围栏"""
        if fence_id in self._all_fences:
            raise ValueError(f"Fence ID '{fence_id}' already exists")
        fence = CompoundFence(children=children, logic=logic, id=fence_id, name=name or fence_id)
        self._compound_fences[fence_id] = fence
        self._all_fences[fence_id] = fence
        logger.info(f"Added compound fence '{fence_id}' logic={logic.value} with {len(children)} children")
        return fence

    def remove_fence(self, fence_id: str) -> bool:
        """移除围栏"""
        if fence_id not in self._all_fences:
            return False
        self._polygon_fences.pop(fence_id, None)
        self._circle_fences.pop(fence_id, None)
        self._altitude_fences.pop(fence_id, None)
        self._compound_fences.pop(fence_id, None)
        self._all_fences.pop(fence_id, None)
        self._active_violations.pop(fence_id, None)
        self._last_violation_time.pop(fence_id, None)
        logger.info(f"Removed fence '{fence_id}'")
        return True

    def clear_all(self):
        """清除所有围栏"""
        self._polygon_fences.clear()
        self._circle_fences.clear()
        self._altitude_fences.clear()
        self._compound_fences.clear()
        self._all_fences.clear()
        self._active_violations.clear()
        self._last_violation_time.clear()
        self._violation_history.clear()
        self._auto_rtl_triggered = False
        self._auto_hover_triggered = False
        logger.info("Cleared all geofences")

    def load_from_geojson(self, filepath: str):
        """从 GeoJSON 文件批量加载围栏"""
        fences = load_geojson(filepath)
        for fence in fences:
            if fence.id and fence.id not in self._all_fences:
                if isinstance(fence, PolygonFence):
                    self._polygon_fences[fence.id] = fence
                elif isinstance(fence, CircleFence):
                    self._circle_fences[fence.id] = fence
                elif isinstance(fence, AltitudeFence):
                    self._altitude_fences[fence.id] = fence
                elif isinstance(fence, CompoundFence):
                    self._compound_fences[fence.id] = fence
                self._all_fences[fence.id] = fence
        logger.info(f"Loaded {len(fences)} fences from GeoJSON: {filepath}")

    # ── 回调注册 ──

    def on_violation(self, callback: ViolationCallback):
        """注册违规回调"""
        self._violation_callbacks.append(callback)

    def on_warning(self, callback: WarningCallback):
        """注册预警回调"""
        self._warning_callbacks.append(callback)

    def clear_callbacks(self):
        """清除所有回调"""
        self._violation_callbacks.clear()
        self._warning_callbacks.clear()

    # ── 查询 ──

    def get_fence(self, fence_id: str) -> Optional[FenceType]:
        """获取指定围栏"""
        return self._all_fences.get(fence_id)

    def list_fences(self) -> List[Dict[str, Any]]:
        """列出所有围栏摘要"""
        result = []
        for fid, fence in self._all_fences.items():
            info = {'id': fid, 'name': getattr(fence, 'name', '')}
            if isinstance(fence, PolygonFence):
                info['type'] = 'polygon'
                info['vertices'] = len(fence.vertices)
            elif isinstance(fence, CircleFence):
                info['type'] = 'circle'
                info['radius_m'] = fence.radius_m
            elif isinstance(fence, AltitudeFence):
                info['type'] = 'altitude'
                info['min_m'] = fence.min_altitude_m
                info['max_m'] = fence.max_altitude_m
            elif isinstance(fence, CompoundFence):
                info['type'] = 'compound'
                info['logic'] = fence.logic.value
                info['children'] = len(fence.children)
            result.append(info)
        return result

    def get_violation_history(self) -> List[ViolationEvent]:
        """获取违规历史记录"""
        return list(self._violation_history)

    def get_active_violations(self) -> List[ViolationEvent]:
        """获取当前活跃的违规事件"""
        return list(self._active_violations.values())

    @property
    def auto_rtl_triggered(self) -> bool:
        """是否已触发自动返航"""
        return self._auto_rtl_triggered

    @property
    def auto_hover_triggered(self) -> bool:
        """是否已触发自动悬停"""
        return self._auto_hover_triggered

    @property
    def fence_count(self) -> int:
        """围栏总数"""
        return len(self._all_fences)

    # ── 核心检测逻辑 ──

    def check_position(
        self,
        position: GeoPoint,
        altitude_m: float,
        timestamp: Optional[float] = None
    ) -> Optional[ViolationEvent]:
        """
        检查给定位置是否违反任何围栏规则。

        检测流程：
        1. 多边形围栏：射线法判断点是否在内
        2. 圆形围栏：Haversine 距离判断
        3. 高度围栏：高度范围判断
        4. 复合围栏：AND/OR 逻辑递归判断
        5. 缓冲区预警：接近边界时发出 WARNING

        Args:
            position: 当前位置
            altitude_m: 当前高度（米）
            timestamp: 时间戳（Unix 秒），None 则使用当前时间

        Returns:
            ViolationEvent 如果违规/预警，否则 None
        """
        import time
        now = timestamp or time.time()

        # 收集所有检测结果
        violations: List[ViolationEvent] = []
        warnings: List[ViolationEvent] = []

        # 1. 检测多边形围栏
        for fid, fence in self._polygon_fences.items():
            evt = self._check_polygon(fence, position, now)
            if evt:
                if evt.severity == ViolationSeverity.WARNING:
                    warnings.append(evt)
                else:
                    violations.append(evt)

        # 2. 检测圆形围栏
        for fid, fence in self._circle_fences.items():
            evt = self._check_circle(fence, position, now)
            if evt:
                if evt.severity == ViolationSeverity.WARNING:
                    warnings.append(evt)
                else:
                    violations.append(evt)

        # 3. 检测高度围栏
        for fid, fence in self._altitude_fences.items():
            evt = self._check_altitude(fence, altitude_m, now)
            if evt:
                if evt.severity == ViolationSeverity.WARNING:
                    warnings.append(evt)
                else:
                    violations.append(evt)

        # 4. 检测复合围栏
        for fid, fence in self._compound_fences.items():
            evt = self._check_compound(fence, position, altitude_m, now)
            if evt:
                if evt.severity == ViolationSeverity.WARNING:
                    warnings.append(evt)
                else:
                    violations.append(evt)

        # 优先返回违规（严重程度最高的），否则返回预警
        if violations:
            # 按严重程度排序
            severity_order = {
                ViolationSeverity.CRITICAL: 0,
                ViolationSeverity.MAJOR: 1,
                ViolationSeverity.MINOR: 2,
            }
            violations.sort(key=lambda v: severity_order.get(v.severity, 99))
            event = violations[0]
            self._handle_violation(event)
            return event

        if warnings:
            event = warnings[0]
            self._handle_warning(event)
            return event

        # 无违规也无预警——清除该位置的活跃违规
        # 注意：这里简单处理，复杂场景中应检查具体哪个围栏不再违规
        return None

    def _check_polygon(
        self, fence: PolygonFence, position: GeoPoint, now: float
    ) -> Optional[ViolationEvent]:
        """检查多边形围栏"""
        if fence.contains(position):
            return None  # 在围栏内，安全

        # 检查缓冲区
        if fence.buffer_distance(position, self.config.buffer_distance_m):
            return self._make_event(
                fence, position, None, ViolationSeverity.WARNING, now,
                f"Approaching polygon fence boundary: '{fence.name}'"
            )

        # 违规
        return self._make_event(
            fence, position, None, ViolationSeverity.MAJOR, now,
            f"Outside polygon fence: '{fence.name}'"
        )

    def _check_circle(
        self, fence: CircleFence, position: GeoPoint, now: float
    ) -> Optional[ViolationEvent]:
        """检查圆形围栏"""
        if fence.contains(position):
            return None

        if fence.buffer_distance(position, self.config.buffer_distance_m):
            return self._make_event(
                fence, position, None, ViolationSeverity.WARNING, now,
                f"Approaching circle fence boundary: '{fence.name}'"
            )

        return self._make_event(
            fence, position, None, ViolationSeverity.MAJOR, now,
            f"Outside circle fence: '{fence.name}'"
        )

    def _check_altitude(
        self, fence: AltitudeFence, altitude_m: float, now: float
    ) -> Optional[ViolationEvent]:
        """检查高度围栏"""
        if fence.contains(altitude_m):
            return None

        if fence.buffer_distance(altitude_m, self.config.altitude_buffer_m):
            return self._make_event(
                fence, None, altitude_m, ViolationSeverity.WARNING, now,
                f"Approaching altitude limit: '{fence.name}'"
            )

        severity = ViolationSeverity.CRITICAL if altitude_m > fence.max_altitude_m + 100 else ViolationSeverity.MAJOR
        return self._make_event(
            fence, None, altitude_m, severity, now,
            f"Altitude violation: {altitude_m}m not in [{fence.min_altitude_m}, {fence.max_altitude_m}]m"
        )

    def _check_compound(
        self,
        fence: CompoundFence,
        position: GeoPoint,
        altitude_m: float,
        now: float
    ) -> Optional[ViolationEvent]:
        """检查复合围栏（递归检测子围栏）"""
        child_events: List[ViolationEvent] = []

        for child in fence.children:
            if isinstance(child, PolygonFence):
                if not child.contains(position):
                    child_events.append(self._make_event(
                        child, position, None, ViolationSeverity.MAJOR, now,
                        f"Compound child violated: polygon '{child.name}'"
                    ))
            elif isinstance(child, CircleFence):
                if not child.contains(position):
                    child_events.append(self._make_event(
                        child, position, None, ViolationSeverity.MAJOR, now,
                        f"Compound child violated: circle '{child.name}'"
                    ))
            elif isinstance(child, AltitudeFence):
                if not child.contains(altitude_m):
                    child_events.append(self._make_event(
                        child, None, altitude_m, ViolationSeverity.MAJOR, now,
                        f"Compound child violated: altitude '{child.name}'"
                    ))
            elif isinstance(child, CompoundFence):
                sub_event = self._check_compound(child, position, altitude_m, now)
                if sub_event:
                    child_events.append(sub_event)

        if not child_events:
            return None

        if fence.logic == CompoundLogic.AND:
            # AND: 仅所有子围栏同时违规才算违规
            if len(child_events) == len(fence.children):
                return self._make_event(
                    fence, position, altitude_m, ViolationSeverity.CRITICAL, now,
                    f"Compound fence (AND) violated: '{fence.name}' — all children violated"
                )
            return None
        else:
            # OR: 任一子围栏违规即算违规
            return self._make_event(
                fence, position, altitude_m, ViolationSeverity.MAJOR, now,
                f"Compound fence (OR) violated: '{fence.name}' — {len(child_events)} children violated"
            )

    def _make_event(
        self,
        fence: FenceType,
        position: Optional[GeoPoint],
        altitude_m: Optional[float],
        severity: ViolationSeverity,
        now: float,
        message: str
    ) -> ViolationEvent:
        """构造违规事件"""
        if isinstance(fence, PolygonFence):
            ftype = GeofenceType.POLYGON
        elif isinstance(fence, CircleFence):
            ftype = GeofenceType.CIRCLE
        elif isinstance(fence, AltitudeFence):
            ftype = GeofenceType.ALTITUDE
        else:
            ftype = GeofenceType.COMPOUND

        return ViolationEvent(
            fence_id=fence.id,
            fence_name=fence.name,
            fence_type=ftype,
            severity=severity,
            position=position,
            altitude_m=altitude_m,
            timestamp=now,
            message=message
        )

    def _handle_violation(self, event: ViolationEvent):
        """处理违规事件"""
        fid = event.fence_id

        # 冷却检查
        import time
        now = time.time()
        if fid in self._last_violation_time:
            if now - self._last_violation_time[fid] < self.config.violation_cooldown_s:
                return  # 冷却中，跳过

        self._last_violation_time[fid] = now
        self._active_violations[fid] = event
        self._violation_history.append(event)

        logger.warning(f"VIOLATION: {event.message}")

        # 触发回调
        for cb in self._violation_callbacks:
            try:
                cb(event)
            except Exception as e:
                logger.error(f"Violation callback error: {e}")

        # 自动 RTL
        if self.config.auto_rtl_on_violation and not self._auto_rtl_triggered:
            self._auto_rtl_triggered = True
            logger.warning("AUTO RTL TRIGGERED due to geofence violation")

    def _handle_warning(self, event: ViolationEvent):
        """处理预警事件"""
        logger.info(f"WARNING: {event.message}")

        for cb in self._warning_callbacks:
            try:
                cb(event)
            except Exception as e:
                logger.error(f"Warning callback error: {e}")

    def reset_violation_state(self):
        """重置违规状态（例如 RTL 完成后调用）"""
        self._active_violations.clear()
        self._auto_rtl_triggered = False
        self._auto_hover_triggered = False
        logger.info("Violation state reset")


# ═══════════════════════════════════════════════════════════════════
#  便捷工厂函数
# ═══════════════════════════════════════════════════════════════════

def create_circle_fence(
    fence_id: str,
    lat: float, lon: float, radius_m: float,
    name: str = ""
) -> CircleFence:
    """快速创建圆形围栏"""
    return CircleFence(
        center=GeoPoint(lat=lat, lon=lon),
        radius_m=radius_m,
        id=fence_id,
        name=name or fence_id
    )


def create_polygon_fence(
    fence_id: str,
    vertices_latlon: List[Tuple[float, float]],
    name: str = ""
) -> PolygonFence:
    """
    快速创建多边形围栏。

    Args:
        fence_id: 围栏 ID
        vertices_latlon: 顶点列表 [(lat, lon), ...]
        name: 围栏名称

    Returns:
        PolygonFence
    """
    vertices = [GeoPoint(lat=p[0], lon=p[1]) for p in vertices_latlon]
    return PolygonFence(vertices=vertices, id=fence_id, name=name or fence_id)
