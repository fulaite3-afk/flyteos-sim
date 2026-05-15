cmake_minimum_required(VERSION 3.16)
project(FlyteOS VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 编译选项 ─────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -O2)

# ── 包含目录 ─────────────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/simulator
)

# ── 核心库（飞控+导航+浮力+安全+姿态+桥接+总线）──────────────────────
file(GLOB_RECURSE CORE_SOURCES
    "src/flyteos_types.cpp"
    "src/core/*.cpp"
    "src/power/*.cpp"
    "src/attitude/*.cpp"
    "src/flight_control/*.cpp"
    "src/navigation/*.cpp"
    "src/safety/*.cpp"
    "src/dji_bridge/*.cpp"
    "src/bus/*.cpp"
    "src/sitl/*.cpp"
    "src/bridge/*.cpp"
)

add_library(flyteos_core STATIC ${CORE_SOURCES})

# ── SITL仿真器（v2.0 闭环仿真入口）──────────────────────────────────
add_executable(flyteos_sitl
    simulator/main_sitl.cpp
)
target_link_libraries(flyteos_sitl flyteos_core pthread)

# ── 旧版模拟器（兼容保留）─────────────────────────────────────────────
add_executable(flyteos_sim
    simulator/main_simulator.cpp
    simulator/physics_engine.cpp
    simulator/sensor_mock.cpp
    simulator/visualizer.cpp
)
target_link_libraries(flyteos_sim flyteos_core pthread)

# ── 主程序（嵌入式目标）─────────────────────────────────────────────
add_executable(flyteos_main
    src/main.cpp
)
target_link_libraries(flyteos_main flyteos_core pthread)

# ── 单元测试 ─────────────────────────────────────────────────────────
add_executable(flyteos_test
    tests/test_flight_control.cpp
)
target_link_libraries(flyteos_test flyteos_core pthread)

# 启用测试
enable_testing()
add_test(NAME FlightControlTest COMMAND flyteos_test)

message(STATUS "FlyteOS v${PROJECT_VERSION} - 武汉福莱特航空科技有限公司")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Targets: flyteos_sitl (SITL闭环), flyteos_sim (旧版), flyteos_main (嵌入式)")
