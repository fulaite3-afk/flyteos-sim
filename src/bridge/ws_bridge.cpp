/**
 * @file ws_bridge.cpp
 * @brief WebSocket桥接层实现 —— 轻量HTTP服务器 + SSE推送 + POST控制
 *
 * API端点：
 *   GET  /api/telemetry   → 返回当前遥测JSON
 *   GET  /api/stream      → SSE事件流（持续推送遥测）
 *   POST /api/rc          → 发送遥控指令（JSON body）
 *   GET  /                → 嵌入式状态页面
 */

#include "ws_bridge.hpp"
#include <sstream>
#include <cmath>

namespace FlyteOS::Bridge {

// ════════════════════════════════════════════════════════════════
//  构造/析构
// ════════════════════════════════════════════════════════════════

WSBridge::WSBridge(Config cfg) : cfg_(cfg) {}

WSBridge::~WSBridge() {
    stop();
}

// ════════════════════════════════════════════════════════════════
//  启动/停止
// ════════════════════════════════════════════════════════════════

bool WSBridge::start(Sim::SITLManager* sitl) {
    if (running_) return true;
    sitl_ = sitl;
    if (!sitl_) return false;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        printf("[WSBridge] Failed to create socket\n");
        return false;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cfg_.port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[WSBridge] Failed to bind port %d\n", cfg_.port);
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 5) < 0) {
        printf("[WSBridge] Failed to listen\n");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&WSBridge::serverLoop, this);

    printf("[WSBridge] Server started on port %d\n", cfg_.port);
    printf("[WSBridge] Endpoints:\n");
    printf("  GET  /api/telemetry  - Current telemetry\n");
    printf("  GET  /api/stream     - SSE telemetry stream\n");
    printf("  POST /api/rc         - RC input control\n");
    printf("  GET  /               - Status page\n");
    return true;
}

void WSBridge::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    printf("[WSBridge] Stopped\n");
}

// ════════════════════════════════════════════════════════════════
//  服务器主循环
// ════════════════════════════════════════════════════════════════

void WSBridge::serverLoop() {
    while (running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd_, &read_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ret = select(server_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        char buf[4096] = {};
        int n = read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            close(client_fd);
            continue;
        }

        std::string request(buf, n);
        std::string response = handleRequest(request);

        ssize_t written = write(client_fd, response.c_str(), response.size());
        (void)written;
        close(client_fd);
    }
}

// ════════════════════════════════════════════════════════════════
//  HTTP请求处理
// ════════════════════════════════════════════════════════════════

std::string WSBridge::handleRequest(const std::string& request) {
    std::string method, path;
    std::istringstream iss(request);
    iss >> method >> path;

    if (cfg_.verbose) {
        printf("[WSBridge] %s %s\n", method.c_str(), path.c_str());
    }

    // GET /api/telemetry
    if (method == "GET" && path == "/api/telemetry") {
        auto snap = sitl_->telemetry();
        return httpResponse(200, "application/json", telemetryToJson(snap));
    }

    // GET /api/stream (SSE)
    if (method == "GET" && path == "/api/stream") {
        std::string sse_response;
        sse_response += "HTTP/1.1 200 OK\r\n";
        sse_response += corsHeaders();
        sse_response += "Content-Type: text/event-stream\r\n";
        sse_response += "Cache-Control: no-cache\r\n";
        sse_response += "Connection: close\r\n";
        sse_response += "\r\n";

        for (int i = 0; i < 10 && running_; i++) {
            auto snap = sitl_->telemetry();
            sse_response += "data: " + telemetryToJson(snap) + "\n\n";
            usleep(50000);
        }
        return sse_response;
    }

    // POST /api/rc
    if (method == "POST" && path == "/api/rc") {
        size_t body_start = request.find("\r\n\r\n");
        std::string body;
        if (body_start != std::string::npos) {
            body = request.substr(body_start + 4);
        }

        auto rc = parseRcInput(body);
        sitl_->setRcInput(rc);

        return httpResponse(200, "application/json", "{\"status\":\"ok\"}");
    }

    // GET / (status page - minimal, no JS single quotes)
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        auto snap = sitl_->telemetry();
        std::string html = R"RAW(<!DOCTYPE html>
<html><head><title>FlyteOS SITL</title>
<meta charset="utf-8">
<style>
body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;margin:20px}
h1{color:#00d4ff;border-bottom:2px solid #00d4ff;padding-bottom:8px}
.card{background:#16213e;border:1px solid #0f3460;border-radius:8px;padding:16px;margin:12px 0}
.label{color:#8888aa}.value{color:#00ff88;font-size:1.2em}
#telemetry{white-space:pre;font-size:13px;line-height:1.6}
.btn{background:#0f3460;color:#00d4ff;border:1px solid #00d4ff;padding:8px 16px;
border-radius:4px;cursor:pointer;margin:4px;font-size:14px}
.btn:hover{background:#00d4ff;color:#1a1a2e}
.bd{border-color:#ff4444;color:#ff4444}
.bd:hover{background:#ff4444;color:#1a1a2e}
</style></head><body>
<h1>FlyteOS SITL</h1>
<div class="card">
<h3>Control</h3>
<button class="btn" onclick="sendRc(&quot;arm&quot;)">ARM</button>
<button class="btn" onclick="sendRc(&quot;takeoff&quot;)">TAKEOFF</button>
<button class="btn" onclick="sendRc(&quot;land&quot;)">LAND</button>
<button class="btn bd" onclick="sendRc(&quot;emergency&quot;)">EMERGENCY</button>
<button class="btn" onclick="sendRc(&quot;hover&quot;)">HOVER</button>
</div>
<div class="card">
<h3>Telemetry</h3>
<div id="telemetry">Connecting...</div>
</div>
<script>
function sendRc(action){
var t=document.getElementById("throttle");
var tv=t?t.value/100:0.5;
var body=JSON.stringify({action:action,throttle:tv});
fetch("/api/rc",{method:"POST",body:body});
}
setInterval(function(){
fetch("/api/telemetry").then(function(r){return r.json()}).then(function(d){
var s="";
s+="Time: "+d.sim_time_s.toFixed(1)+" s\n";
s+="State: "+d.flight_state+"\n";
s+="----------------------------\n";
s+="Alt: "+d.altitude.toFixed(1)+" m\n";
s+="Speed: "+d.speed.toFixed(1)+" m/s\n";
s+="Heading: "+d.heading.toFixed(1)+" deg\n";
s+="----------------------------\n";
s+="Buoyancy: "+d.buoyancy_n.toFixed(1)+" N\n";
s+="B/W: "+d.bw_ratio.toFixed(3)+"\n";
s+="Gas Vol: "+d.gas_volume.toFixed(1)+" m3\n";
s+="Gas Temp: "+(d.gas_temp-273.15).toFixed(1)+" C\n";
s+="Gas Pres: "+(d.gas_pressure/1000).toFixed(1)+" kPa\n";
s+="Vent: "+(d.vent_open*100).toFixed(0)+" %\n";
s+="Pump: "+(d.pump_active*100).toFixed(0)+" %\n";
s+="----------------------------\n";
s+="Thrust: "+d.thrust_n.toFixed(1)+" N\n";
document.getElementById("telemetry").textContent=s;
});
},200);
</script></body></html>)RAW";
        return httpResponse(200, "text/html; charset=utf-8", html);
    }

    return httpResponse(404, "text/plain", "Not Found");
}

// ════════════════════════════════════════════════════════════════
//  遥测JSON序列化
// ════════════════════════════════════════════════════════════════

std::string WSBridge::telemetryToJson(const Sim::SITLManager::TelemetrySnapshot& snap) {
    auto stateStr = [](FlightState s) -> const char* {
        switch (s) {
            case FlightState::DISARMED:        return "DISARMED";
            case FlightState::STANDBY:         return "STANDBY";
            case FlightState::ARMED:           return "ARMED";
            case FlightState::TAKING_OFF:      return "TAKING_OFF";
            case FlightState::IN_FLIGHT:       return "IN_FLIGHT";
            case FlightState::HOVERING:        return "HOVERING";
            case FlightState::WAYPOINT_NAV:    return "WAYPOINT_NAV";
            case FlightState::LANDING:         return "LANDING";
            case FlightState::EMERGENCY_LANDING: return "EMERGENCY";
            case FlightState::FAILSAFE:        return "FAILSAFE";
            case FlightState::GROUND_ERROR:    return "GROUND_ERROR";
            default: return "UNKNOWN";
        }
    };

    f32 altitude = -snap.pos_d;
    f32 speed = sqrtf(snap.vel_n * snap.vel_n + snap.vel_e * snap.vel_e);
    f32 heading = snap.yaw_rad * 180.0f / (f32)M_PI;

    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{"
        "\"sim_time_s\":%.3f,"
        "\"step_count\":%llu,"
        "\"flight_state\":\"%s\","
        "\"altitude\":%.2f,"
        "\"speed\":%.2f,"
        "\"heading\":%.1f,"
        "\"pos_n\":%.2f,"
        "\"pos_e\":%.2f,"
        "\"pos_d\":%.2f,"
        "\"vel_n\":%.2f,"
        "\"vel_e\":%.2f,"
        "\"vel_d\":%.2f,"
        "\"roll\":%.3f,"
        "\"pitch\":%.3f,"
        "\"yaw\":%.3f,"
        "\"buoyancy_n\":%.2f,"
        "\"bw_ratio\":%.4f,"
        "\"gas_volume\":%.2f,"
        "\"gas_temp\":%.2f,"
        "\"gas_pressure\":%.1f,"
        "\"vent_open\":%.2f,"
        "\"pump_active\":%.2f,"
        "\"thrust_n\":%.2f,"
        "\"motor\":[%.3f,%.3f,%.3f,%.3f],"
        "\"battery_pct\":%.1f"
        "}",
        snap.sim_time_s,
        (unsigned long long)snap.step_count,
        stateStr(snap.flight_state),
        altitude, speed, heading,
        snap.pos_n, snap.pos_e, snap.pos_d,
        snap.vel_n, snap.vel_e, snap.vel_d,
        snap.roll_rad, snap.pitch_rad, snap.yaw_rad,
        snap.buoyancy_n, snap.buoyancy_ratio,
        snap.gas_volume_m3, snap.gas_temp_k, snap.gas_pressure_pa,
        snap.vent_open, snap.pump_active,
        snap.thrust_n,
        snap.motor[0], snap.motor[1], snap.motor[2], snap.motor[3],
        snap.battery_pct
    );
    return std::string(buf);
}

// ════════════════════════════════════════════════════════════════
//  RC指令解析
// ════════════════════════════════════════════════════════════════

Bus::MsgRcInput WSBridge::parseRcInput(const std::string& body) {
    Bus::MsgRcInput rc;
    if (body.empty()) return rc;

    auto findValue = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = body.find(search);
        if (pos == std::string::npos) return "";
        pos = body.find(":", pos + search.size());
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
        std::string val;
        if (pos < body.size() && body[pos] == '"') {
            pos++;
            while (pos < body.size() && body[pos] != '"') val += body[pos++];
        } else {
            while (pos < body.size() && body[pos] != ',' && body[pos] != '}' && body[pos] != '\n') {
                val += body[pos++];
            }
        }
        return val;
    };

    std::string action = findValue("action");
    std::string throttle_str = findValue("throttle");

    if (!throttle_str.empty()) {
        rc.throttle = std::stof(throttle_str);
    }

    if (action == "arm")      rc.arm = true;
    if (action == "takeoff")  { rc.arm = true; rc.takeoff = true; }
    if (action == "land")     rc.land = true;
    if (action == "emergency") rc.emergency = true;
    if (action == "hover")    rc.throttle = 0.5f;

    rc.ts = now_us();
    return rc;
}

// ════════════════════════════════════════════════════════════════
//  HTTP响应构造
// ════════════════════════════════════════════════════════════════

std::string WSBridge::httpResponse(int code, const std::string& content_type,
                                     const std::string& body) {
    const char* status = (code == 200) ? "OK" : (code == 404) ? "Not Found" : "Error";
    std::string resp;
    resp += "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    resp += corsHeaders();
    resp += "Content-Type: " + content_type + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}

std::string WSBridge::corsHeaders() {
    return "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type\r\n";
}

} // namespace FlyteOS::Bridge
