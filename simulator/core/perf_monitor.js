/**
 * perf_monitor.js — 性能基准测试工具
 * ============================================================================
 * 功能：对 FlyteOS Simulator 进行运行时性能测量，覆盖以下维度：
 *   - EventBus 事件吞吐（每秒 emit 次数、平均延迟 P50/P99）
 *   - StateManager 状态读写延迟（set/get 操作耗时）
 *   - 渲染帧率 FPS（requestAnimationFrame 计数）
 *   - 内存使用（performance.memory API）
 *
 * 依赖：EventBus、StateManager（需先加载）
 *
 * 暴露接口（挂载到 window.PerfMonitor）：
 *   PerfMonitor.start(durationMs)  — 启动测试（默认 30000ms）
 *   PerfMonitor.stop()             — 提前停止
 *   PerfMonitor.report()           — 输出 JSON 报告到 console 和 window.__perfReport
 *   PerfMonitor.status()           — 获取当前运行状态
 *
 * 使用示例：
 *   PerfMonitor.start(30000);          // 运行 30 秒
 *   // 30 秒后自动调用 report()
 *   PerfMonitor.report();              // 手动输出报告
 *   const r = window.__perfReport;     // 获取 JSON 报告对象
 *
 * 输出报告字段：
 *   meta.duration_ms, meta.start_time, meta.end_time
 *   eventbus.throughput_rps, eventbus.latency_ms (min/avg/max/p99)
 *   statemanager.set_latency_us, statemanager.get_latency_us (min/avg/max/p99)
 *   render.fps (min/avg/max/p1), render.frame_count
 *   memory.used_heap_mb, memory.total_heap_mb, memory.limit_heap_mb (min/avg/max)
 *   mavlink.msg_per_sec (min/avg/max)
 * ============================================================================
 */
(function () {
  'use strict';

  // =========================================================================
  // 内部状态
  // =========================================================================
  let _running = false;
  let _startTime = 0;
  let _duration = 30000;
  let _timerId = null;
  let _fpsTimerId = null;

  // ── 采样缓冲区 ──
  let _eventLatencies = [];       // EventBus emit → 监听器回调 延迟 (ms)
  let _eventTimestamps = [];      // 每秒 emit 次数时间戳
  let _setLatencies = [];         // StateManager.set() 延迟 (ms)
  let _getLatencies = [];         // StateManager.get() 延迟 (ms)
  let _fpsSamples = [];           // 每秒 FPS 采样
  let _memSamples = [];           // 内存采样点
  let _mavlinkCounts = [];        // 每秒 MAVLink 消息计数

  // ── FPS 内部计数器 ──
  let _frameCount = 0;
  let _lastFpsSample = 0;
  let _fpsAccumulator = 0;

  // ── MAVLink 计数器 ──
  let _mavlinkThisSecond = 0;
  let _mavlinkLastSecond = 0;

  // ── 原始钩子引用 ──
  let _origEmit = null;
  let _origSet = null;
  let _origGet = null;

  // =========================================================================
  // 工具函数
  // =========================================================================
  function _now() {
    return performance.now();
  }

  function _nowUs() {
    return performance.now() * 1000; // ms → μs
  }

  function _percentile(sorted, pct) {
    if (!sorted || sorted.length === 0) return 0;
    const idx = Math.ceil(sorted.length * pct / 100) - 1;
    return sorted[Math.max(0, idx)];
  }

  function _stats(arr) {
    if (!arr || arr.length === 0) return { min: 0, avg: 0, max: 0, p99: 0 };
    const sorted = arr.slice().sort((a, b) => a - b);
    const sum = sorted.reduce((a, b) => a + b, 0);
    return {
      min: sorted[0],
      avg: sum / sorted.length,
      max: sorted[sorted.length - 1],
      p99: _percentile(sorted, 99),
    };
  }

  // =========================================================================
  // 内存采样
  // =========================================================================
  function _sampleMemory() {
    if (!performance.memory) {
      _memSamples.push({ time: _now(), used: 0, total: 0, limit: 0 });
      return;
    }
    const m = performance.memory;
    _memSamples.push({
      time: _now(),
      used: m.usedJSHeapSize / (1024 * 1024),
      total: m.totalJSHeapSize / (1024 * 1024),
      limit: m.jsHeapSizeLimit / (1024 * 1024),
    });
  }

  // =========================================================================
  // Hook 安装与卸载
  // =========================================================================
  function _installHooks() {
    // ── Hook EventBus.emit ──
    if (typeof EventBus !== 'undefined' && EventBus.emit) {
      _origEmit = EventBus.emit;
      EventBus.emit = function (event, payload) {
        const t0 = _now();

        // 记录 MAVLink 消息
        if (event === 'mavlink:message') {
          _mavlinkThisSecond++;
        }

        // 为 data:update 事件注入延迟测量回调
        if (event === 'data:update' && _running) {
          const result = _origEmit.call(this, event, payload);
          const latency = _now() - t0;
          _eventLatencies.push(latency);
          _eventTimestamps.push(t0);
          return result;
        }

        return _origEmit.call(this, event, payload);
      };
    }

    // ── Hook StateManager.set ──
    if (typeof StateManager !== 'undefined' && StateManager.set) {
      _origSet = StateManager.set;
      StateManager.set = function (key, value) {
        if (!_running) return _origSet.call(this, key, value);
        const t0 = _nowUs();
        const result = _origSet.call(this, key, value);
        _setLatencies.push((_nowUs() - t0) / 1000); // μs → ms
        return result;
      };
    }

    // ── Hook StateManager.get ──
    if (typeof StateManager !== 'undefined' && StateManager.get) {
      _origGet = StateManager.get;
      StateManager.get = function (key) {
        if (!_running) return _origGet.call(this, key);
        const t0 = _nowUs();
        const result = _origGet.call(this, key);
        _getLatencies.push((_nowUs() - t0) / 1000); // μs → ms
        return result;
      };
    }
  }

  function _uninstallHooks() {
    if (_origEmit && typeof EventBus !== 'undefined') {
      EventBus.emit = _origEmit;
      _origEmit = null;
    }
    if (_origSet && typeof StateManager !== 'undefined') {
      StateManager.set = _origSet;
      _origSet = null;
    }
    if (_origGet && typeof StateManager !== 'undefined') {
      StateManager.get = _origGet;
      _origGet = null;
    }
  }

  // =========================================================================
  // FPS 测量循环
  // =========================================================================
  function _fpsLoop() {
    if (!_running) return;
    _frameCount++;
    _fpsTimerId = requestAnimationFrame(_fpsLoop);
  }

  function _fpsSampler() {
    if (!_running) return;

    const now = _now();
    const elapsed = now - _lastFpsSample;
    const fps = _frameCount / (elapsed / 1000);

    _fpsSamples.push(fps);
    _frameCount = 0;
    _lastFpsSample = now;

    // MAVLink 速率采样
    _mavlinkCounts.push(_mavlinkThisSecond);
    _mavlinkThisSecond = 0;

    // 内存采样
    _sampleMemory();

    setTimeout(_fpsSampler, 1000);
  }

  // =========================================================================
  // 公开 API
  // =========================================================================
  function start(durationMs) {
    if (_running) {
      console.warn('[PerfMonitor] Already running. Stop first.');
      return;
    }

    _duration = durationMs || 30000;
    _running = true;
    _startTime = _now();

    // 清空采样缓冲区
    _eventLatencies = [];
    _eventTimestamps = [];
    _setLatencies = [];
    _getLatencies = [];
    _fpsSamples = [];
    _memSamples = [];
    _mavlinkCounts = [];
    _frameCount = 0;
    _lastFpsSample = _startTime;
    _mavlinkThisSecond = 0;

    // 安装监控钩子
    _installHooks();

    // 启动 FPS 循环
    _fpsTimerId = requestAnimationFrame(_fpsLoop);

    // 每秒采样 FPS / MAVLink / Memory
    setTimeout(_fpsSampler, 1000);

    // 自动停止定时器
    _timerId = setTimeout(function () {
      stop();
      report();
    }, _duration);

    window.__perfReport = null;

    console.log('[PerfMonitor] Test started. Duration: ' + _duration + 'ms');
  }

  function stop() {
    if (!_running) return;

    _running = false;

    if (_timerId) { clearTimeout(_timerId); _timerId = null; }
    if (_fpsTimerId) { cancelAnimationFrame(_fpsTimerId); _fpsTimerId = null; }

    _uninstallHooks();

    console.log('[PerfMonitor] Test stopped. Elapsed: ' + (_now() - _startTime).toFixed(0) + 'ms');
  }

  function status() {
    if (!_running) return { running: false, elapsed: 0, duration: _duration };
    return { running: true, elapsed: _now() - _startTime, duration: _duration };
  }

  function report() {
    const endTime = _now();
    const actualDuration = endTime - _startTime;

    // ── 计算 EventBus 吞吐 ──
    const totalEvents = _eventTimestamps.length;
    const throughputRps = actualDuration > 0 ? (totalEvents / (actualDuration / 1000)) : 0;

    // ── 构建报告对象 ──
    const reportObj = {
      meta: {
        test_name: 'M1-09 Performance Baseline',
        test_date: new Date().toISOString(),
        duration_ms: Math.round(actualDuration),
        start_time: new Date(_startTime).toISOString(),
        end_time: new Date(endTime).toISOString(),
      },

      eventbus: {
        total_events: totalEvents,
        throughput_rps: Number(throughputRps.toFixed(2)),
        latency_ms: _stats(_eventLatencies),
      },

      statemanager: {
        set_count: _setLatencies.length,
        get_count: _getLatencies.length,
        set_latency_ms: _stats(_setLatencies),
        get_latency_ms: _stats(_getLatencies),
      },

      render: {
        frame_count: _fpsSamples.reduce((a, b) => a + b, 0),
        fps: _stats(_fpsSamples),
        fps_p1: _percentile(_fpsSamples.slice().sort((a, b) => a - b), 1),
      },

      memory: {
        samples: _memSamples.length,
        used_heap_mb: _stats(_memSamples.map(function (s) { return s.used; })),
        total_heap_mb: _stats(_memSamples.map(function (s) { return s.total; })),
        limit_heap_mb: _memSamples.length > 0 ? _memSamples[0].limit : 0,
      },

      mavlink: {
        msg_per_sec: _stats(_mavlinkCounts),
        total_messages: _mavlinkCounts.reduce(function (a, b) { return a + b; }, 0),
      },

      thresholds: {
        fps_30: (function () {
          const s = _stats(_fpsSamples);
          return s.avg >= 30 ? 'PASS' : 'FAIL';
        })(),
        mavlink_10: (function () {
          const s = _stats(_mavlinkCounts);
          return s.avg >= 10 ? 'PASS' : 'FAIL';
        })(),
      },
    };

    // 保存到全局变量
    window.__perfReport = reportObj;

    // 输出到控制台
    console.log('========================================');
    console.log('  FlyteOS Simulator — Performance Report');
    console.log('========================================');
    console.log(JSON.stringify(reportObj, null, 2));
    console.log('========================================');
    console.log('  Report also available at window.__perfReport');
    console.log('========================================');

    return reportObj;
  }

  // =========================================================================
  // 导出
  // =========================================================================
  window.PerfMonitor = {
    start: start,
    stop: stop,
    report: report,
    status: status,
  };

  console.log('[PerfMonitor] Loaded. API: PerfMonitor.start(ms) / stop() / report() / status()');
})();
