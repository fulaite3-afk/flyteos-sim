/**
 * sensor_panel.js — 传感器面板
 * ============================================================================
 * 功能：展示四维传感器（IMU 陀螺仪、气压高度计、GPS 模块、磁力计）的
 *       实时数据和健康状态，支持进度条 + 状态灯可视化。
 *       监听 EventBus 的 sensor:fault 事件实现故障注入。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   state:sensors:changed         — 监视 sensors 状态对象自动刷新
 *   sensor:fault                  — 接收故障注入（{ sensor:'IMU', fault:true }）
 *   sensor:fault:cleared          — 清除指定传感器故障
 *
 * 暴露接口（挂载到 window.Panels.sensor）：
 *   init(containerId)      → 在指定容器中创建面板 DOM
 *   refresh(data?)         → 手动刷新面板
 *   injectFault(sensorId)  → 注入故障到指定传感器
 *   clearFault(sensorId)   → 清除指定传感器故障
 *   destroy()              → 移除面板 DOM 及所有订阅
 * ============================================================================
 */

(function (global) {
  'use strict';

  const EventBus = global.EventBus;
  const StateManager = global.StateManager;

  let _container = null, _rootEl = null, _styleEl = null;
  let _subscriptions = [], _eventUnsubs = [];
  let _faults = {};
  const _sensorRefs = {};

  const SENSOR_DEFS = [
    { id: 'IMU',  label: 'IMU Gyro',      unit: 'dps',  range: 2000, icon: '\u21BB' },
    { id: 'BARO', label: 'Barometer',      unit: 'hPa',  range: 1100, icon: '\u2195' },
    { id: 'GPS',  label: 'GPS Module',     unit: 'sats', range: 24,   icon: '\u2606' },
    { id: 'MAG',  label: 'Magnetometer',   unit: '\xB5T',range: 100,  icon: '\u2191' },
  ];

  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'sensor-panel-styles';
    _styleEl.textContent = `
      .sp-card { background:#101830; border:1px solid #1a2a4a; border-radius:6px; padding:12px; margin-bottom:8px; font-family:'Segoe UI','Consolas',monospace; }
      .sp-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .sp-sensor { margin-bottom:10px; padding:8px; background:#0c1426; border-radius:4px; border:1px solid #182442; transition:border-color 0.3s; }
      .sp-sensor.fault { border-color:#f04e4e; background:#1a0a0a; }
      .sp-sensor-header { display:flex; align-items:center; justify-content:space-between; margin-bottom:4px; }
      .sp-sensor-name { font-size:12px; font-weight:600; color:#8899bb; }
      .sp-sensor-value { font-size:12px; font-family:'Consolas',monospace; font-weight:bold; color:#4ef0a0; }
      .sp-sensor-value.fault { color:#f04e4e; }
      .sp-bar-track { height:6px; background:#1a2a4a; border-radius:3px; overflow:hidden; margin-top:4px; }
      .sp-bar-fill { height:100%; border-radius:3px; background:linear-gradient(90deg,#4ef0a0,#4ea8ff); transition:width 0.5s ease,background 0.3s; }
      .sp-bar-fill.warn { background:linear-gradient(90deg,#f0a04e,#f0d04e); }
      .sp-bar-fill.fault { background:#f04e4e; animation:sp-blink-bar 0.5s infinite; }
      .sp-status-light { display:inline-block; width:8px; height:8px; border-radius:50%; margin-right:6px; transition:background 0.3s; }
      .sp-light-ok    { background:#4ef0a0; box-shadow:0 0 6px #4ef0a0; }
      .sp-light-warn  { background:#f0a04e; box-shadow:0 0 6px #f0a04e; }
      .sp-light-fault { background:#f04e4e; box-shadow:0 0 6px #f04e4e; animation:sp-blink 0.5s infinite; }
      .sp-footer { margin-top:10px; padding-top:8px; border-top:1px solid #182442; font-size:11px; color:#556688; display:flex; justify-content:space-between; }
      @keyframes sp-blink { 0%,100%{opacity:1;} 50%{opacity:0.2;} }
      @keyframes sp-blink-bar { 0%,100%{opacity:1;} 50%{opacity:0.4;} }
    `;
    document.head.appendChild(_styleEl);
  }

  function _buildRoot() {
    const root = document.createElement('div');
    root.className = 'sp-card';
    root.id = 'sensor-panel-root';

    const h3 = document.createElement('h3');
    h3.textContent = 'SENSOR ARRAY';
    root.appendChild(h3);

    SENSOR_DEFS.forEach(function (def) {
      const el = document.createElement('div');
      el.className = 'sp-sensor';
      el.id = 'sp-sensor-' + def.id;
      el.innerHTML =
        '<div class="sp-sensor-header">' +
          '<span class="sp-sensor-name"><span class="sp-status-light sp-light-ok" id="sp-light-' + def.id + '"></span>' + def.icon + ' ' + def.label + '</span>' +
          '<span class="sp-sensor-value" id="sp-val-' + def.id + '">-- ' + def.unit + '</span>' +
        '</div>' +
        '<div class="sp-bar-track"><div class="sp-bar-fill" id="sp-bar-' + def.id + '" style="width:0%"></div></div>';
      root.appendChild(el);
      _sensorRefs[def.id] = {
        el: el,
        bar: el.querySelector('#sp-bar-' + def.id),
        val: el.querySelector('#sp-val-' + def.id),
        light: el.querySelector('#sp-light-' + def.id),
      };
    });

    const footer = document.createElement('div');
    footer.className = 'sp-footer';
    footer.innerHTML = '<span id="sp-summary">All sensors nominal</span><span id="sp-fault-count">Faults: 0</span>';
    root.appendChild(footer);
    _sensorRefs._summary    = root.querySelector('#sp-summary');
    _sensorRefs._faultCount = root.querySelector('#sp-fault-count');

    return root;
  }

  function refresh(data) {
    const sensors = data || (StateManager ? StateManager.get('sensors') : null) || {};
    let faultyCount = 0;

    SENSOR_DEFS.forEach(function (def) {
      const refs = _sensorRefs[def.id];
      if (!refs) return;
      const val = sensors[def.id];
      const isFault = _faults[def.id] === true;
      const percent = val != null ? Math.min(100, (val / def.range) * 100) : 0;

      if (refs.val) {
        refs.val.textContent = isFault ? 'FAULT' : ((val != null ? val.toFixed(1) : '--') + ' ' + def.unit);
        refs.val.className = 'sp-sensor-value' + (isFault ? ' fault' : '');
      }
      if (refs.bar) {
        refs.bar.style.width = isFault ? '100%' : percent + '%';
        refs.bar.className = 'sp-bar-fill' + (isFault ? ' fault' : percent > 85 ? ' warn' : '');
      }
      if (refs.light) refs.light.className = 'sp-status-light ' + (isFault ? 'sp-light-fault' : 'sp-light-ok');
      if (refs.el) refs.el.className = 'sp-sensor' + (isFault ? ' fault' : '');
      if (isFault) faultyCount++;
    });

    if (_sensorRefs._summary) _sensorRefs._summary.textContent = faultyCount > 0 ? faultyCount + ' sensor(s) in FAULT state' : 'All sensors nominal';
    if (_sensorRefs._faultCount) _sensorRefs._faultCount.textContent = 'Faults: ' + faultyCount;
  }

  function injectFault(sensorId) {
    if (!SENSOR_DEFS.find(function (d) { return d.id === sensorId; })) { console.warn('[SensorPanel] Unknown sensor:', sensorId); return; }
    _faults[sensorId] = true;
    refresh();
    if (EventBus) EventBus.emit('sensor:fault:injected', { sensor: sensorId, faults: _faults });
  }

  function clearFault(sensorId) {
    if (sensorId) delete _faults[sensorId]; else _faults = {};
    refresh();
    if (EventBus) EventBus.emit('sensor:fault:cleared', { sensor: sensorId, faults: _faults });
  }

  function _bindEvents() {
    if (StateManager) {
      _subscriptions.push(StateManager.subscribe('sensors', function (newVal) { refresh(newVal); }));
      const cur = StateManager.get('sensors');
      if (cur) refresh(cur); else refresh();
    }
    if (EventBus) {
      _eventUnsubs.push(EventBus.on('sensor:fault', function (payload) {
        if (payload && payload.sensor) payload.fault ? injectFault(payload.sensor) : clearFault(payload.sensor);
      }));
      _eventUnsubs.push(EventBus.on('sensor:fault:clearAll', function () { clearFault(); }));
    }
  }

  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[SensorPanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _bindEvents();
    console.log('[SensorPanel] Initialized in', containerId);
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
    console.log('[SensorPanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.sensor = { init: init, refresh: refresh, injectFault: injectFault, clearFault: clearFault, destroy: destroy };
})(window);
