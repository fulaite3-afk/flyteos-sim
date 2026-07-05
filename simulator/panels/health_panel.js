/**
 * health_panel.js — 健康监控面板
 * ============================================================================
 * 功能：展示飞行器总体健康评分、四维子系统状态（电池/动力/通信/导航）、
 *       电量倒计时、Canvas 健康趋势图。纯 JS 生成 DOM。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   state:health:changed          — 监视 health 状态对象自动刷新
 *   health:lowBattery:warning     — 低电量警告
 *
 * 暴露接口（挂载到 window.Panels.health）：
 *   init(containerId)      → 在指定容器中创建面板 DOM
 *   refresh(data?)         → 手动刷新面板
 *   destroy()              → 移除面板 DOM 及所有订阅
 * ============================================================================
 */

(function (global) {
  'use strict';

  const EventBus = global.EventBus;
  const StateManager = global.StateManager;

  let _container = null, _rootEl = null, _styleEl = null;
  let _subscriptions = [], _eventUnsubs = [];
  let _canvasEl = null, _canvasCtx = null;
  const _trendBuffer = [];
  const MAX_TREND_POINTS = 60;
  const _refs = {};

  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'health-panel-styles';
    _styleEl.textContent = `
      .hp-card { background:#101830; border:1px solid #1a2a4a; border-radius:6px; padding:12px; margin-bottom:8px; font-family:'Segoe UI','Consolas',monospace; }
      .hp-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .hp-score-section { display:flex; align-items:center; justify-content:space-between; padding:8px 0; }
      .hp-score-ring { position:relative; width:64px; height:64px; }
      .hp-score-ring svg { width:64px; height:64px; }
      .hp-score-num { position:absolute; top:50%; left:50%; transform:translate(-50%,-50%); font-size:22px; font-weight:bold; color:#4ef0a0; }
      .hp-score-grade { font-size:14px; font-weight:bold; padding:4px 12px; border-radius:3px; letter-spacing:1px; }
      .hp-grade-A { background:#1a3a1a; color:#4ef0a0; }
      .hp-grade-B { background:#2a3a0a; color:#c0f04e; }
      .hp-grade-C { background:#3a2a0a; color:#f0a04e; }
      .hp-grade-D { background:#3a1a0a; color:#f05a4e; }
      .hp-grade-F { background:#3a1a1a; color:#f04e4e; }
      .hp-subsys-grid { display:grid; grid-template-columns:1fr 1fr; gap:6px; margin-top:10px; }
      .hp-subsys { padding:6px 8px; background:#0c1426; border-radius:4px; border:1px solid #182442; font-size:11px; }
      .hp-subsys-name { color:#8899bb; }
      .hp-subsys-bar { height:4px; background:#1a2a4a; border-radius:2px; margin-top:3px; overflow:hidden; }
      .hp-subsys-fill { height:100%; border-radius:2px; transition:width 0.5s; }
      .hp-subsys-fill.ok   { background:#4ef0a0; }
      .hp-subsys-fill.warn { background:#f0a04e; }
      .hp-subsys-fill.bad  { background:#f04e4e; }
      .hp-battery { margin-top:10px; padding:8px; background:#0c1426; border-radius:4px; border:1px solid #182442; text-align:center; }
      .hp-battery-label { font-size:11px; color:#8899bb; margin-bottom:4px; }
      .hp-battery-time { font-size:28px; font-weight:bold; color:#4ef0a0; font-family:'Consolas',monospace; }
      .hp-battery-time.low { color:#f04e4e; animation:hp-blink 0.8s infinite; }
      .hp-trend { margin-top:10px; }
      .hp-trend h4 { font-size:11px; color:#506a8e; margin:0 0 4px 0; }
      .hp-trend canvas { width:100%; height:80px; background:#0c1426; border-radius:4px; border:1px solid #182442; }
      @keyframes hp-blink { 0%,100%{opacity:1;} 50%{opacity:0.3;} }
    `;
    document.head.appendChild(_styleEl);
  }

  function _drawTrend() {
    if (!_canvasCtx) return;
    const w = _canvasEl.width, h = _canvasEl.height;
    const ctx = _canvasCtx;
    ctx.clearRect(0, 0, w, h);

    // 网格线
    ctx.strokeStyle = '#182442';
    ctx.lineWidth = 0.5;
    for (let i = 0; i <= 4; i++) {
      const y = (h / 4) * i;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    }

    if (_trendBuffer.length < 2) return;

    const stepX = w / MAX_TREND_POINTS;
    const marginL = _trendBuffer.length < MAX_TREND_POINTS ? (MAX_TREND_POINTS - _trendBuffer.length) * stepX : 0;

    ctx.strokeStyle = '#4ea8ff';
    ctx.lineWidth = 2;
    ctx.shadowColor = 'rgba(78,168,255,0.5)';
    ctx.shadowBlur = 4;
    ctx.beginPath();
    for (let i = 0; i < _trendBuffer.length; i++) {
      const x = marginL + i * stepX;
      const y = h - (_trendBuffer[i] / 100) * (h - 4) - 2;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.shadowBlur = 0;

    if (_trendBuffer.length > 0) {
      const lastX = marginL + (_trendBuffer.length - 1) * stepX;
      const lastY = h - (_trendBuffer[_trendBuffer.length - 1] / 100) * (h - 4) - 2;
      ctx.fillStyle = '#4ef0a0';
      ctx.beginPath();
      ctx.arc(lastX, lastY, 3, 0, Math.PI * 2);
      ctx.fill();
    }
  }

  function _pushTrend(score) {
    _trendBuffer.push(score);
    if (_trendBuffer.length > MAX_TREND_POINTS) _trendBuffer.shift();
    _drawTrend();
  }

  function _computeGrade(score) {
    if (score >= 90) return 'A';
    if (score >= 75) return 'B';
    if (score >= 60) return 'C';
    if (score >= 40) return 'D';
    return 'F';
  }

  function _updateRing(score) {
    if (!_refs.ringFg) return;
    const circumference = 176;
    _refs.ringFg.setAttribute('stroke-dashoffset', circumference - (score / 100) * circumference);
    let color;
    if (score >= 90) color = '#4ef0a0';
    else if (score >= 75) color = '#c0f04e';
    else if (score >= 60) color = '#f0a04e';
    else color = '#f04e4e';
    _refs.ringFg.setAttribute('stroke', color);
  }

  function _buildRoot() {
    const root = document.createElement('div');
    root.className = 'hp-card';
    root.id = 'health-panel-root';

    const h3 = document.createElement('h3');
    h3.textContent = 'HEALTH MONITOR';
    root.appendChild(h3);

    // 评分区
    const scoreSec = document.createElement('div');
    scoreSec.className = 'hp-score-section';
    scoreSec.innerHTML =
      '<div class="hp-score-ring">' +
        '<svg viewBox="0 0 64 64">' +
          '<circle cx="32" cy="32" r="28" fill="none" stroke="#1a2a4a" stroke-width="6"/>' +
          '<circle cx="32" cy="32" r="28" fill="none" stroke="#4ef0a0" stroke-width="6" stroke-linecap="round" stroke-dasharray="176" stroke-dashoffset="88" id="hp-ring-fg" transform="rotate(-90 32 32)"/>' +
        '</svg>' +
        '<div class="hp-score-num" id="hp-score">--</div>' +
      '</div>' +
      '<div class="hp-score-grade hp-grade-A" id="hp-grade">A</div>';
    root.appendChild(scoreSec);

    // 子系统网格
    const subsysGrid = document.createElement('div');
    subsysGrid.className = 'hp-subsys-grid';
    ['battery','motors','comm','navigation'].forEach(function (id) {
      const label = { battery:'Battery', motors:'Motors', comm:'Comm Link', navigation:'Navigation' }[id];
      const div = document.createElement('div');
      div.className = 'hp-subsys';
      div.innerHTML =
        '<span class="hp-subsys-name">' + label + '</span>' +
        '<span style="float:right;font-size:11px;color:#4ef0a0;" id="hp-sub-' + id + '-val">95%</span>' +
        '<div class="hp-subsys-bar"><div class="hp-subsys-fill ok" id="hp-sub-' + id + '-bar" style="width:95%"></div></div>';
      subsysGrid.appendChild(div);
      _refs['sub_' + id + '_val'] = div.querySelector('#hp-sub-' + id + '-val');
      _refs['sub_' + id + '_bar'] = div.querySelector('#hp-sub-' + id + '-bar');
    });
    root.appendChild(subsysGrid);

    // 电量倒计时
    const battery = document.createElement('div');
    battery.className = 'hp-battery';
    battery.innerHTML =
      '<div class="hp-battery-label">ESTIMATED FLIGHT TIME REMAINING</div>' +
      '<div class="hp-battery-time" id="hp-battery-time">--:--:--</div>';
    root.appendChild(battery);

    // 趋势图
    const trend = document.createElement('div');
    trend.className = 'hp-trend';
    trend.innerHTML = '<h4>HEALTH TREND (60s)</h4>';
    _canvasEl = document.createElement('canvas');
    _canvasEl.width = 300; _canvasEl.height = 80;
    _canvasCtx = _canvasEl.getContext('2d');
    trend.appendChild(_canvasEl);
    root.appendChild(trend);

    _refs.score       = root.querySelector('#hp-score');
    _refs.grade       = root.querySelector('#hp-grade');
    _refs.ringFg      = root.querySelector('#hp-ring-fg');
    _refs.batteryTime = root.querySelector('#hp-battery-time');

    return root;
  }

  function refresh(data) {
    const health = data || (StateManager ? StateManager.get('health') : null) || {};
    const subsys = health.subsystems || {};
    const keys = Object.keys(subsys);
    let total = 0;
    keys.forEach(function (k) { total += subsys[k] || 0; });
    const score = health.overall || (keys.length > 0 ? Math.round(total / keys.length) : 95);

    if (_refs.score) _refs.score.textContent = score;
    _updateRing(score);

    const grade = _computeGrade(score);
    if (_refs.grade) { _refs.grade.textContent = grade; _refs.grade.className = 'hp-score-grade hp-grade-' + grade; }

    ['battery','motors','comm','navigation'].forEach(function (id) {
      const v = subsys[id] != null ? subsys[id] : 95;
      const valEl = _refs['sub_' + id + '_val'];
      const barEl = _refs['sub_' + id + '_bar'];
      if (valEl) valEl.textContent = v + '%';
      if (barEl) { barEl.style.width = v + '%'; barEl.className = 'hp-subsys-fill ' + (v >= 75 ? 'ok' : v >= 50 ? 'warn' : 'bad'); }
    });

    const remaining = health.batteryRemaining != null ? health.batteryRemaining : 3600;
    const h = Math.floor(remaining / 3600);
    const m = Math.floor((remaining % 3600) / 60);
    const s = Math.floor(remaining % 60);
    const timeStr = String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');
    if (_refs.batteryTime) { _refs.batteryTime.textContent = timeStr; _refs.batteryTime.className = 'hp-battery-time' + (remaining < 600 ? ' low' : ''); }

    _pushTrend(score);
  }

  function _bindEvents() {
    if (StateManager) {
      _subscriptions.push(StateManager.subscribe('health', function (newVal) { refresh(newVal); }));
      refresh(StateManager.get('health'));
    }
    if (EventBus) {
      _eventUnsubs.push(EventBus.on('health:lowBattery:warning', function () { console.warn('[HealthPanel] Low battery!'); }));
    }
  }

  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[HealthPanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _bindEvents();
    console.log('[HealthPanel] Initialized in', containerId);
  }

  function destroy() {
    _subscriptions.forEach(function (fn) { if (typeof fn === 'function') fn(); });
    _subscriptions = [];
    _eventUnsubs.forEach(function (fn) { if (typeof fn === 'function') fn(); });
    _eventUnsubs = [];
    if (_rootEl && _rootEl.parentNode) _rootEl.parentNode.removeChild(_rootEl);
    _rootEl = null; _canvasEl = null; _canvasCtx = null;
    if (_styleEl && _styleEl.parentNode) _styleEl.parentNode.removeChild(_styleEl);
    _styleEl = null;
    console.log('[HealthPanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.health = { init: init, refresh: refresh, destroy: destroy };
})(window);
