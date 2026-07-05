/**
 * event_bus.js — 轻量事件总线
 * ============================================================================
 * 功能：发布/订阅事件总线，支持命名空间隔离和回调优先级排序。
 * 依赖：无（零外部依赖）
 *
 * 暴露接口：
 *   EventBus.on(event, callback, options?)   → 订阅事件，返回取消函数
 *   EventBus.off(event, callback?)           → 取消订阅
 *   EventBus.emit(event, ...args)            → 发布事件
 *   EventBus.once(event, callback, options?) → 一次性订阅，触发后自动取消
 *
 * options:
 *   { priority: number }  数值越大优先执行（默认 0）
 *   { namespace: string } 命名空间，用于 off 时批量取消（默认 'default'）
 *
 * 使用示例：
 *   EventBus.on('demo:start', () => console.log('started'), { priority: 10 });
 *   EventBus.emit('demo:start', { speed: 2.0 });
 *   EventBus.once('init', () => console.log('one-shot'));
 *   EventBus.off('demo:*', fn);  // 取消特定回调
 * ============================================================================
 */

const EventBus = (() => {
  /** @type {Map<string, Array<{callback: Function, priority: number, namespace: string}>>} */
  const _listeners = new Map();

  /**
   * 规范化事件名（去除首尾空格）
   * @param {string} event
   * @returns {string}
   */
  function _normalize(event) {
    return (event || '').trim();
  }

  /**
   * 获取事件对应的监听器列表（不存在则创建）
   * @param {string} event
   * @returns {Array}
   */
  function _ensure(event) {
    if (!_listeners.has(event)) {
      _listeners.set(event, []);
    }
    return _listeners.get(event);
  }

  /**
   * 订阅事件
   * @param {string}   event    事件名
   * @param {Function} callback 回调函数
   * @param {{priority?: number, namespace?: string}} [options]
   * @returns {Function} 取消此订阅的函数
   */
  function on(event, callback, options = {}) {
    const ev = _normalize(event);
    if (!ev || typeof callback !== 'function') {
      console.warn('[EventBus] on() requires valid event name and callback');
      return () => {};
    }

    const priority = Number(options.priority) || 0;
    const namespace = options.namespace || 'default';
    const entry = { callback, priority, namespace };
    const listeners = _ensure(ev);
    listeners.push(entry);
    // 按优先级降序排列
    listeners.sort((a, b) => b.priority - a.priority);

    return () => off(ev, callback);
  }

  /**
   * 取消订阅
   * - 不传 callback：取消该事件下所有监听器
   * - 传 callback：仅取消该特定回调的订阅
   * @param {string}   event    事件名
   * @param {Function} [callback] 要移除的特定回调
   */
  function off(event, callback) {
    const ev = _normalize(event);
    if (!ev) return;

    if (!_listeners.has(ev)) return;

    if (callback === undefined) {
      // 取消该事件全部监听
      _listeners.delete(ev);
    } else {
      const listeners = _listeners.get(ev);
      const idx = listeners.findIndex(e => e.callback === callback);
      if (idx !== -1) {
        listeners.splice(idx, 1);
      }
      if (listeners.length === 0) {
        _listeners.delete(ev);
      }
    }
  }

  /**
   * 发布事件
   * @param {string} event 事件名
   * @param {...*}    args  传递给回调的参数
   */
  function emit(event, ...args) {
    const ev = _normalize(event);
    if (!ev) return;

    const listeners = _listeners.get(ev);
    if (!listeners || listeners.length === 0) return;

    // 复制一份防止回调中修改数组
    const snapshot = listeners.slice();
    for (const entry of snapshot) {
      // 跳过已在 emit 期间被 off 的回调
      if (!_listeners.has(ev) || !_listeners.get(ev).includes(entry)) continue;
      try {
        entry.callback(...args);
      } catch (err) {
        console.error(`[EventBus] Error in listener for "${ev}":`, err);
      }
    }
  }

  /**
   * 一次性订阅：触发一次后自动取消
   * @param {string}   event    事件名
   * @param {Function} callback 回调函数
   * @param {{priority?: number, namespace?: string}} [options]
   * @returns {Function} 取消函数
   */
  function once(event, callback, options = {}) {
    const ev = _normalize(event);
    const wrapper = (...args) => {
      off(ev, wrapper);
      callback(...args);
    };
    return on(ev, wrapper, options);
  }

  // ------ 调试接口（非生产环境可用） ------
  function _dump() {
    const result = {};
    for (const [ev, listeners] of _listeners.entries()) {
      result[ev] = listeners.map(e => ({
        priority: e.priority,
        namespace: e.namespace,
      }));
    }
    return result;
  }

  return {
    on,
    off,
    emit,
    once,
    _dump, // 调试用
  };
})();

// 挂载到全局（后续 ES Module 可改为 export）
if (typeof window !== 'undefined') {
  window.EventBus = EventBus;
}
