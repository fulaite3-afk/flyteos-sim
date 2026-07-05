/**
 * data_simulator.js — 数据仿真引擎
 * ============================================================================
 * 功能：模拟真实飞控数据流，通过 EventBus 广播 + StateManager 批量更新
 *       向所有面板推送模拟传感器数据，支持故障注入。
 *
 * 依赖：event_bus.js、state_manager.js（需先加载）
 *
 * 数据通道（8 路）：
 *   GPS       — 经纬度/高度/速度/卫星数/定位精度
 *   IMU       — 加速度(xyz)/角速度(xyz)/温度
 *   气压计    — 气压值/推算高度/温度漂移
 *   磁罗盘    — 航向角/磁场强度/校准状态
 *   MAVLink   — 每秒 5 条心跳消息（系统ID/组件ID/消息类型）
 *   姿态      — Roll/Pitch/Yaw（正弦轨迹 + 高斯噪声）
 *   电池      — 电压/电流/剩余容量%/预估剩余时间
 *   链路      — RSSI/延迟/丢包率
 *
 * 暴露接口（挂载到 window.DataSimulator）：
 *   DataSimulator.start()           — 启动数据流（默认 5Hz）
 *   DataSimulator.stop()            — 停止数据流
 *   DataSimulator.setRate(Hz)       — 动态调整更新速率
 *   DataSimulator.injectFault(type, severity) — 注入故障
 *   DataSimulator.clearFault(type)  — 清除故障
 *   DataSimulator.getState()        — 获取当前模拟状态（调试用）
 *
 * 使用示例：
 *   DataSimulator.start();
 *   DataSimulator.setRate(10);
 *   DataSimulator.injectFault('battery', 0.7);
 *   DataSimulator.stop();
 * ============================================================================
 */

const DataSimulator = (() => {
  'use strict';

  // ============================================================
  //  内部状态
  // ============================================================

  /** 仿真时钟（秒） */
  let _t = 0;

  /** 定时器 ID */
  let _timerId = null;

  /** 当前更新间隔（毫秒），默认 200ms = 5Hz */
  let _interval = 200;

  /** 运行标志 */
  let _running = false;

  /** MAVLink 消息序号 */
  let _mavSeq = 0;

  /** 已激活的故障：{ type: severity (0~1) } */
  const _faults = {};

  // ============================================================
  //  高斯噪声生成器
  // ============================================================

  /**
   * Box-Muller 方法生成标准正态分布随机数
   * @returns {number} ~N(0,1)
   */
  function _gaussian() {
    let u = 0, v = 0;
    while (u === 0) u = Math.random();
    while (v === 0) v = Math.random();
    return Math.sqrt(-2.0 * Math.log(u)) * Math.cos(2.0 * Math.PI * v);
  }

  /**
   * 带噪声的值：base + base * noiseLevel * gaussian()
   * noiseLevel 为噪声相对振幅（如 0.005 表示 ±0.5% 1σ）
   */
  function _noisy(base, noiseLevel) {
    return base + base * noiseLevel * _gaussian();
  }

  /**
   * 钳位
   */
  function _clamp(val, min, max) {
    return Math.max(min, Math.min(max, val));
  }

  // ============================================================
  //  各传感器模拟函数（返回对象，由主循环调用）
  // ============================================================

  /**
   * GPS 数据
   * 基准：SF 湾区 (37.7, -122.4)
   */
  function _simulateGPS(t) {
    // 模拟绕圈飞行（半径 ~0.002° ≈ 200m）
    const lat = 37.7 + 0.002 * Math.sin(t * 0.05);
    const lng = -122.4 + 0.002 * Math.cos(t * 0.05);
    const alt = _noisy(500 + 30 * Math.sin(t * 0.03), 0.005);
    const speed = _noisy(25 + 5 * Math.sin(t * 0.08), 0.01);
    const sats = _clamp(Math.floor(_noisy(14, 0.02)), 6, 20);
    const hdop = _noisy(1.2, 0.01);

    // 故障影响
    const gpsFault = _faults.gps || 0;
    const acc = _noisy(hdop * (1 + gpsFault * 5), 0.01);

    return {
      lat: +lat.toFixed(7),
      lng: +lng.toFixed(7),
      alt: +alt.toFixed(1),
      speed: +speed.toFixed(1),
      sats,
      hdop: +hdop.toFixed(2),
      accuracy: +acc.toFixed(2),
      fixType: gpsFault > 0.5 ? 1 : 3, // 3=3D fix, 1=no fix
    };
  }

  /**
   * IMU 数据
   */
  function _simulateIMU(t) {
    const ax = _noisy(0.05 * Math.sin(t * 0.3), 0.015);
    const ay = _noisy(0.03 * Math.cos(t * 0.25), 0.015);
    const az = _noisy(-9.81 + 0.1 * Math.sin(t * 0.1), 0.008);

    const gx = _noisy(0.02 * Math.sin(t * 0.4), 0.02);
    const gy = _noisy(0.015 * Math.cos(t * 0.35), 0.02);
    const gz = _noisy(0.01 * Math.sin(t * 0.5), 0.02);

    const temp = _noisy(38 + 2 * Math.sin(t * 0.01), 0.005);

    const imuFault = _faults.imu || 0;

    return {
      accX: +(ax * (1 + imuFault * 2)).toFixed(4),
      accY: +(ay * (1 + imuFault * 2)).toFixed(4),
      accZ: +(az * (1 + imuFault * 2)).toFixed(4),
      gyroX: +(gx * (1 + imuFault * 2)).toFixed(4),
      gyroY: +(gy * (1 + imuFault * 2)).toFixed(4),
      gyroZ: +(gz * (1 + imuFault * 2)).toFixed(4),
      temp: +temp.toFixed(1),
    };
  }

  /**
   * 气压计数据
   */
  function _simulateBarometer(t) {
    const pressure = _noisy(1013.25 - 0.12 * Math.sin(t * 0.03), 0.002);
    const baroAlt = _noisy(500 + 30 * Math.sin(t * 0.03), 0.01);
    const tempDrift = _noisy(0.0, 0.01) + 0.02 * Math.sin(t * 0.005);

    const baroFault = _faults.baro || 0;

    return {
      pressure: +(pressure * (1 - baroFault * 0.3)).toFixed(2),
      altitude: +(baroAlt * (1 + baroFault * 3)).toFixed(1),
      tempDrift: +(tempDrift * (1 + baroFault * 5)).toFixed(3),
      temperature: +(25 + 0.5 * Math.sin(t * 0.01)).toFixed(1),
    };
  }

  /**
   * 磁罗盘数据
   */
  function _simulateMagnetometer(t) {
    const heading = _noisy(90 + 30 * Math.sin(t * 0.02), 0.01);
    const fieldX = _noisy(24.5, 0.01);
    const fieldY = _noisy(0.0, 0.01);
    const fieldZ = _noisy(45.0, 0.01);

    const magFault = _faults.mag || 0;
    const calibrated = magFault > 0.3;

    return {
      heading: +(_clamp(heading, 0, 360)).toFixed(1),
      fieldX: +(fieldX * (1 + magFault * 1.5)).toFixed(3),
      fieldY: +(fieldY * (1 + magFault * 1.5)).toFixed(3),
      fieldZ: +(fieldZ * (1 + magFault * 1.5)).toFixed(3),
      fieldStrength: +Math.sqrt(fieldX*fieldX + fieldY*fieldY + fieldZ*fieldZ).toFixed(2),
      calibrated,
    };
  }

  /**
   * 姿态数据（Roll/Pitch/Yaw）
   * 模拟轻微波动 + 正弦轨迹
   */
  function _simulateAttitude(t) {
    const roll = _noisy(3 * Math.sin(t * 0.15), 0.015);
    const pitch = _noisy(2 * Math.cos(t * 0.12), 0.015);
    const yaw = _noisy(90 + 30 * Math.sin(t * 0.02), 0.01);

    return {
      roll: +roll.toFixed(2),
      pitch: +pitch.toFixed(2),
      yaw: +yaw.toFixed(2),
    };
  }

  /**
   * MAVLink 心跳消息
   */
  function _simulateMAVLink(t) {
    _mavSeq = (_mavSeq + 1) % 256;

    const messages = [];
    // 每秒 5 条消息 → 每次 tick 发 1 条
    const types = ['HEARTBEAT', 'SYS_STATUS', 'GPS_RAW_INT', 'ATTITUDE', 'GLOBAL_POSITION_INT'];
    const msgType = types[_mavSeq % 5];

    messages.push({
      seq: _mavSeq,
      sysId: 1,
      compId: 1,
      msgType,
      timestamp: +(t * 1000).toFixed(0),
      payload: {
        type: msgType === 'HEARTBEAT' ? 2 : 0, // 2=MAV_TYPE_QUADROTOR
        autopilot: 12, // PX4
        baseMode: 81,
        customMode: 0,
        systemStatus: _faults.mavlink ? 4 : 0, // 4=CRITICAL, 0=OK
      },
    });

    return messages;
  }

  /**
   * 电池数据
   */
  function _simulateBattery(t) {
    // 模拟电池在 3500s 内从 100% 放电到 0%
    const elapsed = t % 3600;
    const remainingPct = _clamp(100 - (elapsed / 3600) * 100, 0, 100);
    const voltage = _noisy(11.1 + (remainingPct / 100) * 1.5, 0.005); // 3S LiPo: 11.1~12.6V
    const current = _noisy(remainingPct > 10 ? 8 + 2 * Math.sin(t * 0.1) : 12, 0.02);
    const estTime = remainingPct > 0 ? (remainingPct / 100) * 3600 - elapsed : 0;

    const batFault = _faults.battery || 0;
    const faultyRemaining = remainingPct * (1 - batFault * 0.8);

    return {
      voltage: +voltage.toFixed(2),
      current: +current.toFixed(2),
      remainingPct: +faultyRemaining.toFixed(1),
      estTimeRemaining: +Math.max(0, estTime * (1 - batFault)).toFixed(0),
      cells: [
        +_noisy(voltage / 3, 0.002).toFixed(2),
        +_noisy(voltage / 3, 0.002).toFixed(2),
        +_noisy(voltage / 3, 0.002).toFixed(2),
      ],
      temperature: +_noisy(35 + 0.5 * (1 - remainingPct / 100) * 10, 0.005).toFixed(1),
    };
  }

  /**
   * 链路数据
   */
  function _simulateLink(t) {
    const baseRSSI = -55;
    const linkFault = _faults.link || 0;

    const rssi = _noisy(baseRSSI - linkFault * 40, 0.015);
    const latency = _noisy(15 + linkFault * 200, 0.02);
    const packetLoss = _noisy(0.01 + linkFault * 0.4, 0.02);

    return {
      rssi: +rssi.toFixed(1),
      latency: +latency.toFixed(1),
      packetLoss: +_clamp(packetLoss, 0, 1).toFixed(4),
      signalQuality: +_clamp((rssi + 100) / 60, 0, 1).toFixed(3), // 归一化 0~1
    };
  }

  // ============================================================
  //  主循环：聚合所有传感器数据 → StateManager + EventBus
  // ============================================================

  function _tick() {
    if (!_running) return;

    _t += _interval / 1000;

    // 1. 模拟所有传感器
    const gps = _simulateGPS(_t);
    const imu = _simulateIMU(_t);
    const baro = _simulateBarometer(_t);
    const mag = _simulateMagnetometer(_t);
    const attitude = _simulateAttitude(_t);
    const mavlink = _simulateMAVLink(_t);
    const battery = _simulateBattery(_t);
    const link = _simulateLink(_t);

    // 2. 构建数据帧
    const frame = {
      timestamp: +(_t * 1000).toFixed(0),
      phase: _running ? 'CRUISE' : 'IDLE',
      gps,
      imu,
      barometer: baro,
      magnetometer: mag,
      attitude,
      battery,
      link,
      mavlink,
    };

    // 3. 通过 StateManager 批量更新
    if (typeof StateManager !== 'undefined') {
      // sensors 分支
      StateManager.set('sensors', {
        IMU: _clamp(Math.floor(450 + imu.gyroX * 100), 0, 900),
        BARO: +baro.pressure.toFixed(0),
        GPS: gps.sats,
        MAG: +mag.fieldStrength.toFixed(0),
      });

      // flight 分支
      StateManager.set('flight', {
        lat: gps.lat,
        lng: gps.lng,
        alt: gps.alt,
        spd: gps.speed,
        hdg: attitude.yaw,
        vertRate: +(imu.accZ * 0.3).toFixed(1),
        phase: frame.phase,
        wpLabel: 'WP3 (SF Bay)',
        gfStatus: 'safe',
        gpsSats: gps.sats,
        attitude: { roll: attitude.roll, pitch: attitude.pitch, yaw: attitude.yaw },
      });

      // health 分支
      const batteryHealth = _clamp(battery.remainingPct, 0, 100);
      const commHealth = _clamp(link.signalQuality * 100, 0, 100);
      const navHealth = _clamp(gps.fixType >= 3 ? 95 : 60, 0, 100);
      const motorHealth = _clamp(88 + imu.accZ * 0.5, 0, 100);

      const overall = +(
        0.4 * batteryHealth +
        0.25 * commHealth +
        0.2 * navHealth +
        0.15 * motorHealth
      ).toFixed(0);

      StateManager.set('health', {
        overall,
        subsystems: {
          battery: Math.floor(batteryHealth),
          motors: Math.floor(motorHealth),
          comm: Math.floor(commHealth),
          navigation: Math.floor(navHealth),
        },
        batteryRemaining: battery.estTimeRemaining,
      });
    }

    // 4. 通过 EventBus 广播数据帧
    if (typeof EventBus !== 'undefined') {
      EventBus.emit('data:update', frame);

      // MAVLink 消息逐条广播
      if (mavlink && mavlink.length > 0) {
        mavlink.forEach(msg => {
          EventBus.emit('mavlink:message', msg);
        });
      }
    }
  }

  // ============================================================
  //  公开 API
  // ============================================================

  /**
   * 启动数据流
   * @param {number} [hz=5] 更新频率（Hz），默认 5Hz
   */
  function start(hz) {
    if (_running) {
      console.warn('[DataSimulator] Already running, stop first.');
      return;
    }
    if (hz && hz > 0) {
      _interval = Math.floor(1000 / hz);
    }

    _running = true;
    _t = 0;
    _mavSeq = 0;

    // 立即发第一帧
    _tick();

    _timerId = setInterval(_tick, _interval);

    console.log(`[DataSimulator] Started at ${(1000/_interval).toFixed(1)}Hz (interval=${_interval}ms)`);

    if (typeof EventBus !== 'undefined') {
      EventBus.emit('simulator:started', { rate: 1000 / _interval });
    }
  }

  /**
   * 停止数据流
   */
  function stop() {
    if (!_running) return;

    _running = false;
    if (_timerId !== null) {
      clearInterval(_timerId);
      _timerId = null;
    }

    console.log('[DataSimulator] Stopped.');

    if (typeof EventBus !== 'undefined') {
      EventBus.emit('simulator:stopped');
    }

    // 更新状态为 IDLE
    if (typeof StateManager !== 'undefined') {
      StateManager.set('flight', Object.assign(
        StateManager.get('flight') || {},
        { phase: 'STOPPED' }
      ));
    }
  }

  /**
   * 动态调整更新频率
   * @param {number} hz 目标频率（Hz），如 5、10、20
   */
  function setRate(hz) {
    if (!hz || hz <= 0) {
      console.warn('[DataSimulator] setRate() requires positive Hz value.');
      return;
    }
    _interval = Math.floor(1000 / hz);
    if (_running && _timerId !== null) {
      clearInterval(_timerId);
      _timerId = setInterval(_tick, _interval);
    }
    console.log(`[DataSimulator] Rate set to ${hz}Hz (interval=${_interval}ms)`);
  }

  /**
   * 注入故障
   * @param {string}  type     故障类型：gps / imu / baro / mag / battery / link / mavlink
   * @param {number}  severity 严重程度 0~1（0=正常，1=完全故障）
   */
  function injectFault(type, severity) {
    const validTypes = ['gps', 'imu', 'baro', 'mag', 'battery', 'link', 'mavlink'];
    if (!validTypes.includes(type)) {
      console.warn(`[DataSimulator] Unknown fault type: "${type}". Valid: ${validTypes.join(', ')}`);
      return;
    }

    const s = _clamp(severity, 0, 1);
    _faults[type] = s;

    console.log(`[DataSimulator] Fault injected: ${type} severity=${s.toFixed(2)}`);

    if (typeof EventBus !== 'undefined') {
      EventBus.emit('fault:injected', { type, severity: s, timestamp: Date.now() });
    }
  }

  /**
   * 清除故障
   * @param {string} [type] 故障类型，不传则清除全部
   */
  function clearFault(type) {
    if (type) {
      delete _faults[type];
      console.log(`[DataSimulator] Fault cleared: ${type}`);
    } else {
      Object.keys(_faults).forEach(k => delete _faults[k]);
      console.log('[DataSimulator] All faults cleared.');
    }

    if (typeof EventBus !== 'undefined') {
      EventBus.emit('fault:cleared', { type: type || 'all', timestamp: Date.now() });
    }
  }

  /**
   * 获取当前模拟器内部状态（调试用）
   * @returns {{ running: boolean, interval: number, t: number, rate: number, faults: object }}
   */
  function getState() {
    return {
      running: _running,
      interval: _interval,
      t: _t,
      rate: +(1000 / _interval).toFixed(1),
      faults: { ..._faults },
    };
  }

  // ============================================================
  //  挂载到全局
  // ============================================================

  if (typeof window !== 'undefined') {
    window.DataSimulator = {
      start,
      stop,
      setRate,
      injectFault,
      clearFault,
      getState,
    };
  }

  return { start, stop, setRate, injectFault, clearFault, getState };
})();
