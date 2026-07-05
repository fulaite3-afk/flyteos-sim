/**
 * flight_panel.js — 飞控面板
 * ============================================================================
 * 功能：实时展示飞行器姿态数据（GPS、速度、高度、航向）、飞行模式、
 *       当前航点、围栏状态。纯 JS 生成 DOM，零外部 HTML 依赖。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   state:flight:changed        — 监视 flight 状态对象，自动刷新面板
 *   demo:phase:*                — 响应起飞/巡航/着陆阶段变化
 *
 * 暴露接口（挂载到 window.Panels.flight）：
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

  // ── CSS 注入 ──────────────────────────────────────────────
  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'flight-panel-styles';
    _styleEl.textContent = `
      .fp-card {
        background: #101830; border: 1px solid #1a2a4a; border-radius: 6px;
        padding: 12px; margin-bottom: 8px;
        font-family: 'Segoe UI', 'Consolas', monospace;
      }
      .fp-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .fp-row { display:flex; justify-content:space-between; padding:3px 0; font-size:12px; }
      .fp-label { color:#8899bb; }
      .fp-val { color:#4ef0a0; font-family:'Consolas',monospace; font-weight:bold; transition:color 0.3s; }
      .fp-val.warn  { color:#f0a04e; }
      .fp-val.danger { color:#f04e4e; animation:fp-blink 0.6s infinite; }
      .fp-section { margin-top:10px; padding-top:8px; border-top:1px solid #182442; }
      .fp-section h4 { font-size:11px; color:#506a8e; margin:0 0 6px 0; letter-spacing:0.5px; }
      .fp-mode-badge { display:inline-block; padding:2px 8px; border-radius:3px; font-size:11px; font-weight:bold; letter-spacing:1px; }
      .fp-mode-IDLE     { background:#1a2a4a; color:#6a9fd8; }
      .fp-mode-TAKEOFF  { background:#1a3a1a; color:#4ef0a0; }
      .fp-mode-CRUISE   { background:#1a2a4a; color:#4ea8ff; }
      .fp-mode-PAUSED   { background:#3a2a0a; color:#f0a04e; }
      .fp-mode-LANDED   { background:#1a3a1a; color:#4ef0a0; }
      .fp-mode-STOPPED  { background:#3a1a1a; color:#f04e4e; }
      @keyframes fp-blink { 0%,100%{opacity:1;} 50%{opacity:0.3;} }
    `;
    document.head.appendChild(_styleEl);
  }

  // ── DOM 构建 ──────────────────────────────────────────────
  function _buildRoot() {
    const root = document.createElement('div');
    root.className = 'fp-card';
    root.id = 'flight-panel-root';
    root.innerHTML =
      '<h3>FLIGHT CONTROL</h3>' +
      '<div class="fp-section"><h4>Position (GPS)</h4>' +
        '<div class="fp-row"><span class="fp-label">Latitude</span><span class="fp-val" id="fp-gps-lat">--</span></div>' +
        '<div class="fp-row"><span class="fp-label">Longitude</span><span class="fp-val" id="fp-gps-lng">--</span></div>' +
        '<div class="fp-row"><span class="fp-label">Altitude (MSL)</span><span class="fp-val" id="fp-gps-alt">--</span></div>' +
        '<div class="fp-row"><span class="fp-label">GPS Satellites</span><span class="fp-val" id="fp-gps-sats">12</span></div>' +
      '</div>' +
      '<div class="fp-section"><h4>Flight Dynamics</h4>' +
        '<div class="fp-row"><span class="fp-label">Speed (Ground)</span><span class="fp-val" id="fp-dyn-spd">--</span></div>' +
        '<div class="fp-row"><span class="fp-label">Heading</span><span class="fp-val" id="fp-dyn-hdg">--</span></div>' +
        '<div class="fp-row"><span class="fp-label">Vertical Rate</span><span class="fp-val" id="fp-dyn-vs">--</span></div>' +
      '</div>' +
      '<div class="fp-section"><h4>Mission</h4>' +
        '<div class="fp-row"><span class="fp-label">Flight Mode</span><span class="fp-mode-badge fp-mode-IDLE" id="fp-msn-mode">IDLE</span></div>' +
        '<div class="fp-row"><span class="fp-label">Waypoint</span><span class="fp-val" id="fp-msn-wp">--</span></div>' +
        '<div class="fp-row"><span class="fp-label">Geofence</span><span class="fp-val safe" id="fp-msn-gf">SAFE</span></div>' +
      '</div>';

    _refs.lat  = root.querySelector('#fp-gps-lat');
    _refs.lng  = root.querySelector('#fp-gps-lng');
    _refs.alt  = root.querySelector('#fp-gps-alt');
    _refs.sats = root.querySelector('#fp-gps-sats');
    _refs.spd  = root.querySelector('#fp-dyn-spd');
    _refs.hdg  = root.querySelector('#fp-dyn-hdg');
    _refs.vs   = root.querySelector('#fp-dyn-vs');
    _refs.mode = root.querySelector('#fp-msn-mode');
    _refs.wp   = root.querySelector('#fp-msn-wp');
    _refs.gf   = root.querySelector('#fp-msn-gf');

    return root;
  }

  // ── 数据刷新 ──────────────────────────────────────────────
  function refresh(data) {
    const f = data || (StateManager ? StateManager.get('flight') : null);
    if (!f) return;

    if (_refs.lat)  _refs.lat.textContent  = (f.lat || 0).toFixed(6);
    if (_refs.lng)  _refs.lng.textContent  = (f.lng || 0).toFixed(6);
    if (_refs.alt)  _refs.alt.textContent  = (f.alt || 0).toFixed(0) + ' m';
    if (_refs.sats) _refs.sats.textContent = f.gpsSats || 12;
    if (_refs.spd)  _refs.spd.textContent  = (f.spd || 0).toFixed(1) + ' m/s';
    if (_refs.hdg)  _refs.hdg.textContent  = String(Math.round(f.hdg || 0)).padStart(3, '0') + '\u00B0';
    if (_refs.vs)   _refs.vs.textContent   = (f.vertRate != null ? f.vertRate.toFixed(1) + ' m/s' : '--');

    if (_refs.mode && f.phase) {
      const phase = f.phase.toUpperCase();
      _refs.mode.textContent = phase;
      _refs.mode.className = 'fp-mode-badge fp-mode-' + phase;
    }
    if (_refs.wp) _refs.wp.textContent = f.wpLabel || '--';
    if (_refs.gf && f.gfStatus) {
      _refs.gf.textContent = f.gfStatus.toUpperCase();
      _refs.gf.className = 'fp-val ' + (f.gfStatus === 'violation' ? 'danger' : f.gfStatus === 'warn' ? 'warn' : 'safe');
    }
  }

  // ── 事件绑定 ──────────────────────────────────────────────
  function _bindEvents() {
    if (StateManager) {
      const unsub = StateManager.subscribe('flight', function (newVal) { refresh(newVal); });
      _subscriptions.push(unsub);
      const cur = StateManager.get('flight');
      if (cur) refresh(cur);
    }
    if (EventBus) {
      _eventUnsubs.push(EventBus.on('flight:phase:changed', function (phase) {
        console.log('[FlightPanel] Phase transition:', phase);
      }));
    }
  }

  // ── 公开接口 ──────────────────────────────────────────────
  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[FlightPanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _bindEvents();
    console.log('[FlightPanel] Initialized in', containerId);
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
    console.log('[FlightPanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.flight = { init: init, refresh: refresh, destroy: destroy };
})(window);
