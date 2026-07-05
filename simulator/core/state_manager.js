/**
 * state_manager.js — 全局状态管理器
 * ============================================================================
 * 功能：集中式响应状态管理，单例模式。提供 get/set/subscribe/watch，
 *       自动维护最近 50 个状态快照用于调试/回滚。
 * 依赖：event_bus.js（需先加载，通过 EventBus 发布 state:changed 事件）
 *
 * 暴露接口：
 *   StateManager.get(key?)           → 获取整个 state 或指定 key 的值
 *   StateManager.set(key, value)     → 设置单个 key 的值
 *   StateManager.merge(partial)      → 合并多个 key
 *   StateManager.subscribe(key, fn)  → 订阅 key 变化（返回取消函数）
 *   StateManager.watch(fn)           → 监听所有变化（返回取消函数）
 *   StateManager.getSnapshot(index?) → 获取快照（默认最新，-1 上一步）
 *   StateManager.rollback(steps?)    → 回滚到指定步数前的状态
 *   StateManager.reset()             → 重置为空状态
 *
 * 使用示例：
 *   StateManager.set('flight', { lat: 31.23, lng: 121.47 });
 *   StateManager.subscribe('flight', (newVal, oldVal) => { ... });
 *   StateManager.watch((key, newVal, oldVal) => { ... });
 * ============================================================================
 */

const StateManager = (() => {
  /** @type {Record<string, any>} 当前状态 */
  let _state = {};

  /** @type {Array<Record<string, any>>} 状态快照栈（最近 50 个） */
  const _snapshots = [];

  /** 快照最大保留数 */
  const MAX_SNAPSHOTS = 50;

  /** @type {Map<string, Array<Function>>} 按 key 分的订阅者 */
  const _subscribers = new Map();

  /** @type {Array<Function>} 全局监听器（监听所有变化） */
  const _watchers = [];

  /**
   * 保存当前状态快照到栈
   */
  function _saveSnapshot() {
    _snapshots.push(JSON.parse(JSON.stringify(_state)));
    if (_snapshots.length > MAX_SNAPSHOTS) {
      _snapshots.shift();
    }
  }

  /**
   * 通知订阅者
   * @param {string} key     变化的 key
   * @param {*}      newVal  新值
   * @param {*}      oldVal  旧值
   */
  function _notify(key, newVal, oldVal) {
    // 通知 key 级订阅者
    const keySubs = _subscribers.get(key);
    if (keySubs) {
      keySubs.slice().forEach(fn => {
        try { fn(newVal, oldVal); } catch (e) { console.error('[StateManager] subscriber error:', e); }
      });
    }
    // 通知全局 watcher
    _watchers.slice().forEach(fn => {
      try { fn(key, newVal, oldVal); } catch (e) { console.error('[StateManager] watcher error:', e); }
    });

    // 通过 EventBus 广播（如果已加载）
    if (typeof EventBus !== 'undefined') {
      EventBus.emit('state:changed', { key, newVal, oldVal, state: _state });
      EventBus.emit(`state:${key}:changed`, { newVal, oldVal });
    }
  }

  /**
   * 获取状态
   * @param {string} [key] 可选，不传返回整个 state 对象
   * @returns {*}
   */
  function get(key) {
    if (key === undefined) {
      return { ..._state };
    }
    return _state[key];
  }

  /**
   * 设置单个 key 的值（会触发通知 + 快照）
   * @param {string} key
   * @param {*}      value
   */
  function set(key, value) {
    if (typeof key !== 'string' || key === '') {
      console.warn('[StateManager] set() requires a non-empty string key');
      return;
    }
    _saveSnapshot();
    const oldVal = _state[key];
    _state[key] = value;
    _notify(key, value, oldVal);
  }

  /**
   * 合并多个 key/value（一次快照，按 key 逐个通知）
   * @param {Record<string, any>} partial
   */
  function merge(partial) {
    if (!partial || typeof partial !== 'object') return;
    const keys = Object.keys(partial);
    if (keys.length === 0) return;

    _saveSnapshot();
    for (const key of keys) {
      const oldVal = _state[key];
      const newVal = partial[key];
      _state[key] = newVal;
      _notify(key, newVal, oldVal);
    }
  }

  /**
   * 订阅指定 key 的变化
   * @param {string}   key
   * @param {Function} callback(newVal, oldVal)
   * @returns {Function} 取消订阅函数
   */
  function subscribe(key, callback) {
    if (!_subscribers.has(key)) {
      _subscribers.set(key, []);
    }
    _subscribers.get(key).push(callback);
    return () => {
      const arr = _subscribers.get(key);
      if (arr) {
        const idx = arr.indexOf(callback);
        if (idx !== -1) arr.splice(idx, 1);
      }
    };
  }

  /**
   * 全局监听器（任一 key 变化都触发）
   * @param {Function} fn(key, newVal, oldVal)
   * @returns {Function} 取消监听函数
   */
  function watch(fn) {
    _watchers.push(fn);
    return () => {
      const idx = _watchers.indexOf(fn);
      if (idx !== -1) _watchers.splice(idx, 1);
    };
  }

  /**
   * 获取历史快照
   * @param {number} [index] 不传返回最新快照，-1 返回上一步，-2 更早
   * @returns {Record<string, any>|null}
   */
  function getSnapshot(index) {
    if (_snapshots.length === 0) return null;

    if (index === undefined) {
      return JSON.parse(JSON.stringify(_snapshots[_snapshots.length - 1]));
    }
    const realIdx = index < 0
      ? _snapshots.length + index
      : index;
    if (realIdx < 0 || realIdx >= _snapshots.length) return null;
    return JSON.parse(JSON.stringify(_snapshots[realIdx]));
  }

  /**
   * 回滚到指定步数前的状态
   * @param {number} [steps=1] 回退步数（默认 1）
   * @returns {boolean} 是否回滚成功
   */
  function rollback(steps = 1) {
    if (_snapshots.length < steps) return false;

    // 弹出最近 steps 个快照
    for (let i = 0; i < steps; i++) {
      _snapshots.pop();
    }
    // 恢复为最新快照，或空状态
    if (_snapshots.length > 0) {
      _state = JSON.parse(JSON.stringify(_snapshots[_snapshots.length - 1]));
    } else {
      _state = {};
    }
    // 全局广播重置
    if (typeof EventBus !== 'undefined') {
      EventBus.emit('state:rollback', { steps, state: _state });
    }
    return true;
  }

  /**
   * 完全重置状态和快照
   */
  function reset() {
    _state = {};
    _snapshots.length = 0;
    if (typeof EventBus !== 'undefined') {
      EventBus.emit('state:reset');
    }
  }

  /**
   * 获取快照数量（调试用）
   */
  function snapshotCount() {
    return _snapshots.length;
  }

  // 挂载到全局
  if (typeof window !== 'undefined') {
    window.StateManager = StateManager;
  }

  return {
    get,
    set,
    merge,
    subscribe,
    watch,
    getSnapshot,
    rollback,
    reset,
    snapshotCount,
  };
})();
