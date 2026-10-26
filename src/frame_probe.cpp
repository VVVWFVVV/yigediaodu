#include "frame_probe.hpp"

#include <ctype.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "log.hpp"
#include "nodes.hpp"
#include "paths.hpp"
#include "state.hpp"

namespace xdd {
namespace {

using Clock = std::chrono::steady_clock;

const char* kTraceDir = "/sys/kernel/tracing";
const char* kProbeName = "xdd_frame";
const long long kSampleMs = 1000;  // 采样节流
const long long kDeadMs = 8000;    // 启动后计数从未变化的自校验阈值（8s：包容
                                   // 慢加载游戏的首帧前静默，又够快识破偏移失效）

std::string uprobe_events_path() { return std::string(kTraceDir) + "/uprobe_events"; }
std::string profile_path() { return std::string(kTraceDir) + "/uprobe_profile"; }
std::string enable_path() {
    return std::string(kTraceDir) + "/events/uprobes/" + kProbeName + "/enable";
}

bool g_active = false;
bool g_gave_up = false;  // 写入失败/自校验失败后本进程不再重试（防刷屏），
                         // 仅 daemon 重启解除
long long g_last_count = -1;
Clock::time_point g_last_sample{};
Clock::time_point g_started{};
Clock::time_point g_last_move{};  // 最近一次计数增长的时刻（初值=启动时刻）
int g_last_fps = -1;

// 裸写（不经 nd_write 的回读自证——uprobe_events 读回内容是内核规范化后的
// 注册行，与写入内容必然不一致，回读比对会误判失败）
bool raw_write(const std::string& logic_path, const std::string& content) {
    std::string real = nd_path(logic_path);
    FILE* f = std::fopen(real.c_str(), "w");
    if (!f) return false;
    bool ok = std::fputs(content.c_str(), f) != EOF;
    if (ok && std::fflush(f) != 0) ok = false;
    std::fclose(f);
    return ok;
}

// 从 uprobe_profile 解析本探针事件的计数（-1=读不到/没有我们的行）。
// 行格式："  <lib_path> <event_name>  <count>"，计数是行尾最后一个 token。
long long read_probe_count() {
    std::string text;
    if (!nd_read(profile_path(), text)) return -1;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find(kProbeName) == std::string::npos) continue;
        std::istringstream ls(line);
        std::string tok, last;
        while (ls >> tok) last = tok;
        if (last.empty()) continue;
        char* end = nullptr;
        long long v = std::strtoll(last.c_str(), &end, 10);
        if (end && *end == '\0' && v >= 0) return v;
    }
    return -1;
}

}  // namespace

bool load_frame_probe_config(FrameProbeConfig& out, const Conf* overrides) {
    auto pr = conf_parse_file(settings_path());
    const Conf& kv = pr.kv;
    std::string v;
    if (conf_get(kv, "fps.frame_probe", v)) out.enable = (v != "0" && v != "false");
    if (conf_get(kv, "fps.target_fps", v)) {
        int t = std::atoi(v.c_str());
        if (t >= 30 && t <= 240) out.target_fps = t;
    }
    if (conf_get(kv, "fps.low_ratio", v)) {
        int r = std::atoi(v.c_str());
        if (r >= 50 && r <= 100) out.low_ratio = r;
    }
    if (conf_get(kv, "frame.lib_path", v) && !v.empty()) out.lib_path = v;
    if (conf_get(kv, "frame.symbol_off", v) && !v.empty()) {
        out.symbol_off = std::strtoll(v.c_str(), nullptr, 0);  // 支持 0x 前缀
    }
    // perapp 覆盖（与 fps.* 阶梯同一通道：档位在前、perapp 在后）
    if (overrides) {
        for (const auto& kvp : *overrides) {
            const std::string& k = kvp.first;
            const std::string& val = kvp.second;
            if (k == "fps.frame_probe") {
                out.enable = (val != "0" && val != "false");
            } else if (k == "fps.target_fps") {
                int t = std::atoi(val.c_str());
                if (t >= 30 && t <= 240) out.target_fps = t;
            } else if (k == "fps.low_ratio") {
                int r = std::atoi(val.c_str());
                if (r >= 50 && r <= 100) out.low_ratio = r;
            } else if (k == "frame.lib_path" && !val.empty()) {
                out.lib_path = val;
            } else if (k == "frame.symbol_off" && !val.empty()) {
                out.symbol_off = std::strtoll(val.c_str(), nullptr, 0);
            }
        }
    }
    return true;
}

bool frame_probe_start(const FrameProbeConfig& cfg) {
    if (g_active) return true;
    if (g_gave_up) return false;
    if (cfg.lib_path.empty() || cfg.symbol_off <= 0) {
        g_gave_up = true;
        return false;
    }

    // 幂等：先删可能残留的旧事件（daemon 异常被杀后事件还留在内核里）
    raw_write(uprobe_events_path(), "-:" + std::string(kProbeName));

    char line[512];
    std::snprintf(line, sizeof(line), "p:%s %s:0x%llx", kProbeName, cfg.lib_path.c_str(),
                  (unsigned long long)cfg.symbol_off);
    if (!raw_write(uprobe_events_path(), line)) {
        log_line(LogLevel::Warn,
                 "帧探针创建失败（tracefs 不可写或目标库被占用），回退卡顿率阶梯");
        g_gave_up = true;
        return false;
    }
    if (!raw_write(enable_path(), "1")) {
        log_line(LogLevel::Warn, "帧探针启用失败，回退卡顿率阶梯");
        raw_write(uprobe_events_path(), "-:" + std::string(kProbeName));
        g_gave_up = true;
        return false;
    }

    g_active = true;
    g_gave_up = false;
    g_last_count = -1;
    g_last_fps = -1;
    g_last_sample = Clock::now();
    g_started = Clock::now();
    g_last_move = g_started;
    char off[32];
    std::snprintf(off, sizeof(off), "0x%llx", (unsigned long long)cfg.symbol_off);
    log_line(LogLevel::Info, "帧探针已启动：" + cfg.lib_path + " " + off +
                                 "（目标 " + std::to_string(cfg.target_fps) + "fps）");
    return true;
}

void frame_probe_stop() {
    if (g_active) {
        raw_write(enable_path(), "0");
        raw_write(uprobe_events_path(), "-:" + std::string(kProbeName));
        log_line(LogLevel::Info, "帧探针已停止");
    }
    g_active = false;
    g_last_count = -1;
    g_last_fps = -1;
}

bool frame_probe_active() { return g_active; }

void frame_probe_tick() {
    if (!g_active) return;
    Clock::time_point now = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_sample)
                       .count();
    if (elapsed < kSampleMs) return;
    g_last_sample = now;

    long long count = read_probe_count();
    if (count < 0) return;  // profile 暂不可读：保持上次结论，不立即判死

    if (g_last_count < 0) {  // 首个采样点：只建基线
        g_last_count = count;
        return;
    }
    long long delta = count - g_last_count;
    if (delta < 0) {  // 计数重置（事件被外部重建）：重新建基线
        g_last_count = count;
        g_last_fps = -1;
        return;
    }
    if (delta > 0) {
        g_last_move = now;
        g_last_fps = (int)(delta * 1000 / elapsed);
    } else {
        g_last_fps = 0;  // 有采样无帧：静止画面（电影/对话框）——有效低信号
    }
    g_last_count = count;

    // 自校验：启动至今计数从未变化（g_last_move 仍等于启动时刻且超过阈值）
    // = 符号没被调用（OTA 改了偏移/库路径）。只查「从未变化」——渲染中途的
    // 静止画面是合法 0 帧，不触罚。
    auto since_move =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_move).count();
    if (since_move >= kDeadMs &&
        g_last_move.time_since_epoch().count() == g_started.time_since_epoch().count()) {
        log_line(LogLevel::Warn,
                 "帧探针启动 8 秒无计数（符号偏移可能已失效），停用并回退卡顿率阶梯");
        frame_probe_stop();
        g_gave_up = true;
        set_capability("frame", "degraded");
    }
}

int frame_probe_last_fps() { return (g_active && g_last_fps >= 0) ? g_last_fps : -1; }

std::string frame_probe_state() {
    if (!g_active) return g_gave_up ? "degraded" : "off";
    return "ok";
}

}  // namespace xdd
