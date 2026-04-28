# FlyteOS - Open Source Aerostat Flight Simulator

<div align="center">

**"Open Flight Simulation · Build the Floating Ecosystem"**

[中文](README_zh.md) · [Contributing](CONTRIBUTING.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web Simulator](https://img.shields.io/badge/Web-FlyteOS-green.svg)](https://github.com/fulaite3-afk/flyteos-sim)
[![Flight Phase](https://img.shields.io/badge/Flight%20Controller-Phase%201-orange.svg)]()

</div>

---

## Overview

**FlyteOS** is an open-source aerostat flight simulation platform developed by **Wuhan Fulaite Aviation Technology Co., Ltd.**

Unlike DJI and other closed-source vendors, FlyteOS is **100% open source**, including:
- 🌐 Web-based flight simulator (Three.js + real-world terrain)
- ⚙️ Flight controller firmware (C++ / STM32)
- 📡 Open API and SDK

> DJI closed-source serves enterprise customers; we open-source to serve the entire industry ecosystem.

---

## Key Features

| Feature | Description |
|---------|-------------|
| 🗺️ **Real-World Maps** | ESRI World Imagery + Mapzen terrain, zero-cost realistic rendering |
| 🎮 **Web-Based** | No installation needed, fly directly in browser |
| ⚡ **PD/PID Flight Control** | Open-source control algorithms, hardware-ready |
| 🔗 **DJI OSDK Integration** | DJI SDK bridge, compatible with major flight controllers |
| 🌍 **Global Terrain** | Auto-load real terrain at any GPS location |
| 🛤️ **Waypoint Navigation** | Visual waypoint planning, autonomous flight |
| 🌤️ **Weather System** | Procedural sky, clouds, volumetric fog |

---

## Quick Start

```bash
# Clone the repo
git clone https://github.com/fulaite3-afk/flyteos-sim.git
cd flyteos-sim/simulator

# Start server
python -m http.server 8080
# or
npx serve .

# Open browser
http://localhost:8080/flight_sim_2.html
```

### Controls

| Key | Action |
|-----|--------|
| W / S | Speed up / Slow down |
| A / D | Turn left / right |
| Q / E | Ascend / Descend |
| Space | Auto takeoff |
| 1-5 | Switch camera view |
| Click map | Add waypoint |
| Start → End | Begin waypoint flight |

---

## Data Sources (100% Open Source)

| Data Type | Source | License |
|-----------|--------|---------|
| Satellite Imagery | ESRI World Imagery | Free, attribution required |
| Terrain Elevation | Mapzen Terrain (AWS) | Public Domain |
| Road Maps | OpenStreetMap | ODbL |
| Weather | Open-Meteo API | Fully Open Source |

---

## Roadmap

- [x] **v1.0** - Web simulator basic (starfield background)
- [ ] **v1.1** - Real-world terrain integration (ESRI + Mapzen)
- [ ] **v2.0** - Procedural weather system
- [ ] **v2.1** - Chinese localization
- [ ] **v3.0** - Flight controller firmware open source (Phase 2)
- [ ] **v3.1** - DJI OSDK deep integration
- [ ] **v4.0** - Open API + SDK

---

## Contributing

We welcome all developers!

1. Fork this repository
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add: AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

---

## License

This project is licensed under the [MIT License](LICENSE).

---

## About Fulaite

**Wuhan Fulaite Aviation Technology Co., Ltd.**
- Founded: November 2016
- Location: Wuhan Donghu New Technology Development Zone, China
- Email: 325403386@qq.com

Specializing in environmentally-friendly aerostat UAVs, leveraging solar membrane technology and electric turbochargers for low-carbon, green aviation.

---

<div align="center">

**Making Flight Free, Making Sky Open**

</div>
