# M1 Phase 3 Integration Report

**日期**: 2026-07-05
**版本**: M1 Phase 3 — 8 Panel Modules
**来源**: `flight_demo_cesium.html` (单体文件) → 模块化面板

---

## 1. 文件清单

### 1.1 核心共享层 (core/)

| 文件 | 大小 | 职责 |
|------|------|------|
| `core/event_bus.js` | 4.8 KB | 发布/订阅事件总线，支持优先级、命名空间、一次性订阅 |
| `core/state_manager.js` | 7.2 KB | 集中式响应状态管理，快照/回滚/订阅 |
| `core/cesium_core.js` | 6.3 KB | Cesium Viewer 封装，相机/实体管理 |

### 1.2 Phase 2 面板 (panels/)

| 文件 | 大小 | 命名空间 | CSS 前缀 | 职责 |
|------|------|----------|----------|------|
| `panels/flight_panel.js` | 8.5 KB | `Panels.flight` | `fp-*` | 飞控面板：GPS/速度/高度/航向/飞行模式/围栏状态 |
| `panels/sensor_panel.js` | 8.8 KB | `Panels.sensor` | `sp-*` | 传感器面板：IMU/Barometer/GPS/Magnetometer 四维进度条+状态灯 |
| `panels/health_panel.js` | 11.4 KB | `Panels.health` | `hp-*` | 健康监控：SVG 环形评分+四维子系统网格+电量倒计时+Canvas 趋势图 |
| `panels/log_panel.js` | 9.6 KB | `Panels.log` | `lp-*` | 操作日志：50 条滚动列表+角色标签+审计统计摘要 |

### 1.3 Phase 3 面板 (panels/) — 本次新增

| 文件 | 大小 | 命名空间 | CSS 前缀 | 职责 |
|------|------|----------|----------|------|
| `panels/task_panel.js` | 14.0 KB | `Panels.task` | `tp-*` | 任务规划：5 个航点列表（可点击选中）+ 3 个禁飞区状态 + 航线可视化开关 |
| `panels/formation_panel.js` | 13.8 KB | `Panels.formation` | `fm-*` | 编队控制：三机状态卡片 + 4 种队形选择器 + 间距滑块（横向/纵向/高度） |
| `panels/replay_panel.js` | 12.7 KB | `Panels.replay` | `rp-*` | 回放控制：时间轴滑块 + 播放/暂停/停止按钮 + 变速（0.5x/1x/2x/4x） |
| `panels/fault_tree_panel.js` | 18.0 KB | `Panels.faultTree` | `ft-*` | 故障树：4 分支 12 叶节点树形结构 + 可折叠 + 统计汇总 + 全展开/折叠/清除 |

### 1.4 入口文件

| 文件 | 大小 | 说明 |
|------|------|------|
| `index.html` | ~15 KB | 模块化入口页面，左右分栏布局，按依赖加载全部模块 |

**总计**: 12 个文件，~132 KB

---

## 2. CSS ID 唯一性检查

### 2.1 Style 标签 ID（每面板独立注入）

| 面板 | Style ID |
|------|----------|
| flight | `flight-panel-styles` |
| sensor | `sensor-panel-styles` |
| health | `health-panel-styles` |
| log | `log-panel-styles` |
| task | `task-panel-styles` |
| formation | `formation-panel-styles` |
| replay | `replay-panel-styles` |
| faultTree | `fault-tree-panel-styles` |

### 2.2 面板根元素 ID

| 面板 | Root ID |
|------|---------|
| flight | `flight-panel-root` |
| sensor | `sensor-panel-root` |
| health | `health-panel-root` |
| log | `log-panel-root` |
| task | `task-panel-root` |
| formation | `formation-panel-root` |
| replay | `replay-panel-root` |
| faultTree | `fault-tree-panel-root` |

### 2.3 内部元素 CSS 类前缀

| 面板 | 前缀 | 示例 |
|------|------|------|
| flight | `fp-` | `.fp-card`, `.fp-val.warn` |
| sensor | `sp-` | `.sp-card`, `.sp-bar-fill` |
| health | `hp-` | `.hp-card`, `.hp-gauge` |
| log | `lp-` | `.lp-card`, `.lp-entry` |
| task | `tp-` | `.tp-card`, `.tp-wp-item` |
| formation | `fm-` | `.fm-card`, `.fm-drone-card` |
| replay | `rp-` | `.rp-card`, `.rp-timeline` |
| faultTree | `ft-` | `.ft-card`, `.ft-node-row` |

**结论**: 全部 8 个面板的 CSS ID / 类前缀 **零冲突**。

---

## 3. 加载顺序

```
Cesium 1.120 CDN (CSS + JS)
  ↓
core/event_bus.js          ← 零依赖
  ↓
core/state_manager.js      ← 依赖 EventBus
  ↓
core/cesium_core.js        ← 依赖 Cesium + EventBus + StateManager
  ↓
panels/flight_panel.js     ← Phase 2
panels/sensor_panel.js     ← Phase 2
panels/health_panel.js     ← Phase 2
panels/log_panel.js        ← Phase 2
  ↓
panels/task_panel.js       ← Phase 3
panels/formation_panel.js  ← Phase 3
panels/replay_panel.js     ← Phase 3
panels/fault_tree_panel.js ← Phase 3
  ↓
index.html 内联启动脚本    ← 初始化 Cesium + 初始化全部 8 面板 + 注入种子数据
```

依赖链正确：所有面板仅依赖 `EventBus` + `StateManager`，面板之间无硬依赖。

---

## 4. StateManager 注册状态键

启动脚本中注入的种子数据覆盖以下 StateManager 键：

| 键 | Phase | 内容 |
|-----|-------|------|
| `flight` | P2 | lat/lng/alt/spd/hdg/vertRate/phase/wpLabel/gfStatus/gpsSats |
| `sensors` | P2 | IMU/BARO/GPS/MAG 传感器值 |
| `health` | P2 | overall/subsystems{4}/batteryRemaining |
| `task` | P3 | routeVisible/wpVisible/gfVisible |
| `formation` | P3 | shape/spacing{lateral/longitudinal/altitude} |
| `replay` | P3 | playing/speed/progress |

---

## 5. EventBus 事件矩阵

### 5.1 Phase 2 事件（已有）

| 事件 | 发布者 | 订阅者 |
|------|--------|--------|
| `demo:start` / `demo:pause` / `demo:resume` / `demo:stop` / `demo:toggle` | index.html 按钮 | 各面板 |
| `state:flight:changed` | StateManager | flight_panel, log_panel |
| `sensor:fault:injected` | sensor_panel | health_panel, log_panel |
| `sensor:fault:cleared` | sensor_panel | health_panel, log_panel |

### 5.2 Phase 3 新增事件

| 事件 | 发布者 | 说明 |
|------|--------|------|
| `task:waypoint:select` | task_panel | 用户点击航点时发布，携带航点对象 |
| `task:route:toggle` | task_panel | 航线路径显示/隐藏 |
| `task:waypoints:toggle` | task_panel | 航点标记显示/隐藏 |
| `task:geofence:toggle` | task_panel | 围栏墙体显示/隐藏 |
| `formation:switch` | formation_panel | 队形切换时发布，携带队形名称 |
| `formation:spacing:changed` | formation_panel | 间距滑块变化时发布，携带 {lateral, longitudinal, altitude} |
| `replay:play` / `replay:pause` / `replay:stop` | replay_panel | 回放播放/暂停/停止控制 |
| `replay:seek` | replay_panel | 时间轴拖动时发布，携带进度百分比 |
| `replay:speed:changed` | replay_panel | 变速变更时发布，携带倍速值 |
| `fault:node:select` | fault_tree_panel | 选中故障节点时发布，携带节点对象 |
| `fault:clear:all` | fault_tree_panel | 清除所有故障状态 |

### 5.3 跨 Phase 事件监听

| 监听方 | 事件 | 用途 |
|--------|------|------|
| replay_panel | `demo:start/pause/resume/stop` | 同步回放状态与演示控制 |
| fault_tree_panel | `sensor:fault:injected` | 传感器故障注入时联动更新故障树节点状态 |

---

## 6. 架构规范一致性

所有 8 个面板均遵循：

- **IIFE 模式**: `(function(global){ 'use strict'; ... })(window);`
- **命名空间**: `window.Panels.<name>`
- **纯 JS DOM**: 零外部 HTML 依赖，动态生成 DOM
- **CSS 动态注入**: 每个面板独立 `<style>` 标签，唯一 ID 避免冲突
- **生命周期**: `init(containerId)` / `refresh(data?)` / `destroy()`
- **状态订阅**: 通过 `StateManager.subscribe` 自动响应状态变化
- **跨面板通信**: 通过 `EventBus.on/emit` 事件机制
- **中文注释**: 文件头完整功能说明 + 接口文档

---

## 7. 已知问题

1. **index.html 内联脚本过长** (~130 行)，后续应由 `app.js` 接管启动逻辑。
2. **task_panel 航点/禁飞区数据硬编码**，与 `flight_demo_cesium.html` 中的 WAYPOINTS/GEOFENCES 保持独立副本，未做实时同步。后续应通过 StateManager 统一管理。
3. **fault_tree_panel 故障注入映射**仅覆盖 IMU/Barometer/GPS/Magnetometer 四种传感器，需随 sensor 扩展同步更新 `_mapSensorToFaultId` 映射表。
4. **replay_panel 总时长固定 60s**，未从 `cesium_core` 或 `StateManager` 动态获取。
5. **面板之间无横向依赖**，虽然灵活但缺少编排层确保初始化顺序（目前依赖 `<script>` 加载顺序天然保证）。
6. **所有面板挂载到同一容器 `panel-scroll`**，若面板数量继续增长可能需要分页/折叠机制。

---

## 8. 下一步建议

1. **创建 `app.js`**: 将 index.html 内联启动脚本抽取为独立 `app.js` 模块，统一编排初始化流程。
2. **Waypoint 数据统一**: 将 task_panel 中的航点/禁飞区数据迁移到 StateManager 的 `mission` 键下，由 cesium_core 和 task_panel 共享同一数据源。
3. **回放引擎集成**: 实现 `replay_panel` 事件与 Cesium 时间线（Cesium.Clock）的对接，支持真实回放。
4. **故障注入闭环**: 完善 fault_tree → sensor 的故障注入闭环，实现从故障树面板直接触发传感器故障。
5. **CSS 主题变量**: 将 `#101830` / `#1a2a4a` / `#4ea8ff` 等公共颜色提取为 CSS 自定义属性，统一主题。
6. **单元测试**: 为 EventBus / StateManager / 各面板生命周期编写测试。
7. **构建优化**: 引入打包工具（ESBuild/Vite）合并 12 个 JS 文件，减少 HTTP 请求。

---

**报告生成**: M1 Phase 3 Integration — File Agent
