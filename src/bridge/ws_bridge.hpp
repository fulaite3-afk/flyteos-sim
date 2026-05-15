#pragma once
/**
 * @file ws_bridge.hpp
 * @brief WebSocket桥接层 —— C++ SITL ↔ HTML5前端通信
 *
 * 架构：
 *   C++ SITL Manager → TelemetrySnapshot → JSON序列化 → WebSocket → HTML5前端
 *   HTML5前端 → JSON指令 → WebSocket → C++ → MsgRcInput → SITL
 *
 * 依赖：libwebsocket 或 原始套接字
 * 当前实现：HTTP + SSE 推送（纯C++17，零外部依赖）
 *          + HTTP POST 接收控制指令
 *
 * 未来升级：替换为真正的WebSocket（需libwebsocket或uWebSockets）
 */

#include "../../include/flyteos_types.hpp"
#include "../sitl/sitl_manager.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>

// 网络头文件
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

namespace FlyteOS::Bridge {

class WSBridge {
public:
    struct Config {
        int  port           = 8765;
        int  broadcast_hz   = 20;     // 遥测推送频率
        bool verbose        = false;
    };

    explicit WSBridge(Config cfg);
    WSBridge() : WSBridge(Config{}) {}
    ~WSBridge();

    /// 启动桥接服务（阻塞或后台线程）
    bool start(Sim::SITLManager* sitl);

    /// 停止
    void stop();

    bool isRunning() const { return running_; }

private:
    Config cfg_;
    Sim::SITLManager* sitl_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    int server_fd_ = -1;

    // HTTP服务器主循环
    void serverLoop();

    // 处理HTTP请求
    std::string handleRequest(const std::string& request);

    // 遥测数据序列化为JSON
    std::string telemetryToJson(const Sim::SITLManager::TelemetrySnapshot& snap);

    // 解析控制指令
    Bus::MsgRcInput parseRcInput(const std::string& body);

    // HTTP响应构造
    std::string httpResponse(int code, const std::string& content_type,
                              const std::string& body);
    std::string corsHeaders();
};

} // namespace FlyteOS::Bridge
