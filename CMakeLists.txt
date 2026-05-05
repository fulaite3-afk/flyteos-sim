cmake_minimum_required(VERSION 3.16)
project(FlyteOS VERSION 1.0.0 LANGUAGES CXX)

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

# ── 核心库 ───────────────────────────────────────────────────────────
file(GLOB_RECURSE CORE_SOURCES
    "src/flyteos_types.cpp"
    "src/core/*.cpp"
    "src/power/*.cpp"
    "src/attitude/*.cpp"
    "src/flight_control/*.cpp"
    "src/navigation/*.cpp"
    "src/safety/*.cpp"
    "src/dji_bridge/*.cpp"
)

add_library(flyteos_core STATIC ${CORE_SOURCES})

# ── 模拟器可执行文件 ─────────────────────────────────────────────────
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
