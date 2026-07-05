/**
 * fault_tree_panel.js — 故障树面板
 * ============================================================================
 * 功能：故障树形展示与诊断统计。展示故障节点层级结构（可折叠），
 *       每个节点显示名称/状态/概率，底部统计区汇总故障等级分布。
 *       从 flight_demo_cesium.html 传感器/健康监控逻辑中抽象故障模型。
 * 依赖：core/event_bus.js（EventBus）、core/state_manager.js（StateManager）
 *
 * 绑定事件：
 *   state:fault:changed          — 监视 fault 状态对象变化
 *   fault:node:select            — 选中故障节点时发布
 *   fault:inject                 — 故障注入事件（与 sensor_panel 联动）
 *   fault:clear:all              — 清除所有故障
 *
 * 暴露接口（挂载到 window.Panels.faultTree）：
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

  // ── 默认故障树数据 ────────────────────────────────────────
  const DEFAULT_TREE = [
    {
      id: 'SYS', label: 'System Failure', level: 'root', expanded: true,
      probability: '0.02%', status: 'nominal',
      children: [
        {
          id: 'NAV', label: 'Navigation Failure', level: 'branch', expanded: true,
          probability: '0.15%', status: 'nominal',
          children: [
            { id: 'GPS_LOSS',    label: 'GPS Signal Loss',       probability: '0.08%', status: 'nominal',  level: 'leaf' },
            { id: 'IMU_DRIFT',   label: 'IMU Gyro Drift',        probability: '0.05%', status: 'nominal',  level: 'leaf' },
            { id: 'MAG_ANOMALY', label: 'Magnetometer Anomaly',  probability: '0.02%', status: 'nominal',  level: 'leaf' },
          ],
        },
        {
          id: 'COMM', label: 'Communication Failure', level: 'branch', expanded: true,
          probability: '0.12%', status: 'nominal',
          children: [
            { id: 'LINK_LOSS',   label: 'Data Link Loss',        probability: '0.06%', status: 'nominal',  level: 'leaf' },
            { id: 'RC_TIMEOUT',  label: 'RC Signal Timeout',     probability: '0.04%', status: 'nominal',  level: 'leaf' },
            { id: 'SAT_LOST',    label: 'Satellite Lost',        probability: '0.02%', status: 'nominal',  level: 'leaf' },
          ],
        },
        {
          id: 'PWR', label: 'Power System Failure', level: 'branch', expanded: true,
          probability: '0.08%', status: 'nominal',
          children: [
            { id: 'BAT_LOW',     label: 'Battery Low Voltage',   probability: '0.05%', status: 'nominal',  level: 'leaf' },
            { id: 'ESC_FAULT',   label: 'ESC Overheat',          probability: '0.02%', status: 'nominal',  level: 'leaf' },
            { id: 'BEC_FAIL',    label: 'BEC Output Failure',    probability: '0.01%', status: 'nominal',  level: 'leaf' },
          ],
        },
        {
          id: 'PROP', label: 'Propulsion Failure', level: 'branch', expanded: true,
          probability: '0.10%', status: 'nominal',
          children: [
            { id: 'MOTOR_FAIL',  label: 'Motor Stall',           probability: '0.06%', status: 'nominal',  level: 'leaf' },
            { id: 'PROP_DMG',    label: 'Propeller Damage',      probability: '0.04%', status: 'nominal',  level: 'leaf' },
          ],
        },
      ],
    },
  ];

  // ── CSS 注入 ──────────────────────────────────────────────
  function _injectCSS() {
    if (_styleEl) return;
    _styleEl = document.createElement('style');
    _styleEl.id = 'fault-tree-panel-styles';
    _styleEl.textContent = `
      .ft-card {
        background: #101830; border: 1px solid #1a2a4a; border-radius: 6px;
        padding: 12px; margin-bottom: 8px;
        font-family: 'Segoe UI', 'Consolas', monospace; color: #c0d0f0;
      }
      .ft-card h3 { font-size:13px; color:#6a9fd8; margin:0 0 10px 0; border-bottom:1px solid #1a2a4a; padding-bottom:6px; text-transform:uppercase; letter-spacing:1px; }
      .ft-section { margin-top:10px; padding-top:8px; border-top:1px solid #182442; }
      .ft-section h4 { font-size:11px; color:#506a8e; margin:0 0 6px 0; letter-spacing:0.5px; }

      /* ── 树节点 ── */
      .ft-tree { font-size:11px; }
      .ft-node { padding:3px 0; }
      .ft-node-row { display:flex; align-items:center; padding:3px 6px; border-radius:3px; cursor:pointer; transition:background 0.15s; }
      .ft-node-row:hover { background:#141e38; }
      .ft-node-row.selected { background:#1a2a4a; }
      .ft-toggle { width:14px; height:14px; font-size:10px; line-height:14px; text-align:center; margin-right:4px;
        color:#506a8e; flex-shrink:0; border-radius:2px; transition:transform 0.2s; }
      .ft-toggle.empty { visibility:hidden; }
      .ft-toggle.expanded { transform:rotate(90deg); }
      .ft-dot { width:8px; height:8px; border-radius:50%; margin-right:6px; flex-shrink:0; }
      .ft-dot.nominal { background:#4ef0a0; box-shadow:0 0 4px rgba(78,240,160,0.4); }
      .ft-dot.warning { background:#f0a04e; box-shadow:0 0 4px rgba(240,160,78,0.4); animation:ft-pulse 1.5s infinite; }
      .ft-dot.fault   { background:#f04e4e; box-shadow:0 0 4px rgba(240,78,78,0.4); animation:ft-pulse 0.8s infinite; }
      .ft-dot.inactive { background:#3a3a4a; }
      .ft-node-name { flex:1; color:#c0d8f0; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
      .ft-node.prob { font-size:10px; color:#6a8aae; font-family:'Consolas',monospace; margin-left:6px; min-width:42px; text-align:right; }
      .ft-children { margin-left:18px; border-left:1px solid #182442; padding-left:6px; display:block; }
      .ft-children.collapsed { display:none; }

      /* ── 统计区 ── */
      .ft-stats-grid { display:grid; grid-template-columns:1fr 1fr 1fr; gap:4px; }
      .ft-stat-item { background:#0d1428; border:1px solid #182442; border-radius:4px; padding:6px 8px; text-align:center; }
      .ft-stat-value { font-size:16px; font-weight:bold; font-family:'Consolas',monospace; }
      .ft-stat-label { font-size:10px; color:#506a8e; margin-top:2px; }
      .ft-stat-item.nominal { border-color:#1a3a2a; }
      .ft-stat-item.warning { border-color:#3a2a0a; }
      .ft-stat-item.fault   { border-color:#3a1a1a; }
      .ft-stat-value.nominal { color:#4ef0a0; }
      .ft-stat-value.warning { color:#f0a04e; }
      .ft-stat-value.fault   { color:#f04e4e; }

      /* ── 操作按钮 ── */
      .ft-actions { display:flex; gap:4px; margin-top:8px; }
      .ft-action-btn { flex:1; padding:5px 8px; border:1px solid #182442; border-radius:3px; background:#0d1428;
        color:#6a8aae; font-size:10px; cursor:pointer; transition:all 0.2s; text-align:center; }
      .ft-action-btn:hover { background:#141e38; color:#c0d8f0; }
      .ft-action-btn.clear { border-color:#3a1a1a; color:#f04e4e; }
      .ft-action-btn.clear:hover { background:rgba(240,78,78,0.08); }

      @keyframes ft-pulse { 0%,100%{opacity:1;} 50%{opacity:0.5;} }
    `;
    document.head.appendChild(_styleEl);
  }

  // ── DOM 构建 ──────────────────────────────────────────────
  function _buildRoot() {
    var root = document.createElement('div');
    root.className = 'ft-card';
    root.id = 'fault-tree-panel-root';
    root.innerHTML = '<h3>FAULT TREE</h3>';

    // 树形展示区
    var treeWrap = document.createElement('div');
    treeWrap.className = 'ft-tree';
    treeWrap.id = 'ft-tree-wrap';
    root.appendChild(treeWrap);
    _refs.treeWrap = treeWrap;

    // 统计区
    var statsSection = document.createElement('div');
    statsSection.className = 'ft-section';
    statsSection.innerHTML = '<h4>Diagnostics Summary</h4>';
    var statsGrid = document.createElement('div');
    statsGrid.className = 'ft-stats-grid';
    statsGrid.id = 'ft-stats-grid';
    statsSection.appendChild(statsGrid);
    _refs.statsGrid = statsGrid;

    // 操作按钮
    var actionsDiv = document.createElement('div');
    actionsDiv.className = 'ft-actions';
    actionsDiv.innerHTML =
      '<button class="ft-action-btn" id="ft-btn-expand-all">Expand All</button>' +
      '<button class="ft-action-btn" id="ft-btn-collapse-all">Collapse All</button>' +
      '<button class="ft-action-btn clear" id="ft-btn-clear-faults">Clear Faults</button>';
    _refs.btnExpandAll  = actionsDiv.querySelector('#ft-btn-expand-all');
    _refs.btnCollapseAll = actionsDiv.querySelector('#ft-btn-collapse-all');
    _refs.btnClearFaults = actionsDiv.querySelector('#ft-btn-clear-faults');

    root.appendChild(statsSection);
    root.appendChild(actionsDiv);
    return root;
  }

  // ── 递归渲染树节点 ────────────────────────────────────────
  function _renderNode(node, depth) {
    var hasChildren = node.children && node.children.length > 0;
    var status = node.status || 'nominal';

    var nodeDiv = document.createElement('div');
    nodeDiv.className = 'ft-node';
    nodeDiv.setAttribute('data-id', node.id);

    var row = document.createElement('div');
    row.className = 'ft-node-row';
    row.style.paddingLeft = (depth * 0) + 'px';

    // 折叠箭头
    var toggle = document.createElement('span');
    toggle.className = 'ft-toggle' + (hasChildren ? ' expanded' : ' empty');
    toggle.textContent = hasChildren ? '\u25B6' : '';
    row.appendChild(toggle);

    // 状态点
    var dot = document.createElement('span');
    dot.className = 'ft-dot ' + status;
    row.appendChild(dot);

    // 名称
    var name = document.createElement('span');
    name.className = 'ft-node-name';
    name.textContent = node.label;
    row.appendChild(name);

    // 概率
    if (node.probability) {
      var prob = document.createElement('span');
      prob.className = 'ft-node prob';
      prob.textContent = node.probability;
      row.appendChild(prob);
    }

    // 行点击事件
    row.addEventListener('click', function () {
      if (hasChildren) _toggleChildren(nodeDiv, toggle);
      _selectNode(node);
    });

    nodeDiv.appendChild(row);

    // 子节点
    if (hasChildren) {
      var childrenWrap = document.createElement('div');
      childrenWrap.className = 'ft-children';
      node.children.forEach(function (child) {
        childrenWrap.appendChild(_renderNode(child, depth + 1));
      });
      nodeDiv.appendChild(childrenWrap);
    }

    return nodeDiv;
  }

  function _toggleChildren(nodeDiv, toggle) {
    var children = nodeDiv.querySelector('.ft-children');
    if (!children) return;
    var isCollapsed = children.classList.contains('collapsed');
    if (isCollapsed) {
      children.classList.remove('collapsed');
      toggle.classList.add('expanded');
    } else {
      children.classList.add('collapsed');
      toggle.classList.remove('expanded');
    }
  }

  function _selectNode(node) {
    // 清除其他选中
    var allRows = _refs.treeWrap.querySelectorAll('.ft-node-row');
    allRows.forEach(function (r) { r.classList.remove('selected'); });
    // 高亮当前
    var targetEl = _refs.treeWrap.querySelector('[data-id="' + node.id + '"]');
    if (targetEl) {
      var targetRow = targetEl.querySelector('.ft-node-row');
      if (targetRow) targetRow.classList.add('selected');
    }
    if (EventBus) EventBus.emit('fault:node:select', node);
  }

  // ── 渲染统计 ──────────────────────────────────────────────
  function _renderStats(treeData) {
    if (!_refs.statsGrid) return;

    // 扁平化统计
    var counts = { nominal: 0, warning: 0, fault: 0 };
    function walk(nodes) {
      nodes.forEach(function (n) {
        counts[n.status] = (counts[n.status] || 0) + 1;
        if (n.children) walk(n.children);
      });
    }
    walk(treeData || DEFAULT_TREE);

    _refs.statsGrid.innerHTML =
      '<div class="ft-stat-item nominal">' +
        '<div class="ft-stat-value nominal">' + counts.nominal + '</div>' +
        '<div class="ft-stat-label">NOMINAL</div>' +
      '</div>' +
      '<div class="ft-stat-item warning">' +
        '<div class="ft-stat-value warning">' + (counts.warning || 0) + '</div>' +
        '<div class="ft-stat-label">WARNING</div>' +
      '</div>' +
      '<div class="ft-stat-item fault">' +
        '<div class="ft-stat-value fault">' + (counts.fault || 0) + '</div>' +
        '<div class="ft-stat-label">FAULT</div>' +
      '</div>';
  }

  // ── 渲染整棵树 ────────────────────────────────────────────
  function _renderTree(treeData) {
    if (!_refs.treeWrap) return;
    _refs.treeWrap.innerHTML = '';
    var data = treeData || DEFAULT_TREE;
    data.forEach(function (node) {
      _refs.treeWrap.appendChild(_renderNode(node, 0));
    });
    _renderStats(data);
  }

  // ── 数据刷新 ──────────────────────────────────────────────
  function refresh(data) {
    var f = data || (StateManager ? StateManager.get('fault') : null);
    if (f && f.tree) {
      _renderTree(f.tree);
    } else if (!f) {
      _renderTree();
    }
  }

  // ── 全展开/折叠 ───────────────────────────────────────────
  function _expandAll() {
    var children = _refs.treeWrap.querySelectorAll('.ft-children');
    var toggles = _refs.treeWrap.querySelectorAll('.ft-toggle:not(.empty)');
    children.forEach(function (c) { c.classList.remove('collapsed'); });
    toggles.forEach(function (t) { t.classList.add('expanded'); });
  }

  function _collapseAll() {
    var children = _refs.treeWrap.querySelectorAll('.ft-children');
    var toggles = _refs.treeWrap.querySelectorAll('.ft-toggle:not(.empty)');
    children.forEach(function (c) { c.classList.add('collapsed'); });
    toggles.forEach(function (t) { t.classList.remove('expanded'); });
  }

  // ── 事件绑定 ──────────────────────────────────────────────
  function _bindEvents() {
    if (_refs.btnExpandAll) {
      _refs.btnExpandAll.addEventListener('click', _expandAll);
    }
    if (_refs.btnCollapseAll) {
      _refs.btnCollapseAll.addEventListener('click', _collapseAll);
    }
    if (_refs.btnClearFaults) {
      _refs.btnClearFaults.addEventListener('click', function () {
        // 重置所有节点状态为 nominal
        function reset(nodes) {
          nodes.forEach(function (n) {
            n.status = 'nominal';
            if (n.children) reset(n.children);
          });
        }
        reset(DEFAULT_TREE);
        _renderTree(DEFAULT_TREE);
        if (EventBus) EventBus.emit('fault:clear:all');
        if (StateManager) StateManager.merge({ fault: { tree: DEFAULT_TREE } });
      });
    }

    // 故障注入监听（与 sensor_panel 联动）
    if (EventBus) {
      _eventUnsubs.push(EventBus.on('sensor:fault:injected', function (payload) {
        // 根据传感器故障类型更新树节点状态
        if (payload && payload.sensor) {
          var faultId = _mapSensorToFaultId(payload.sensor);
          if (faultId) _updateNodeStatus(DEFAULT_TREE, faultId, 'fault');
          _renderTree(DEFAULT_TREE);
          if (StateManager) StateManager.merge({ fault: { tree: DEFAULT_TREE } });
        }
      }));
    }

    if (StateManager) {
      var unsub = StateManager.subscribe('fault', function (newVal) { refresh(newVal); });
      _subscriptions.push(unsub);
      var cur = StateManager.get('fault');
      if (cur) refresh(cur);
    }
  }

  // 传感器到故障 ID 的映射
  function _mapSensorToFaultId(sensorName) {
    var map = {
      'IMU': 'IMU_DRIFT',
      'Barometer': 'BEC_FAIL',
      'GPS': 'GPS_LOSS',
      'Magnetometer': 'MAG_ANOMALY',
    };
    return map[sensorName] || null;
  }

  // 更新节点状态
  function _updateNodeStatus(nodes, targetId, newStatus) {
    for (var i = 0; i < nodes.length; i++) {
      if (nodes[i].id === targetId) {
        nodes[i].status = newStatus;
        return true;
      }
      if (nodes[i].children && _updateNodeStatus(nodes[i].children, targetId, newStatus)) {
        return true;
      }
    }
    return false;
  }

  // ── 公开接口 ──────────────────────────────────────────────
  function init(containerId) {
    _injectCSS();
    _container = document.getElementById(containerId);
    if (!_container) { console.warn('[FaultTreePanel] Container not found:', containerId); return; }
    _rootEl = _buildRoot();
    _container.appendChild(_rootEl);
    _renderTree();
    _bindEvents();
    console.log('[FaultTreePanel] Initialized in', containerId);
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
    console.log('[FaultTreePanel] Destroyed');
  }

  if (!global.Panels) global.Panels = {};
  global.Panels.faultTree = { init: init, refresh: refresh, destroy: destroy };
})(window);
