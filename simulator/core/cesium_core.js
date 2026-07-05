/**
 * cesium_core.js — Cesium Viewer 单例封装
 * ============================================================================
 * 功能：封装 Cesium.Viewer 的创建与常用操作，单例模式。
 *       依赖 Cesium 1.120+ 全局变量（通过 CDN <script> 引入）。
 * 依赖：event_bus.js、state_manager.js（可选，初始化后自动注入 viewer 到 StateManager）
 *
 * 暴露接口：
 *   CesiumCore.initialize(containerId, options?)       → 创建/获取 Viewer 单例
 *   CesiumCore.getViewer()                              → 获取 Viewer 实例
 *   CesiumCore.addAircraft(id, position, orientation?)  → 创建飞行器模型实体，返回 Entity
 *   CesiumCore.updatePosition(entityId, position)       → 更新飞行器位置/朝向
 *   CesiumCore.setCamera(position, orientation?)        → 调整相机视角
 *   CesiumCore.flyTo(position, duration?)               → 平滑飞向目标点
 *   CesiumCore.addTrail(entityId)                       → 为实体添加尾迹 polyline
 *   CesiumCore.removeEntity(entityId)                   → 移除实体
 *   CesiumCore.destroy()                                → 销毁 Viewer（仅测试用）
 *
 * options（initialize 第二个参数）:
 *   { terrainProvider?, imageryProvider?, homePosition?, animation?, timeline? }
 *
 * 使用示例：
 *   CesiumCore.initialize('cesiumContainer', {
 *     terrainProvider: Cesium.createWorldTerrain(),
 *   });
 *   const ac = CesiumCore.addAircraft('UAV-1', { lng: 121, lat: 31, alt: 500 });
 *   CesiumCore.updatePosition('UAV-1', { lng: 121.5, lat: 31.2, alt: 800, heading: 90 });
 * ============================================================================
 */

const CesiumCore = (() => {
  /** @type {Cesium.Viewer|null} */
  let _viewer = null;

  /** @type {Record<string, Cesium.Entity>} 按 ID 索引的 Entity 缓存 */
  const _entityCache = {};

  /**
   * 检查 Cesium 全局变量是否可用
   */
  function _checkCesium() {
    if (typeof Cesium === 'undefined') {
      throw new Error('[CesiumCore] Cesium is not loaded. Ensure Cesium CDN script is loaded before cesium_core.js.');
    }
  }

  /**
   * 经纬度转 Cartesian3
   * @param {{lng: number, lat: number, alt: number}} pos
   * @returns {Cesium.Cartesian3}
   */
  function _toCartesian(pos) {
    _checkCesium();
    return Cesium.Cartesian3.fromDegrees(pos.lng, pos.lat, pos.alt || 0);
  }

  /**
   * 初始化（创建或获取已存在的 Viewer 单例）
   * @param {string} containerId    容器 DOM ID
   * @param {Object} [options={}]   Cesium.Viewer 配置选项
   * @returns {Cesium.Viewer}
   */
  function initialize(containerId, options = {}) {
    if (_viewer) {
      console.warn('[CesiumCore] Viewer already initialized, returning existing instance.');
      return _viewer;
    }

    _checkCesium();

    const defaultOptions = {
      animation: false,
      timeline: false,
      baseLayerPicker: false,
      fullscreenButton: false,
      geocoder: false,
      homeButton: false,
      infoBox: false,
      sceneModePicker: false,
      selectionIndicator: false,
      navigationHelpButton: false,
      terrainProvider: Cesium.createWorldTerrain(),
      ...options,
    };

    _viewer = new Cesium.Viewer(containerId, defaultOptions);

    // 优化渲染性能
    _viewer.scene.globe.enableLighting = true;
    _viewer.scene.globe.depthTestAgainstTerrain = true;
    _viewer.scene.fog.enabled = true;

    // 注入到 StateManager
    if (typeof StateManager !== 'undefined') {
      StateManager.set('cesiumViewer', _viewer);
    }

    // 广播初始化事件
    if (typeof EventBus !== 'undefined') {
      EventBus.emit('cesium:initialized', { viewer: _viewer });
    }

    console.log('[CesiumCore] Viewer initialized successfully.');
    return _viewer;
  }

  /**
   * 获取 Viewer 实例
   * @returns {Cesium.Viewer|null}
   */
  function getViewer() {
    return _viewer;
  }

  /**
   * 添加飞行器实体
   * @param {string}        id              实体 ID
   * @param {{lng: number, lat: number, alt: number}} position     初始位置
   * @param {{heading?: number, pitch?: number, roll?: number}} [orientation] 初始姿态
   * @returns {Cesium.Entity}
   */
  function addAircraft(id, position, orientation = {}) {
    if (!_viewer) throw new Error('[CesiumCore] Viewer not initialized. Call initialize() first.');
    _checkCesium();

    const { heading = 0, pitch = 0, roll = 0 } = orientation;
    const hpr = new Cesium.HeadingPitchRoll(
      Cesium.Math.toRadians(heading),
      Cesium.Math.toRadians(pitch),
      Cesium.Math.toRadians(roll)
    );
    const fixedOrientation = Cesium.Transforms.headingPitchRollQuaternion(
      _toCartesian(position), hpr
    );

    const entity = _viewer.entities.add({
      id,
      name: id,
      position: _toCartesian(position),
      orientation: fixedOrientation,
      model: {
        uri: 'https://raw.githubusercontent.com/nicrzy/CesiumUAVModels/main/models/CesiumMilkTruck.glb',
        minimumPixelSize: 64,
        maximumScale: 32,
        silhouetteColor: Cesium.Color.WHITE,
        silhouetteSize: 2,
      },
      label: {
        text: id,
        font: '14px sans-serif',
        style: Cesium.LabelStyle.FILL,
        fillColor: Cesium.Color.CYAN,
        verticalOrigin: Cesium.VerticalOrigin.BOTTOM,
        pixelOffset: new Cesium.Cartesian2(0, -20),
      },
    });

    _entityCache[id] = entity;

    if (typeof EventBus !== 'undefined') {
      EventBus.emit('cesium:entity:added', { id, entity, type: 'aircraft' });
    }

    return entity;
  }

  /**
   * 更新飞行器位置和姿态
   * @param {string} id     实体 ID
   * @param {{lng: number, lat: number, alt: number, heading?: number, pitch?: number, roll?: number}} position
   */
  function updatePosition(id, position) {
    if (!_viewer) throw new Error('[CesiumCore] Viewer not initialized.');
    _checkCesium();

    const entity = _entityCache[id] || _viewer.entities.getById(id);
    if (!entity) {
      console.warn(`[CesiumCore] Entity "${id}" not found.`);
      return;
    }

    const cart = _toCartesian(position);
    entity.position = cart;

    // 更新朝向
    const heading = position.heading !== undefined ? position.heading : 0;
    const pitch = position.pitch !== undefined ? position.pitch : 0;
    const roll = position.roll !== undefined ? position.roll : 0;

    const hpr = new Cesium.HeadingPitchRoll(
      Cesium.Math.toRadians(heading),
      Cesium.Math.toRadians(pitch),
      Cesium.Math.toRadians(roll)
    );
    entity.orientation = Cesium.Transforms.headingPitchRollQuaternion(cart, hpr);
  }

  /**
   * 设置相机视角（瞬间切换）
   * @param {{lng: number, lat: number, alt: number}} position       相机目标点
   * @param {{heading?: number, pitch?: number, range?: number}} [orientation] 相机姿态
   */
  function setCamera(position, orientation = {}) {
    if (!_viewer) throw new Error('[CesiumCore] Viewer not initialized.');
    _checkCesium();

    const { heading = 0, pitch = -30, range = 5000 } = orientation;

    _viewer.camera.setView({
      destination: Cesium.Cartesian3.fromDegrees(position.lng, position.lat, position.alt + range),
      orientation: {
        heading: Cesium.Math.toRadians(heading),
        pitch: Cesium.Math.toRadians(pitch),
        roll: 0,
      },
    });
  }

  /**
   * 平滑飞向目标位置
   * @param {{lng: number, lat: number, alt: number}} position
   * @param {number} [duration=2] 飞行持续时间（秒）
   */
  function flyTo(position, duration = 2) {
    if (!_viewer) throw new Error('[CesiumCore] Viewer not initialized.');
    _checkCesium();

    _viewer.camera.flyTo({
      destination: Cesium.Cartesian3.fromDegrees(position.lng, position.lat, position.alt + 3000),
      orientation: {
        heading: Cesium.Math.toRadians(0),
        pitch: Cesium.Math.toRadians(-30),
        roll: 0,
      },
      duration,
    });
  }

  /**
   * 为飞行器添加尾迹线
   * @param {string} entityId      飞行器实体 ID
   * @param {Cesium.Color} [color] 尾迹颜色
   * @returns {Cesium.Entity}
   */
  function addTrail(entityId, color) {
    if (!_viewer) throw new Error('[CesiumCore] Viewer not initialized.');
    _checkCesium();

    const trailColor = color || Cesium.Color.YELLOW.withAlpha(0.6);
    const trailId = `${entityId}-trail`;

    const trail = _viewer.entities.add({
      id: trailId,
      polyline: {
        positions: new Cesium.CallbackProperty(() => {
          const ac = _entityCache[entityId] || _viewer.entities.getById(entityId);
          if (!ac || !ac.position) return [];
          const pos = ac.position.getValue(Cesium.JulianDate.now());
          if (!pos) return [];
          // 返回当前点（trailPositions 由外部维护时只需声明实体）
          return [pos];
        }, false),
        width: 2,
        material: trailColor,
        clampToGround: false,
      },
    });

    _entityCache[trailId] = trail;
    return trail;
  }

  /**
   * 移除实体
   * @param {string} id 实体 ID
   */
  function removeEntity(id) {
    if (!_viewer) return;
    _viewer.entities.removeById(id);
    delete _entityCache[id];
  }

  /**
   * 销毁 Viewer（慎用，仅测试环境）
   */
  function destroy() {
    if (_viewer) {
      _viewer.destroy();
      _viewer = null;
      for (const key of Object.keys(_entityCache)) {
        delete _entityCache[key];
      }
      console.log('[CesiumCore] Viewer destroyed.');
    }
  }

  // 挂载到全局
  if (typeof window !== 'undefined') {
    window.CesiumCore = CesiumCore;
  }

  return {
    initialize,
    getViewer,
    addAircraft,
    updatePosition,
    setCamera,
    flyTo,
    addTrail,
    removeEntity,
    destroy,
  };
})();
