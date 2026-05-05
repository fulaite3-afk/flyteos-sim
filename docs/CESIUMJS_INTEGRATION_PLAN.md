# FlyteOS v1.4 CesiumJS 集成方案

**编制人**: 云中鹤  
**日期**: 2026-05-05  
**版本**: v1.0-draft  

---

## 一、技术选型

### 1.1 候选方案对比

| 方案 | 特点 | 许可证 | 适用场景 |
|------|------|--------|---------|
| **CesiumJS** | 全球3D地形+卫星影像+时间动态 | Apache 2.0（需Cesium Ion Token） | 最佳选择：专业级地理可视化 |
| Mapbox GL JS | 2.5D矢量地图+3D建筑 | BSD（商业需付费） | 城市导航，非飞行模拟 |
| Deck.gl | 大数据量地理可视化 | MIT | 数据分析可视化，非地形渲染 |
| Three.js + 自研 | 当前方案，程序化地形 | MIT | 离线可用，但非真实地形 |

### 1.2 选型结论：CesiumJS

**理由：**

1. **真实地形**：全球30m分辨率DEM + 卫星影像，无需自建数据
2. **穿云效果**：CesiumJS 原生支持大气散射和云层渲染
3. **坐标系**：WGS84原点，与GPS坐标直接对应
4. **社区生态**：GitHub 12k+ stars，活跃维护
5. **免费额度**：Cesium Ion 免费版每月50,000次地形请求

**风险点：**
- CesiumJS 体积较大（~4MB gzipped），需考虑加载性能
- Cesium Ion Token 需注册，免费版有请求限制
- 依赖网络加载地形瓦片，离线需额外方案

---

## 二、与现有 Three.js 模拟器的集成方案

### 2.1 方案A：替换（推荐）

完全用 CesiumJS 替换 Three.js 渲染层。

```
当前架构:
  Three.js (渲染) ← 飞行物理 ← HUD ← 键盘控制

新架构:
  CesiumJS (渲染+地形+大气) ← 飞行物理 ← HUD ← 键盘控制
```

**优点：**
- 代码更简洁（CesiumJS 内置地形/大气/云层）
- 真实地形数据自动加载
- 性能更好（CesiumJS 地形LOD优化）

**缺点：**
- 需要重写渲染层
- HUD 需要适配 CesiumJS 的 overlay 模式

### 2.2 方案B：混合

CesiumJS 作为地形底图，Three.js 叠加飞行器模型。

```
  CesiumJS (地形底图) + Three.js (飞行器+特效) → 叠加渲染
```

**优点：**
- 保留现有飞行器渲染代码
- 渐进式迁移

**缺点：**
- 双渲染器性能开销
- 坐标同步复杂（CesiumJS 用 WGS84，Three.js 用笛卡尔）
- 维护成本高

### 2.3 推荐方案：方案A（替换）

理由：FlyteOS v1.3 的 Three.js 程序化地形本来就是过渡方案，CesiumJS 提供的真实地形才是最终目标。一步到位比分步迁移更高效。

---

## 三、真实地形数据加载方案

### 3.1 数据源

| 数据类型 | 来源 | 分辨率 | 费用 |
|---------|------|--------|------|
| 地形DEM | Cesium World Terrain | ~30m | 免费（50K请求/月） |
| 卫星影像 | Bing Maps / Cesium Ion | ~0.5m | 免费（50K请求/月） |
| 建筑物3D | OSM Buildings | LOD1 | 免费 |
| 大气/云层 | CesiumJS 内置 | N/A | 免费 |

### 3.2 模拟区域

以通用模拟坐标为中心，半径 10km 范围：
- 中心：28.0°N, 112.0°E（通用参考点）
- 飞行高度范围：0 - 500m AGL
- 默认巡航高度：200m AGL

### 3.3 离线方案

对于无网络环境，提供两种降级方案：

1. **预缓存地形**：提前下载模拟区域的瓦片包（~200MB）
2. **回退程序化地形**：检测到离线时，自动切换到 v1.3 的 Perlin 噪声地形

```javascript
// 离线检测与降级
async function initTerrain() {
  try {
    const terrain = await Cesium.createWorldTerrainAsync();
    viewer.terrainProvider = terrain;
  } catch (e) {
    console.warn('地形加载失败，回退程序化地形');
    viewer.terrainProvider = new ProceduralTerrainProvider();
  }
}
```

---

## 四、技术实现细节

### 4.1 CesiumJS 初始化

```javascript
const viewer = new Cesium.Viewer('cesiumContainer', {
  terrainProvider: await Cesium.createWorldTerrainAsync(),
  imageryProvider: new Cesium.IonImageryProvider({ assetId: 2 }),
  baseLayerPicker: false,
  geocoder: false,
  homeButton: false,
  sceneModePicker: false,
  navigationHelpButton: false,
  animation: false,
  timeline: false,
  fullscreenButton: false,
  skyAtmosphere: new Cesium.SkyAtmosphere(),
  // 大气效果（穿云）
  scene: {
    fog: { enabled: true, density: 0.0002 },
    skyAtmosphere: { show: true },
  }
});
```

### 4.2 飞行器实体

```javascript
const aircraft = viewer.entities.add({
  position: Cesium.Cartesian3.fromDegrees(112.0, 28.0, 200),
  orientation: new Cesium.VelocityOrientationProperty(),
  model: {
    uri: './models/aerostat.glb', // 浮空器3D模型
    minimumPixelSize: 64,
    maximumScale: 20,
  },
  path: {
    resolution: 1,
    material: new Cesium.PolylineGlowMaterialProperty({
      glowPower: 0.2,
      color: Cesium.Color.CYAN,
    }),
    width: 3,
  }
});
```

### 4.3 穿云效果

CesiumJS 原生大气散射已支持穿云视觉：
- 进入云层时自动产生雾效
- 配合 `scene.fog` 调节密度
- 云层高度带：100m - 300m AGL

### 4.4 HUD 叠加

HUD 使用 HTML overlay 模式（与当前方案相同），叠加在 CesiumJS canvas 上：

```html
<div id="cesiumContainer" style="width:100%;height:100%"></div>
<div id="hud" style="position:fixed;top:0;left:0;width:100%;height:100%;pointer-events:none;z-index:10">
  <!-- 现有HUD面板 -->
</div>
```

---

## 五、性能指标预估

| 指标 | 预估值 | 说明 |
|------|--------|------|
| 首次加载时间 | 3-5s | CesiumJS + 地形瓦片 |
| 后续加载 | <1s | 瓦片缓存命中 |
| 帧率(FPS) | 30-60 | 取决于GPU和地形复杂度 |
| 内存占用 | 200-400MB | 地形瓦片缓存 |
| 网络带宽 | ~5MB/min | 地形瓦片流式加载 |
| JS体积 | ~4MB(gzip) | CesiumJS核心 |

### 5.1 性能优化措施

1. **LOD**：CesiumJS 内置地形LOD，远距离自动降低精度
2. **按需加载**：仅加载当前视野范围内的瓦片
3. **预缓存**：模拟区域瓦片预加载
4. **请求节流**：地形请求走本地代理服务器缓存

---

## 六、迁移计划

| 阶段 | 时间 | 交付物 |
|------|------|--------|
| Phase 1 | 2天 | CesiumJS 基础集成，真实地形显示 |
| Phase 2 | 1天 | 飞行器模型 + 键盘控制迁移 |
| Phase 3 | 1天 | HUD适配 + 穿云效果 |
| Phase 4 | 1天 | 航点导航 + 任务模式 |
| Phase 5 | 1天 | 离线降级 + 性能优化 |

**总计：约6个工作日**

---

## 七、依赖项

| 依赖 | 版本 | 用途 |
|------|------|------|
| CesiumJS | ^1.124 | 3D地形渲染核心 |
| Cesium Ion Token | 免费版 | 地形/影像数据源 |
| Node.js | ^20 | 开发服务器 |
| Vite | ^5 | 构建工具（替代纯HTML） |

---

*云中鹤 · 武汉福莱特航空科技*
