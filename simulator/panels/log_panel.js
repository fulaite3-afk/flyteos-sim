/**
 * log_panel.js — 操作日志面板
 * ============================================================================
 * 功能：50 条滚动操作日志列表，每条日志带时间戳、角色标签（System /
 *       Flight / Sensor / Geofence / Health）、严重等级颜色。
 *       底部审计统计摘要：按角色/等级分类计数。纯 JS 生成 DOM。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   log:add                      — 新增一条日志 { role, level, message }
 *   log:clear                    — 清空全部日志
 *   sensor:fault:* / health:*    — 自动将关键事件写入日志
 *
 * 暴露接口（挂载到 window.Panels.log）：
 *   init(containerId)      → 在指定容器中创建面板 DOM
 *   add(role, level, msg)  → 添加一条日志
 *   clear()                → 清空所有日志
 *   destroy()              → 移除面板 DOM 及所有订阅
 * ============================================================================
 */

(function (global) {
  'use strict';

  const EventBus = global.EventBus;
  const StateManager = global.StateManager;

  let _container = null, _rootEl = null, _styleEl = null;
  let _subscriptions = [], _eventUnsubs = [];
  const MAX_ENTRIES = 50;
  const _entries = [];
  const _stats = {};
  const ROLES  = ['System', 'Flight', 'Sensor', 'Geofence', 'Health'];
  const LEVELS = ['info', 'warn', 'error'];
  const _refs = {};

  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'log-panel-styles';
    _styleEl.textContent = `
      .lp-card { background:#101830; border:1px solid #1a2a4a; border-radius:6px; padding:12px; margin-bottom:8px; font-family:'Segoe UI','Consolas',monospace; }
      .lp-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .lp-list { max-height:260px; overflow-y:auto; font-size:11px; margin-bottom:8px; }
      .lp-entry { display:flex; align-items:flex-start; padding:3px 0; border-bottom:1px solid #0d1225; line-height:1.4; }
      .lp-time { color:#445566; margin-right:8px; white-space:nowrap; font-family:'Consolas',monospace; }
      .lp-role { display:inline-block; padding:0 5px; border-radius:2px; font-size:10px; font-weight:bold; margin-right:6px; white-space:nowrap; line-height:16px; }
      .lp-role-System   { background:#1a2a4a; color:#6a9fd8; }
      .lp-role-Flight   { background:#1a3a1a; color:#4ef0a0; }
      .lp-role-Sensor   { background:#2a1a3a; color:#b06aff; }
      .lp-role-Geofence { background:#3a1a1a; color:#f04e4e; }
      .lp-role-Health   { background:#1a2a3a; color:#4ea8ff; }
      .lp-msg { color:#8899bb; flex:1; word-break:break-word; }
      .lp-msg.warn  { color:#f0a04e; }
      .lp-msg.error { color:#f04e4e; }
      .lp-stats { padding-top:8px; border-top:1px solid #182442; font-size:11px; color:#556688; }
      .lp-stats-table { width:100%; border-collapse:collapse; margin-top:4px; }
      .lp-stats-table th,.lp-stats-table td { padding:2px 4px; text-align:center; border:1px solid #182442; }
      .lp-stats-table th { background:#0c1426; color:#6a9fd8; font-weight:normal; font-size:10px; }
      .lp-stats-table td { font-family:'Consolas',monospace; font-size:11px; font-weight:bold; }
      .lp-stat-warn  { color:#f0a04e; }
      .lp-stat-error { color:#f04e4e; }
      .lp-list::-webkit-scrollbar { width:4px; }
      .lp-list::-webkit-scrollbar-track { background:#0a0e1a; }
      .lp-list::-webkit-scrollbar-thumb { background:#1e3050; border-radius:2px; }
    `;
    document.head.appendChild(_styleEl);
  }

  function _buildRoot() {
    const root = document.createElement('div');
    root.className = 'lp-card';
    root.id = 'log-panel-root';

    const h3 = document.createElement('h3');
    h3.textContent = 'OPERATION LOG';
    root.appendChild(h3);

    const list = document.createElement('div');
    list.className = 'lp-list';
    list.id = 'lp-list';
    root.appendChild(list);
    _refs.list = list;

    const stats = document.createElement('div');
    stats.className = 'lp-stats';
    stats.id = 'lp-stats';
    stats.innerHTML = '<strong>Audit Summary</strong>';
    root.appendChild(stats);
    _refs.stats = stats;

    return root;
  }

  function _buildStatsHTML() {
    let html = '<strong>Audit Summary</strong><table class="lp-stats-table"><thead><tr><th></th>';
    ROLES.forEach(function (r) { html += '<th>' + r + '</th>'; });
    html += '</tr></thead><tbody>';

    LEVELS.forEach(function (level) {
      html += '<tr><td style="color:#8899bb">' + level.toUpperCase() + '</td>';
      ROLES.forEach(function (r) {
        const count = _stats[r + ':' + level] || 0;
        const cls = level === 'warn' ? 'lp-stat-warn' : level === 'error' ? 'lp-stat-error' : '';
        html += '<td class="' + cls + '">' + count + '</td>';
      });
      html += '</tr>';
    });

    html += '<tr style="border-top:2px solid #1a2a4a"><td style="color:#c0d0f0">TOTAL</td>';
    ROLES.forEach(function (r) {
      let total = 0;
      LEVELS.forEach(function (l) { total += (_stats[r + ':' + l] || 0); });
      html += '<td style="color:#c0d0f0">' + total + '</td>';
    });
    html += '</tr></tbody></table>';
    html += '<div style="margin-top:4px;font-size:10px;color:#445566">Total entries: ' + _entries.length + ' / ' + MAX_ENTRIES + ' max</div>';
    return html;
  }

  function _renderStats() { if (_refs.stats) _refs.stats.innerHTML = _buildStatsHTML(); }

  function _renderEntry(entry) {
    const div = document.createElement('div');
    div.className = 'lp-entry';
    const time = document.createElement('span');
    time.className = 'lp-time';
    time.textContent = entry.time || '--:--:--';
    div.appendChild(time);
    const role = document.createElement('span');
    role.className = 'lp-role lp-role-' + (entry.role || 'System');
    role.textContent = (entry.role || 'SYS').slice(0, 4).toUpperCase();
    div.appendChild(role);
    const msg = document.createElement('span');
    msg.className = 'lp-msg' + (entry.level !== 'info' ? ' ' + entry.level : '');
    msg.textContent = entry.message || '';
    div.appendChild(msg);
    return div;
  }

  function _refreshList() {
    if (!_refs.list) return;
    _refs.list.innerHTML = '';
    for (let i = _entries.length - 1; i >= 0; i--) _refs.list.appendChild(_renderEntry(_entries[i]));
  }

  function add(role, level, message) {
    if (!role) role = 'System';
    if (!level) level = 'info';
    const now = new Date();
    const timeStr = String(now.getHours()).padStart(2,'0') + ':' + String(now.getMinutes()).padStart(2,'0') + ':' + String(now.getSeconds()).padStart(2,'0');
    _entries.push({ time: timeStr, role: role, level: level, message: message });
    while (_entries.length > MAX_ENTRIES) _entries.shift();
    const key = role + ':' + level;
    _stats[key] = (_stats[key] || 0) + 1;
    _refreshList();
    _renderStats();
  }

  function clear() {
    _entries.length = 0;
    for (const k in _stats) { if (_stats.hasOwnProperty(k)) delete _stats[k]; }
    _refreshList();
    _renderStats();
  }

  function _bindEvents() {
    if (!EventBus) return;

    _eventUnsubs.push(EventBus.on('log:add', function (payload) {
      if (payload) add(payload.role || 'System', payload.level || 'info', payload.message || '');
    }));
    _eventUnsubs.push(EventBus.on('log:clear', function () { clear(); }));

    // 自动记录故障/健康事件
    _eventUnsubs.push(EventBus.on('sensor:fault:injected', function (payload) {
      add('Sensor', 'error', 'FAULT INJECTED: ' + (payload && payload.sensor || 'UNKNOWN'));
    }));
    _eventUnsubs.push(EventBus.on('sensor:fault:cleared', function (payload) {
      add('Sensor', 'info', 'Fault cleared: ' + (payload && payload.sensor || 'ALL'));
    }));
    _eventUnsubs.push(EventBus.on('health:lowBattery:warning', function () {
      add('Health', 'warn', 'Low battery warning! Flight time critical.');
    }));

    if (StateManager) {
      _subscriptions.push(StateManager.subscribe('flight', function (newVal, oldVal) {
        if (newVal && oldVal && newVal.phase !== oldVal.phase) {
          add('Flight', 'info', 'Phase: ' + (oldVal.phase || 'N/A') + ' -> ' + (newVal.phase || 'N/A'));
        }
        if (newVal && oldVal && newVal.gfStatus !== oldVal.gfStatus) {
          const level = newVal.gfStatus === 'violation' ? 'error' : newVal.gfStatus === 'warn' ? 'warn' : 'info';
          add('Geofence', level, 'Status: ' + (oldVal.gfStatus || 'N/A') + ' -> ' + (newVal.gfStatus || 'N/A'));
        }
      }));
    }
  }

  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[LogPanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _bindEvents();
    add('System', 'info', 'Log panel initialized');
    console.log('[LogPanel] Initialized in', containerId);
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
    _entries.length = 0;
    for (const k in _stats) { if (_stats.hasOwnProperty(k)) delete _stats[k]; }
    console.log('[LogPanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.log = { init: init, add: add, clear: clear, destroy: destroy };
})(window);
