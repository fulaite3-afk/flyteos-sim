# FlyteOS - 开源浮空飞行器飞行模拟器

<div align="center">

**"开源飞行仿真 · 共建浮空生态"**

[English](README.md) · [项目文档](docs/) · [贡献指南](CONTRIBUTING.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web Simulator](https://img.shields.io/badge/Web-FlyteOS-green.svg)](https://github.com/fulaite3-afk/flyteos-sim)
[![飞控Phase](https://img.shields.io/badge/Flight%20Controller-Phase%201-orange.svg)]()

</div>

---

## 项目简介

**FlyteOS** 是浮空飞行器飞行仿真平台，采用MIT许可证开源。

不同于大疆等厂商的闭源方案，FlyteOS 坚持 **100% 开源**，包括：
- 🌐 Web端飞行模拟器（Three.js + 实景地图）
- ⚙️ 飞控固件（C++ / STM32）
- 📡 开放API与SDK

> 大疆闭源服务企业客户，我们开源服务整个行业生态。

---

## 核心特性

| 特性 | 说明 |
|------|------|
| 🗺️ **实景地图** | 基于 ESRI World Imagery + Mapzen 地形，零成本真实地形渲染 |
| 🎮 **Web端运行** | 无需安装，打开浏览器即可飞行 |
| ⚡ **PD/PID飞控** | 开源飞控算法，可对接真实硬件 |
| 🔗 **DJI OSDK集成** | 支持DJI SDK桥接，兼容主流飞控板 |
| 🌍 **全球地形** | 自动加载任意地理位置的真实地形 |
| 🛤️ **航点飞行** | 可视化航点规划，自主航线飞行 |
| 🌤️ **天气系统** | 程序化天空、云层、体积雾效 |

---

## 技术架构

```
┌─────────────────────────────────────────────────────┐
│                   FlyteOS 架构                       │
├─────────────────────────────────────────────────────┤
│                                                      │
│  ┌─────────────┐     ┌──────────────────────────┐   │
│  │  Web 前端   │     │       飞控固件            │   │
│  │  Three.js   │◄───►│  STM32 / ESP32 / 树莓派   │   │
│  │  three-tile │     │  PD/PID 控制算法          │   │
│  │  WebGL 3D   │     │  DJI OSDK 桥接           │   │
│  └──────┬──────┘     └──────────┬───────────────┘   │
│         │                       │                   │
│  ┌──────▼───────────────────────▼───────────────┐    │
│  │              数据层（全开源）                │    │
│  │  ESRI卫星图 · Mapzen地形 · OSM道路图        │    │
│  └─────────────────────────────────────────────┘    │
│                                                      │
└─────────────────────────────────────────────────────┘
```

---

## 快速开始

### Web模拟器

```bash
# 克隆仓库
git clone https://github.com/fulaite3-afk/flyteos-sim.git
cd flyteos-sim/simulator

# 启动服务（任意静态服务器均可）
python -m http.server 8080
# 或
npx serve .

# 打开浏览器
http://localhost:8080/flight_sim_2.html
```

### 飞行操控

| 按键 | 功能 |
|------|------|
| W / S | 加速 / 减速 |
| A / D | 航向左 / 右转 |
| Q / E | 上升 / 下降 |
| 空格 | 自动起飞 |
| 1-5 | 切换视角 |
| 点击地图 | 添加航点 |
| 起点 → 终点 | 开始航点飞行 |

---

## 数据来源（100%开源）

| 数据类型 | 来源 | 授权 |
|---------|------|------|
| 卫星影像 | ESRI World Imagery | 免费，需署名 |
| 地形高程 | Mapzen Terrain (AWS) | 公共领域 |
| 道路地图 | OpenStreetMap | ODbL |
| 天气数据 | Open-Meteo API | 完全开源 |

---

## 路线图

- [ ] **v1.0** - Web模拟器基础版（星空背景） ✅
- [ ] **v1.1** - 实景地图集成（ESRI + Mapzen）
- [ ] **v2.0** - 程序化天气系统
- [ ] **v2.1** - 中国本地化界面
- [ ] **v3.0** - 飞控固件开源（Phase 2）
- [ ] **v3.1** - DJI OSDK深度集成
- [ ] **v4.0** - 开放API + SDK

---

## 参与贡献

我们欢迎所有开发者参与！

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 创建 Pull Request

详细指南请阅读 [贡献指南](CONTRIBUTING.md)。

---

## 许可证

本项目基于 [MIT 许可证](LICENSE) 开源。

> MIT License - © 2026 福莱特航空科技有限公司
> 
> 授予权利：商业使用、修改、分发、私用
> 唯一条件：需包含版权声明
> 禁止条款：无

---

## 关于福莱特

**福莱特航空科技有限公司**
- 成立于：2016年11月
- 地址：中国·武汉
- 官网：http://www.flyteaviation.com
- 邮箱：contact@flyteos-sim.org

> 浮空节能环保型无人机，以"浮空飞行器"专利为依托，利用太阳能膜科技和电动涡轮作为动力来源，低碳节能、绿色环保。

---

<div align="center">

**让飞行更自由，让天空更开放**

</div>
