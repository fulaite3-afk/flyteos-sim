/**
 * task_panel.js — 任务规划面板
 * ============================================================================
 * 功能：展示和管理飞行任务规划，包括航点列表（序号/经纬度/高度/标签）、
 *       禁飞区列表（类型/状态），以及航线可视化开关。
 *       从 flight_demo_cesium.html 提取 WAYPOINTS/GEOFENCES 数据结构。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   state:task:changed           — 监视 task 状态对象变化
 *   task:waypoint:select         — 选中航点时发布
 *   task:geofence:toggle         — 围栏显示/隐藏
 *   task:route:toggle            — 航线显示/隐藏
 *
 * 暴露接口（挂载到 window.Panels.task）：
 *   init(containerId)      → 在指定容器中创建面板 DOM
 *   refresh(data?)         → 手动刷新面板数据
 *   destroy()              → 移除面板 DOM 及所有订阅
 * ============================================================================
 */

(function (global) {
  'use strict';

  const EventBus = global.EventBus;
  const StateManager = global.StateManager;

  // ── 私有状态 ──────────────────────────────────────────────
  let _container = null;
  let _rootEl = null;
  let _subscriptions = [];
  let _eventUnsubs = [];
  let _styleEl = null;
  const _refs = {};

  // ── 默认任务数据（从 flight_demo_cesium.html 提取）─────────
  const DEFAULT_WAYPOINTS = [
    { id: 1, lat: 37.7700, lng: -122.4100, alt: 50,  label: 'WP1 (Takeoff)' },
    { id: 2, lat: 37.7800, lng: -122.3800, alt: 200, label: 'WP2' },
    { id: 3, lat: 37.8000, lng: -122.4200, alt: 300, label: 'WP3' },
    { id: 4, lat: 37.7600, lng: -122.4500, alt: 250, label: 'WP4' },
    { id: 5, lat: 37.7900, lng: -122.3900, alt: 100, label: 'WP5 (Landing)' },
  ];

  const DEFAULT_GEOFENCES = [
    { id: 'GF01', type: 'Circle',   center: '37.785, -122.430', radius: '800m (buffer 150m)', altitude: '0-400m', status: 'safe' },
    { id: 'GF02', type: 'Circle',   center: '37.775, -122.395', radius: '600m (buffer 100m)', altitude: '0-350m', status: 'safe' },
    { id: 'GF03', type: 'Polygon',  center: '4 vertices',       radius: '—',                  altitude: '0-300m', status: 'safe' },
  ];

  // ── CSS 注入 ──────────────────────────────────────────────
  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'task-panel-styles';
    _styleEl.textContent = `
      .tp-card {
        background: #101830; border: 1px solid #1a2a4a; border-radius: 6px;
        padding: 12px; margin-bottom: 8px;
        font-family: 'Segoe UI', 'Consolas', monospace; color: #c0d0f0;
      }
      .tp-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .tp-section { margin-top:10px; padding-top:8px; border-top:1px solid #182442; }
      .tp-section h4 { font-size:11px; color:#506a8e; margin:0 0 6px 0; letter-spacing:0.5px; }

      /* ── 航点列表 ── */
      .tp-wp-list { font-size:11px; }
      .tp-wp-item { display:flex; align-items:center; padding:4px 6px; margin:2px 0; border-radius:3px;
        background:#0d1428; border:1px solid #182442; cursor:pointer; transition:all 0.2s; }
      .tp-wp-item:hover { background:#141e38; border-color:#2a4a6a; }
      .tp-wp-item.active { background:#1a2a4a; border-color:#4ea8ff; }
      .tp-wp-idx { width:20px; height:20px; border-radius:50%; background:#1a2a4a; color:#6a9fd8;
        text-align:center; line-height:20px; font-size:10px; font-weight:bold; margin-right:8px; flex-shrink:0; }
      .tp-wp-item.active .tp-wp-idx { background:#2a5a9a; color:#4ea8ff; }
      .tp-wp-info { flex:1; min-width:0; }
      .tp-wp-label { color:#c0d8f0; font-weight:bold; display:block; }
      .tp-wp-pos { color:#6a8aae; font-size:10px; }
      .tp-wp-toggle { width:14px; height:14px; border-radius:3px; border:1px solid #2a4a6a; flex-shrink:0; cursor:pointer; }
      .tp-wp-toggle.on { background:#4ef0a0; border-color:#4ef0a0; }

      /* ── 禁飞区列表 ── */
      .tp-gf-list { font-size:11px; }
      .tp-gf-item { display:flex; justify-content:space-between; align-items:center; padding:5px 8px;
        margin:3px 0; border-radius:4px; background:#0d1428; border:1px solid #182442; }
      .tp-gf-item.safe { border-color:#1a3a2a; }
      .tp-gf-item.warn { border-color:#f0a04e; background:rgba(240,160,78,0.08); }
      .tp-gf-item.violation { border-color:#f04e4e; background:rgba(240,78,78,0.10); }
      .tp-gf-name { font-weight:bold; }
      .tp-gf-meta { font-size:10px; color:#6a8aae; display:block; }
      .tp-gf-badge { font-size:10px; padding:1px 6px; border-radius:8px; font-weight:bold; text-transform:uppercase; }
      .tp-gf-badge.safe { color:#4ef0a0; background:rgba(78,240,160,0.12); }
      .tp-gf-badge.warn { color:#f0a04e; background:rgba(240,160,78,0.15); }
      .tp-gf-badge.violation { color:#f04e4e; background:rgba(240,78,78,0.15); }

      /* ── 航线可视化控制 ── */
      .tp-route-row { display:flex; justify-content:space-between; align-items:center; padding:6px 0; font-size:11px; }
      .tp-switch { position:relative; display:inline-block; width:36px; height:20px; }
      .tp-switch input { opacity:0; width:0; height:0; }
      .tp-slider { position:absolute; cursor:pointer; top:0;left:0;right:0;bottom:0; background:#1a2a4a; border-radius:10px; transition:0.3s; }
      .tp-slider:before { content:''; position:absolute; height:14px;width:14px; left:3px;bottom:3px; background:#6a9fd8; border-radius:50%; transition:0.3s; }
      .tp-switch input:checked + .tp-slider { background:#2a5a8a; }
      .tp-switch input:checked + .tp-slider:before { transform:translateX(16px); background:#4ef0a0; }
    `;
    document.head.appendChild(_styleEl);
  }

  // ── DOM 构建 ──────────────────────────────────────────────
  function _buildRoot() {
    const root = document.createElement('div');
    root.className = 'tp-card';
    root.id = 'task-panel-root';

    // 航点列表区域
    const wpSection = document.createElement('div');
    wpSection.className = 'tp-section';
    wpSection.innerHTML = '<h4>Waypoints (' + DEFAULT_WAYPOINTS.length + ')</h4>';
    const wpList = document.createElement('div');
    wpList.className = 'tp-wp-list';
    wpList.id = 'tp-waypoint-list';
    wpSection.appendChild(wpList);
    _refs.wpList = wpList;

    // 禁飞区区域
    const gfSection = document.createElement('div');
    gfSection.className = 'tp-section';
    gfSection.innerHTML = '<h4>No-Fly Zones (' + DEFAULT_GEOFENCES.length + ')</h4>';
    const gfList = document.createElement('div');
    gfList.className = 'tp-gf-list';
    gfList.id = 'tp-geofence-list';
    gfSection.appendChild(gfList);
    _refs.gfList = gfList;

    // 航线可视化控制
    const vizSection = document.createElement('div');
    vizSection.className = 'tp-section';
    vizSection.innerHTML =
      '<h4>Route Visualization</h4>' +
      '<div class="tp-route-row"><span>Show Route Path</span>' +
        '<label class="tp-switch"><input type="checkbox" id="tp-route-toggle" checked><span class="tp-slider"></span></label></div>' +
      '<div class="tp-route-row"><span>Show Waypoints</span>' +
        '<label class="tp-switch"><input type="checkbox" id="tp-wpvis-toggle" checked><span class="tp-slider"></span></label></div>' +
      '<div class="tp-route-row"><span>Show Geofence Walls</span>' +
        '<label class="tp-switch"><input type="checkbox" id="tp-gfvis-toggle" checked><span class="tp-slider"></span></label></div>';

    root.innerHTML = '<h3>TASK PLANNING</h3>';
    root.appendChild(wpSection);
    root.appendChild(gfSection);
    root.appendChild(vizSection);

    _refs.routeToggle = root.querySelector('#tp-route-toggle');
    _refs.wpVisToggle  = root.querySelector('#tp-wpvis-toggle');
    _refs.gfVisToggle  = root.querySelector('#tp-gfvis-toggle');

    return root;
  }

  // ── 渲染航点列表 ──────────────────────────────────────────
  function _renderWaypoints(waypoints) {
    const wps = waypoints || DEFAULT_WAYPOINTS;
    if (!_refs.wpList) return;
    _refs.wpList.innerHTML = '';
    wps.forEach(function (wp, i) {
      const item = document.createElement('div');
      item.className = 'tp-wp-item';
      item.id = 'tp-wp-' + wp.id;
      item.innerHTML =
        '<span class="tp-wp-idx">' + wp.id + '</span>' +
        '<span class="tp-wp-info">' +
          '<span class="tp-wp-label">' + wp.label + '</span>' +
          '<span class="tp-wp-pos">' + wp.lat.toFixed(4) + ', ' + wp.lng.toFixed(4) + ' @ ' + wp.alt + 'm</span>' +
        '</span>' +
        '<span class="tp-wp-toggle on"></span>';
      item.addEventListener('click', function () {
        _selectWaypoint(wp.id, wps);
      });
      _refs.wpList.appendChild(item);
    });

    // 航线摘要
    const summary = document.createElement('div');
    summary.style.cssText = 'font-size:10px;color:#506a8e;margin-top:6px;text-align:right;';
    summary.textContent = 'Total: ' + wps.length + ' waypoints';
    _refs.wpList.appendChild(summary);
  }

  function _selectWaypoint(wpId, waypoints) {
    var items = _refs.wpList.querySelectorAll('.tp-wp-item');
    items.forEach(function (el) { el.classList.remove('active'); });
    var target = _refs.wpList.querySelector('#tp-wp-' + wpId);
    if (target) target.classList.add('active');

    if (EventBus) {
      var wp = (waypoints || DEFAULT_WAYPOINTS).find(function (w) { return w.id === wpId; });
      EventBus.emit('task:waypoint:select', wp || null);
    }
  }

  // ── 渲染禁飞区列表 ────────────────────────────────────────
  function _renderGeofences(geofences) {
    var gfs = geofences || DEFAULT_GEOFENCES;
    if (!_refs.gfList) return;
    _refs.gfList.innerHTML = '';
    gfs.forEach(function (gf) {
      var item = document.createElement('div');
      item.className = 'tp-gf-item ' + (gf.status || 'safe');
      item.innerHTML =
        '<span>' +
          '<span class="tp-gf-name">' + gf.id + '</span>' +
          '<span class="tp-gf-meta">' + gf.type + ' | ' + gf.center + ' | ' + gf.altitude + '</span>' +
        '</span>' +
        '<span class="tp-gf-badge ' + (gf.status || 'safe') + '">' + (gf.status || 'SAFE').toUpperCase() + '</span>';
      _refs.gfList.appendChild(item);
    });
  }

  // ── 数据刷新 ──────────────────────────────────────────────
  function refresh(data) {
    var t = data || (StateManager ? StateManager.get('task') : null) || {};
    if (t.waypoints) _renderWaypoints(t.waypoints);
    if (t.geofences) _renderGeofences(t.geofences);
    if (t.routeVisible !== undefined && _refs.routeToggle) {
      _refs.routeToggle.checked = t.routeVisible;
    }
    if (t.wpVisible !== undefined && _refs.wpVisToggle) {
      _refs.wpVisToggle.checked = t.wpVisible;
    }
    if (t.gfVisible !== undefined && _refs.gfVisToggle) {
      _refs.gfVisToggle.checked = t.gfVisible;
    }
  }

  // ── 事件绑定 ──────────────────────────────────────────────
  function _bindEvents() {
    // 路由可视化开关
    if (_refs.routeToggle) {
      _refs.routeToggle.addEventListener('change', function () {
        if (EventBus) EventBus.emit('task:route:toggle', this.checked);
        if (StateManager) StateManager.merge({ task: Object.assign(StateManager.get('task') || {}, { routeVisible: this.checked }) });
      });
    }
    if (_refs.wpVisToggle) {
      _refs.wpVisToggle.addEventListener('change', function () {
        if (EventBus) EventBus.emit('task:waypoints:toggle', this.checked);
        if (StateManager) StateManager.merge({ task: Object.assign(StateManager.get('task') || {}, { wpVisible: this.checked }) });
      });
    }
    if (_refs.gfVisToggle) {
      _refs.gfVisToggle.addEventListener('change', function () {
        if (EventBus) EventBus.emit('task:geofence:toggle', this.checked);
        if (StateManager) StateManager.merge({ task: Object.assign(StateManager.get('task') || {}, { gfVisible: this.checked }) });
      });
    }

    // 订阅状态变化
    if (StateManager) {
      var unsub = StateManager.subscribe('task', function (newVal) { refresh(newVal); });
      _subscriptions.push(unsub);
      var cur = StateManager.get('task');
      if (cur) refresh(cur);
    }
  }

  // ── 公开接口 ──────────────────────────────────────────────
  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[TaskPanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _renderWaypoints();
    _renderGeofences();
    _bindEvents();
    console.log('[TaskPanel] Initialized in', containerId);
  }

  function destroy() {
    _subscriptions.forEach(function (fn) { if (typeof fn === 'function') fn(); });
    _subscriptions = [];
    _eventUnsubs.forEach(function (fn) { if (typeof fn === 'function') fn(); });
    _eventUnsubs = [];
    if (_rootEl && _rootEl.parentNode) _rootEl.parentNode.removeChild(_rootEl);
    _rootEl = null;
    if (_styleEl && _styleEl.parentNode) _styleEl.parentNode.removeChild(_styleEl);
    _styleEl = null;
    console.log('[TaskPanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.task = { init: init, refresh: refresh, destroy: destroy };
})(window);
