/**
 * replay_panel.js — 回放面板
 * ============================================================================
 * 功能：飞行回放控制面板，提供时间轴滑块、播放/暂停/停止按钮、
 *       变速控制（0.5x / 1x / 2x / 4x）、当前时间与进度百分比显示。
 *       与 flight_demo_cesium.html 的 Demo Controls 逻辑对接。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   state:replay:changed         — 监视 replay 状态对象变化
 *   replay:play / replay:pause   — 播放/暂停控制
 *   replay:speed:changed         — 变速变化
 *   replay:seek                  — 时间轴拖动
 *
 * 暴露接口（挂载到 window.Panels.replay）：
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
  let _state = {
    playing: false,
    speed: 1.0,
    progress: 0,
  };

  // ── CSS 注入 ──────────────────────────────────────────────
  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'replay-panel-styles';
    _styleEl.textContent = `
      .rp-card {
        background: #101830; border: 1px solid #1a2a4a; border-radius: 6px;
        padding: 12px; margin-bottom: 8px;
        font-family: 'Segoe UI', 'Consolas', monospace; color: #c0d0f0;
      }
      .rp-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .rp-section { margin-top:10px; padding-top:8px; border-top:1px solid #182442; }

      /* ── 时间轴 ── */
      .rp-timeline { position:relative; padding:4px 0; }
      .rp-timeline input[type=range] { -webkit-appearance:none; width:100%; height:6px; border-radius:3px; background:#182442; outline:none; cursor:pointer; }
      .rp-timeline input[type=range]::-webkit-slider-thumb { -webkit-appearance:none; width:14px;height:14px; border-radius:50%; background:#4ea8ff; cursor:pointer; box-shadow:0 0 6px rgba(78,168,255,0.4); }
      .rp-timeline-labels { display:flex; justify-content:space-between; font-size:10px; color:#506a8e; margin-top:2px; }
      .rp-time-display { display:flex; justify-content:space-between; padding:4px 0; font-size:11px; }
      .rp-time-current { color:#4ef0a0; font-family:'Consolas',monospace; }
      .rp-time-total { color:#6a8aae; }

      /* ── 控制按钮 ── */
      .rp-controls { display:flex; gap:6px; margin:8px 0; }
      .rp-btn { flex:1; padding:8px 4px; border:1px solid #182442; border-radius:4px; background:#0d1428;
        color:#6a9fd8; font-size:12px; font-weight:bold; cursor:pointer; transition:all 0.2s; letter-spacing:1px; }
      .rp-btn:hover { background:#141e38; border-color:#2a4a6a; color:#c0d8f0; }
      .rp-btn:active { transform:scale(0.96); }
      .rp-btn.play { border-color:#1a3a2a; color:#4ef0a0; }
      .rp-btn.play:hover { background:rgba(78,240,160,0.08); }
      .rp-btn.play.active { background:rgba(78,240,160,0.12); }
      .rp-btn.pause { border-color:#3a2a0a; color:#f0a04e; }
      .rp-btn.pause:hover { background:rgba(240,160,78,0.08); }
      .rp-btn.stop { border-color:#3a1a1a; color:#f04e4e; }
      .rp-btn.stop:hover { background:rgba(240,78,78,0.08); }

      /* ── 变速选择 ── */
      .rp-speed-row { display:flex; gap:4px; }
      .rp-speed-btn { flex:1; padding:4px 6px; border:1px solid #182442; border-radius:3px; background:#0d1428;
        color:#6a8aae; font-size:10px; cursor:pointer; text-align:center; transition:all 0.2s; }
      .rp-speed-btn:hover { background:#141e38; border-color:#2a4a6a; color:#c0d8f0; }
      .rp-speed-btn.active { background:#1a2a4a; border-color:#4ea8ff; color:#4ea8ff; font-weight:bold; }
      .rp-speed-label { font-size:10px; color:#506a8e; margin:6px 0 4px 0; }
    `;
    document.head.appendChild(_styleEl);
  }

  // ── DOM 构建 ──────────────────────────────────────────────
  function _buildRoot() {
    var root = document.createElement('div');
    root.className = 'rp-card';
    root.id = 'replay-panel-root';
    root.innerHTML = '<h3>REPLAY</h3>';

    // 时间显示
    var timeDiv = document.createElement('div');
    timeDiv.className = 'rp-time-display';
    timeDiv.innerHTML =
      '<span class="rp-time-current" id="rp-time-curr">00:00:00</span>' +
      '<span class="rp-time-total" id="rp-time-total"> / 00:01:00</span>' +
      '<span id="rp-progress-pct" style="color:#4ea8ff;font-size:10px;">0%</span>';
    root.appendChild(timeDiv);

    // 时间轴滑块
    var timelineDiv = document.createElement('div');
    timelineDiv.className = 'rp-timeline';
    timelineDiv.innerHTML =
      '<input type="range" id="rp-timeline-slider" min="0" max="1000" value="0" step="1">' +
      '<div class="rp-timeline-labels"><span>START</span><span>END</span></div>';
    root.appendChild(timelineDiv);
    _refs.slider = timelineDiv.querySelector('#rp-timeline-slider');
    _refs.timeCurr = timeDiv.querySelector('#rp-time-curr');
    _refs.timeTotal = timeDiv.querySelector('#rp-time-total');
    _refs.progressPct = timeDiv.querySelector('#rp-progress-pct');

    // 控制按钮
    var ctrlDiv = document.createElement('div');
    ctrlDiv.className = 'rp-controls';
    ctrlDiv.id = 'rp-control-btns';
    ctrlDiv.innerHTML =
      '<button class="rp-btn play" id="rp-btn-play">PLAY</button>' +
      '<button class="rp-btn pause" id="rp-btn-pause">PAUSE</button>' +
      '<button class="rp-btn stop" id="rp-btn-stop">STOP</button>';
    root.appendChild(ctrlDiv);
    _refs.btnPlay  = ctrlDiv.querySelector('#rp-btn-play');
    _refs.btnPause = ctrlDiv.querySelector('#rp-btn-pause');
    _refs.btnStop  = ctrlDiv.querySelector('#rp-btn-stop');

    // 变速控制
    var speedSection = document.createElement('div');
    speedSection.className = 'rp-section';
    speedSection.innerHTML = '<span class="rp-speed-label">Speed</span>';
    var speedRow = document.createElement('div');
    speedRow.className = 'rp-speed-row';
    speedRow.id = 'rp-speed-row';
    speedSection.appendChild(speedRow);
    _refs.speedRow = speedRow;

    root.appendChild(speedSection);
    return root;
  }

  // ── 渲染变速按钮 ──────────────────────────────────────────
  function _renderSpeedButtons(activeSpeed) {
    if (!_refs.speedRow) return;
    _refs.speedRow.innerHTML = '';
    [0.5, 1.0, 2.0, 4.0].forEach(function (s) {
      var btn = document.createElement('button');
      btn.className = 'rp-speed-btn' + (s === activeSpeed ? ' active' : '');
      btn.textContent = s + 'x';
      btn.addEventListener('click', function () { _setSpeed(s); });
      _refs.speedRow.appendChild(btn);
    });
  }

  function _setSpeed(speed) {
    _state.speed = speed;
    _renderSpeedButtons(speed);
    if (EventBus) EventBus.emit('replay:speed:changed', speed);
    if (StateManager) StateManager.merge({ replay: Object.assign(StateManager.get('replay') || {}, { speed: speed }) });
  }

  // ── 时间/进度更新 ──────────────────────────────────────────
  function _updateDisplay() {
    var pct = _state.progress || 0;
    var sliderVal = Math.round(pct * 1000);
    if (_refs.slider && parseInt(_refs.slider.value) !== sliderVal) {
      _refs.slider.value = sliderVal;
    }
    if (_refs.progressPct) _refs.progressPct.textContent = (pct * 100).toFixed(0) + '%';

    // 根据进度换算时间（假设总时长 60s）
    var totalSec = 60;
    var currSec = pct * totalSec;
    var t = new Date(0);
    t.setSeconds(currSec);
    if (_refs.timeCurr) _refs.timeCurr.textContent = t.toISOString().substr(11, 8);

    if (_refs.timeTotal) {
      var tt = new Date(0);
      tt.setSeconds(totalSec);
      _refs.timeTotal.textContent = ' / ' + tt.toISOString().substr(11, 8);
    }
  }

  // ── 数据刷新 ──────────────────────────────────────────────
  function refresh(data) {
    var r = data || (StateManager ? StateManager.get('replay') : null) || {};
    if (r.progress !== undefined) _state.progress = r.progress;
    if (r.speed !== undefined) _state.speed = r.speed;
    if (r.playing !== undefined) _state.playing = r.playing;
    _updateDisplay();
    _renderSpeedButtons(_state.speed);
  }

  // ── 事件绑定 ──────────────────────────────────────────────
  function _bindEvents() {
    // 时间轴拖动
    if (_refs.slider) {
      _refs.slider.addEventListener('input', function () {
        var pct = parseInt(this.value) / 1000;
        _state.progress = pct;
        _updateDisplay();
        if (EventBus) EventBus.emit('replay:seek', pct);
        if (StateManager) StateManager.merge({ replay: Object.assign(StateManager.get('replay') || {}, { progress: pct }) });
      });
    }

    // 播放
    if (_refs.btnPlay) {
      _refs.btnPlay.addEventListener('click', function () {
        _state.playing = true;
        if (EventBus) EventBus.emit('replay:play');
        if (StateManager) StateManager.merge({ replay: Object.assign(StateManager.get('replay') || {}, { playing: true }) });
      });
    }

    // 暂停
    if (_refs.btnPause) {
      _refs.btnPause.addEventListener('click', function () {
        _state.playing = false;
        if (EventBus) EventBus.emit('replay:pause');
        if (StateManager) StateManager.merge({ replay: Object.assign(StateManager.get('replay') || {}, { playing: false }) });
      });
    }

    // 停止
    if (_refs.btnStop) {
      _refs.btnStop.addEventListener('click', function () {
        _state.playing = false;
        _state.progress = 0;
        _updateDisplay();
        if (EventBus) EventBus.emit('replay:stop');
        if (StateManager) {
          StateManager.merge({ replay: Object.assign(StateManager.get('replay') || {}, { playing: false, progress: 0 }) });
        }
      });
    }

    if (StateManager) {
      var unsub = StateManager.subscribe('replay', function (newVal) { refresh(newVal); });
      _subscriptions.push(unsub);
      var cur = StateManager.get('replay');
      if (cur) refresh(cur);
    }

    // 跨模块：监听 demo:start 等事件同步回放状态
    if (EventBus) {
      _eventUnsubs.push(EventBus.on('demo:start',  function () { _state.playing = true;  _updateDisplay(); }));
      _eventUnsubs.push(EventBus.on('demo:pause',  function () { _state.playing = false; _updateDisplay(); }));
      _eventUnsubs.push(EventBus.on('demo:resume', function () { _state.playing = true;  _updateDisplay(); }));
      _eventUnsubs.push(EventBus.on('demo:stop',   function () { _state.playing = false; _state.progress = 0; _updateDisplay(); }));
    }
  }

  // ── 公开接口 ──────────────────────────────────────────────
  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[ReplayPanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _renderSpeedButtons(1.0);
    _updateDisplay();
    _bindEvents();
    console.log('[ReplayPanel] Initialized in', containerId);
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
    console.log('[ReplayPanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.replay = { init: init, refresh: refresh, destroy: destroy };
})(window);
