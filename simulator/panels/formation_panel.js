/**
 * formation_panel.js — 编队面板
 * ============================================================================
 * 功能：三机编队状态展示与队形控制。展示 Leader / Wing-1 / Wing-2 的
 *       独立遥测数据，支持队形切换（Line / Wedge / V-Shape / Diamond）
 *       和间距（横向/纵向/高度）滑块控制。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   state:formation:changed      — 监视 formation 状态对象变化
 *   formation:switch             — 队形切换时发布
 *   formation:spacing:changed    — 间距变化时发布
 *
 * 暴露接口（挂载到 window.Panels.formation）：
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

  // ── 队形定义 ──────────────────────────────────────────────
  const FORMATION_TYPES = ['Line', 'Wedge', 'V-Shape', 'Diamond'];

  // ── CSS 注入 ──────────────────────────────────────────────
  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'formation-panel-styles';
    _styleEl.textContent = `
      .fm-card {
        background: #101830; border: 1px solid #1a2a4a; border-radius: 6px;
        padding: 12px; margin-bottom: 8px;
        font-family: 'Segoe UI', 'Consolas', monospace; color: #c0d0f0;
      }
      .fm-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .fm-section { margin-top:10px; padding-top:8px; border-top:1px solid #182442; }
      .fm-section h4 { font-size:11px; color:#506a8e; margin:0 0 6px 0; letter-spacing:0.5px; }

      /* ── 无人机状态卡片 ── */
      .fm-drone-card { background:#0d1428; border:1px solid #182442; border-radius:5px; padding:8px 10px; margin-bottom:6px; }
      .fm-drone-card h5 { font-size:11px; margin:0 0 5px 0; display:flex; justify-content:space-between; align-items:center; }
      .fm-drone-name { color:#c0d8f0; }
      .fm-drone-status { font-size:10px; padding:1px 6px; border-radius:8px; text-transform:uppercase; }
      .fm-drone-status.online { color:#4ef0a0; background:rgba(78,240,160,0.12); }
      .fm-drone-status.offline { color:#f04e4e; background:rgba(240,78,78,0.12); }
      .fm-drone-status.warning { color:#f0a04e; background:rgba(240,160,78,0.12); }
      .fm-drone-meta { display:grid; grid-template-columns:1fr 1fr; gap:2px 8px; font-size:10px; }
      .fm-drone-meta span { color:#6a8aae; }
      .fm-drone-meta span b { color:#c0d8f0; font-weight:normal; }

      /* ── 队形选择器 ── */
      .fm-shape-grid { display:grid; grid-template-columns:1fr 1fr; gap:4px; }
      .fm-shape-btn { padding:6px 8px; border:1px solid #182442; border-radius:4px; background:#0d1428;
        color:#6a9fd8; font-size:11px; cursor:pointer; text-align:center; transition:all 0.2s; letter-spacing:1px; }
      .fm-shape-btn:hover { background:#141e38; border-color:#2a4a6a; }
      .fm-shape-btn.active { background:#1a2a4a; border-color:#4ea8ff; color:#4ea8ff; font-weight:bold; }

      /* ── 间距滑块 ── */
      .fm-spacing-row { display:flex; justify-content:space-between; align-items:center; padding:3px 0; font-size:11px; }
      .fm-spacing-label { color:#6a8aae; min-width:50px; }
      .fm-spacing-val { color:#4ef0a0; font-family:'Consolas',monospace; min-width:36px; text-align:right; font-size:10px; }
      .fm-spacing-slider { flex:1; margin:0 8px; -webkit-appearance:none; height:4px; border-radius:2px; background:#182442; outline:none; }
      .fm-spacing-slider::-webkit-slider-thumb { -webkit-appearance:none; width:12px;height:12px; border-radius:50%; background:#4ea8ff; cursor:pointer; }
    `;
    document.head.appendChild(_styleEl);
  }

  // ── DOM 构建 ──────────────────────────────────────────────
  function _buildRoot() {
    const root = document.createElement('div');
    root.className = 'fm-card';
    root.id = 'formation-panel-root';
    root.innerHTML = '<h3>FORMATION</h3>';

    // 无人机状态卡片区域
    const dronesSection = document.createElement('div');
    dronesSection.className = 'fm-section';
    dronesSection.innerHTML = '<h4>Swarm Status</h4>';
    const dronesWrap = document.createElement('div');
    dronesWrap.id = 'fm-drones-wrap';
    dronesSection.appendChild(dronesWrap);
    _refs.dronesWrap = dronesWrap;

    // 队形选择器
    const shapeSection = document.createElement('div');
    shapeSection.className = 'fm-section';
    shapeSection.innerHTML = '<h4>Formation Shape</h4>';
    const shapeGrid = document.createElement('div');
    shapeGrid.className = 'fm-shape-grid';
    shapeGrid.id = 'fm-shape-grid';
    shapeSection.appendChild(shapeGrid);
    _refs.shapeGrid = shapeGrid;

    // 间距控制
    const spacingSection = document.createElement('div');
    spacingSection.className = 'fm-section';
    spacingSection.id = 'fm-spacing-section';
    spacingSection.innerHTML =
      '<h4>Spacing Control</h4>' +
      '<div class="fm-spacing-row"><span class="fm-spacing-label">Lateral</span>' +
        '<input type="range" class="fm-spacing-slider" id="fm-spacing-lat" min="20" max="200" value="80">' +
        '<span class="fm-spacing-val" id="fm-spacing-lat-val">80m</span></div>' +
      '<div class="fm-spacing-row"><span class="fm-spacing-label">Longitudinal</span>' +
        '<input type="range" class="fm-spacing-slider" id="fm-spacing-lng" min="20" max="200" value="80">' +
        '<span class="fm-spacing-val" id="fm-spacing-lng-val">80m</span></div>' +
      '<div class="fm-spacing-row"><span class="fm-spacing-label">Altitude</span>' +
        '<input type="range" class="fm-spacing-slider" id="fm-spacing-alt" min="0" max="100" value="10">' +
        '<span class="fm-spacing-val" id="fm-spacing-alt-val">10m</span></div>';

    root.appendChild(dronesSection);
    root.appendChild(shapeSection);
    root.appendChild(spacingSection);

    _refs.spacingLat  = spacingSection.querySelector('#fm-spacing-lat');
    _refs.spacingLng  = spacingSection.querySelector('#fm-spacing-lng');
    _refs.spacingAlt  = spacingSection.querySelector('#fm-spacing-alt');
    _refs.spacingLatVal = spacingSection.querySelector('#fm-spacing-lat-val');
    _refs.spacingLngVal = spacingSection.querySelector('#fm-spacing-lng-val');
    _refs.spacingAltVal = spacingSection.querySelector('#fm-spacing-alt-val');

    return root;
  }

  // ── 渲染队形按钮 ──────────────────────────────────────────
  function _renderShapeButtons(activeShape) {
    if (!_refs.shapeGrid) return;
    _refs.shapeGrid.innerHTML = '';
    FORMATION_TYPES.forEach(function (shape) {
      const btn = document.createElement('button');
      btn.className = 'fm-shape-btn' + (shape === activeShape ? ' active' : '');
      btn.textContent = shape;
      btn.addEventListener('click', function () {
        _selectShape(shape);
      });
      _refs.shapeGrid.appendChild(btn);
    });
  }

  function _selectShape(shape) {
    // 更新按钮状态
    var btns = _refs.shapeGrid.querySelectorAll('.fm-shape-btn');
    btns.forEach(function (b) { b.classList.remove('active'); });
    for (var i = 0; i < btns.length; i++) {
      if (btns[i].textContent === shape) { btns[i].classList.add('active'); break; }
    }
    if (EventBus) EventBus.emit('formation:switch', shape);
    if (StateManager) StateManager.merge({ formation: Object.assign(StateManager.get('formation') || {}, { shape: shape }) });
  }

  // ── 渲染无人机状态 ────────────────────────────────────────
  function _renderDrones(drones) {
    if (!_refs.dronesWrap) return;
    _refs.dronesWrap.innerHTML = '';

    var defaultDrones = drones || [
      { id: 'Leader',  role: 'Leader',  online: true,  lat: 37.7700, lng: -122.4100, alt: 200, spd: 45.5, batt: 92, hdg: 90 },
      { id: 'Wing-1',  role: 'Wing 1',  online: true,  lat: 37.7704, lng: -122.4085, alt: 195, spd: 45.2, batt: 88, hdg: 90 },
      { id: 'Wing-2',  role: 'Wing 2',  online: true,  lat: 37.7695, lng: -122.4115, alt: 205, spd: 45.8, batt: 85, hdg: 89 },
    ];

    defaultDrones.forEach(function (dr) {
      var statusClass = dr.online ? 'online' : (dr.online === false ? 'offline' : 'warning');
      var statusText  = dr.online ? 'ONLINE' : (dr.online === false ? 'OFFLINE' : 'WARN');

      var card = document.createElement('div');
      card.className = 'fm-drone-card';
      card.innerHTML =
        '<h5><span class="fm-drone-name">' + dr.role + '</span><span class="fm-drone-status ' + statusClass + '">' + statusText + '</span></h5>' +
        '<div class="fm-drone-meta">' +
          '<span>Lat:</span><span><b>' + (dr.lat || 0).toFixed(4) + '</b></span>' +
          '<span>Lng:</span><span><b>' + (dr.lng || 0).toFixed(4) + '</b></span>' +
          '<span>Alt:</span><span><b>' + (dr.alt || 0).toFixed(0) + 'm</b></span>' +
          '<span>Spd:</span><span><b>' + (dr.spd || 0).toFixed(1) + 'm/s</b></span>' +
          '<span>Hdg:</span><span><b>' + String(Math.round(dr.hdg || 0)).padStart(3, '0') + '°</b></span>' +
          '<span>Bat:</span><span><b>' + (dr.batt || 0) + '%</b></span>' +
        '</div>';
      _refs.dronesWrap.appendChild(card);
    });
  }

  // ── 更新间距显示 ──────────────────────────────────────────
  function _updateSpacingVal(slider, valEl, unit) {
    valEl.textContent = slider.value + unit;
  }

  // ── 数据刷新 ──────────────────────────────────────────────
  function refresh(data) {
    var f = data || (StateManager ? StateManager.get('formation') : null) || {};
    if (f.drones) _renderDrones(f.drones);
    if (f.shape) _renderShapeButtons(f.shape);
    if (f.spacing) {
      if (_refs.spacingLat) { _refs.spacingLat.value = f.spacing.lateral || 80; _updateSpacingVal(_refs.spacingLat, _refs.spacingLatVal, 'm'); }
      if (_refs.spacingLng) { _refs.spacingLng.value = f.spacing.longitudinal || 80; _updateSpacingVal(_refs.spacingLng, _refs.spacingLngVal, 'm'); }
      if (_refs.spacingAlt) { _refs.spacingAlt.value = f.spacing.altitude || 10; _updateSpacingVal(_refs.spacingAlt, _refs.spacingAltVal, 'm'); }
    }
  }

  // ── 事件绑定 ──────────────────────────────────────────────
  function _bindEvents() {
    // 滑块事件
    if (_refs.spacingLat) {
      _refs.spacingLat.addEventListener('input', function () {
        _updateSpacingVal(_refs.spacingLat, _refs.spacingLatVal, 'm');
        _emitSpacing();
      });
    }
    if (_refs.spacingLng) {
      _refs.spacingLng.addEventListener('input', function () {
        _updateSpacingVal(_refs.spacingLng, _refs.spacingLngVal, 'm');
        _emitSpacing();
      });
    }
    if (_refs.spacingAlt) {
      _refs.spacingAlt.addEventListener('input', function () {
        _updateSpacingVal(_refs.spacingAlt, _refs.spacingAltVal, 'm');
        _emitSpacing();
      });
    }

    if (StateManager) {
      var unsub = StateManager.subscribe('formation', function (newVal) { refresh(newVal); });
      _subscriptions.push(unsub);
      var cur = StateManager.get('formation');
      if (cur) refresh(cur);
    }
  }

  function _emitSpacing() {
    var spacing = {
      lateral: parseInt(_refs.spacingLat.value),
      longitudinal: parseInt(_refs.spacingLng.value),
      altitude: parseInt(_refs.spacingAlt.value),
    };
    if (EventBus) EventBus.emit('formation:spacing:changed', spacing);
    if (StateManager) StateManager.merge({ formation: Object.assign(StateManager.get('formation') || {}, { spacing: spacing }) });
  }

  // ── 公开接口 ──────────────────────────────────────────────
  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[FormationPanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _renderDrones();
    _renderShapeButtons('Wedge');
    _bindEvents();
    console.log('[FormationPanel] Initialized in', containerId);
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
    console.log('[FormationPanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.formation = { init: init, refresh: refresh, destroy: destroy };
})(window);
