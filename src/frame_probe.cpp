#include "frame_probe.hpp"

#include <ctype.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "elf_symbol.hpp"
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
const long long kDeadMs = 20000;   // 启动后计数从未变化的自校验阈值（20s：包容
                                   // 慢加载/静止首屏，又够快识破偏移失效）
const long long kRetryMs = 10 * 60 * 1000;  // 失败后重试间隔（防刷屏）

std::string uprobe_events_path() { return std::string(kTraceDir) + "/uprobe_events"; }
std::string profile_path() { return std::string(kTraceDir) + "/uprobe_profile"; }
std::string enable_path() {
    return std::string(kTraceDir) + "/events/uprobes/" + kProbeName + "/enable";
}

bool g_active = false;
bool g_gave_up = false;  // 写入失败/自校验失败后本进程暂停重试（防刷屏），
                         // 10 分钟后或热重载/重启解除
long long g_gave_up_ms = 0;
long long g_last_count = -1;
Clock::time_point g_last_sample{};
Clock::time_point g_started{};
Clock::time_point g_last_move{};  // 最近一次计数增长的时刻（初值=启动时刻）
int g_last_fps = -1;

// 相对基线快信号（v1.2.0）：计数含设备上其它 GL 消费者，绝对值不能直接
// 和目标帧率比；用启动后前 5 个采样的峰值作为健康基线，之后只升不降。
// 连续 2 次低于基线 80% 判骤降，回到 90% 以上解除。
int g_ref_fps = -1;
int g_warmup_samples = 0;
int g_warmup_max = 0;
int g_drop_hits = 0;
bool g_drop = false;
// 最近 12 个 1s 采样的环形窗口（判断当前是否真的在出帧）
int g_recent[12] = {0};
int g_recent_n = 0;
int g_recent_i = 0;

long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

void mark_gave_up() {
    g_gave_up = true;
    g_gave_up_ms = steady_ms();
}

void push_recent_fps(int fps) {
    g_recent[g_recent_i] = fps > 0 ? fps : 0;
    g_recent_i = (g_recent_i + 1) % 12;
    if (g_recent_n < 12) ++g_recent_n;
}

int recent_max_fps() {
    int m = 0;
    for (int i = 0; i < g_recent_n; ++i) {
        if (g_recent[i] > m) m = g_recent[i];
    }
    return m;
}

void reset_recent_fps() {
    for (auto& v : g_recent) v = 0;
    g_recent_n = 0;
    g_recent_i = 0;
}

std::string hex_off(long long v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v);
    return std::string(b);
}

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
    if (g_gave_up) {
        // 粘性失败只保持 10 分钟；配置热重载会直接清掉（见 frame_probe_reset）
        if (g_gave_up_ms > 0 && steady_ms() - g_gave_up_ms < kRetryMs) return false;
        g_gave_up = false;
    }
    if (cfg.lib_path.empty() || cfg.symbol_off <= 0) {
        mark_gave_up();
        return false;
    }

    // 幂等：先删可能残留的旧事件（daemon 异常被杀后事件还留在内核里）
    raw_write(uprobe_events_path(), "-:" + std::string(kProbeName));

    // v1.2.0：优先运行时解析 ELF 动态符号。硬编码偏移在 OTA/换库后会静默失效，
    // 只能等 20s 自校验停用；这里在注册前就把 eglSwapBuffers 的真实文件偏移找出来，
    // 解析失败才回退 frame.symbol_off。
    std::string lib = cfg.lib_path;
    long long off = elf_resolve_symbol_offset(nd_path(lib), "eglSwapBuffers");
    bool auto_resolved = off > 0;
    if (off <= 0) {
        const char* candidates[] = {
            "/vendor/lib64/libEGL_adreno.so",
            "/vendor/lib64/egl/libEGL_adreno.so",
            "/system/lib64/libEGL_adreno.so",
        };
        for (const char* cand : candidates) {
            if (lib == cand) continue;
            long long o = elf_resolve_symbol_offset(nd_path(cand), "eglSwapBuffers");
            if (o > 0) {
                lib = cand;
                off = o;
                auto_resolved = true;
                break;
            }
        }
    }
    if (off > 0) {
        log_line(LogLevel::Info, "帧探针符号: " + lib + " eglSwapBuffers@" + hex_off(off) +
                                     (auto_resolved ? "（ELF 自动解析）" : "（配置值）"));
    } else {
        off = cfg.symbol_off;
        log_line(LogLevel::Info, "帧探针符号自动解析失败，回退配置偏移: " + lib + " " +
                                     hex_off(off));
    }
    if (lib.empty() || off <= 0) {
        mark_gave_up();
        return false;
    }

    char line[512];
    std::snprintf(line, sizeof(line), "p:%s %s:0x%llx", kProbeName, lib.c_str(),
                  (unsigned long long)off);
    if (!raw_write(uprobe_events_path(), line)) {
        log_line(LogLevel::Warn,
                 "帧探针创建失败（tracefs 不可写或目标库被占用），回退卡顿率阶梯");
        mark_gave_up();
        return false;
    }
    if (!raw_write(enable_path(), "1")) {
        log_line(LogLevel::Warn, "帧探针启用失败，回退卡顿率阶梯");
        raw_write(uprobe_events_path(), "-:" + std::string(kProbeName));
        mark_gave_up();
        return false;
    }

    g_active = true;
    g_gave_up = false;
    g_gave_up_ms = 0;
    g_last_count = -1;
    g_last_fps = -1;
    g_ref_fps = -1;
    g_warmup_samples = 0;
    g_warmup_max = 0;
    g_drop_hits = 0;
    g_drop = false;
    reset_recent_fps();
    g_last_sample = Clock::now();
    g_started = Clock::now();
    g_last_move = g_started;
    log_line(LogLevel::Info, "帧探针已启动：" + lib + " " + hex_off(off) +
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
    g_ref_fps = -1;
    g_warmup_samples = 0;
    g_warmup_max = 0;
    g_drop_hits = 0;
    g_drop = false;
    reset_recent_fps();
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
        g_ref_fps = -1;
        g_warmup_samples = 0;
        g_warmup_max = 0;
        g_drop_hits = 0;
        g_drop = false;
        reset_recent_fps();
        return;
    }
    if (delta > 0) {
        g_last_move = now;
        g_last_fps = (int)(delta * 1000 / elapsed);
    } else {
        g_last_fps = 0;  // 有采样无帧：静止画面（电影/对话框）——有效低信号
    }
    g_last_count = count;

    push_recent_fps(g_last_fps);
    // 相对基线快信号：前 5 个有效采样取峰值作为健康基线（游戏冷启动/菜单
    // 阶段帧率可能偏低，用峰值避免把低基线当成正常）；之后只升不降。
    if (g_last_fps > 0) {
        if (g_ref_fps < 0) {
            if (g_last_fps > g_warmup_max) g_warmup_max = g_last_fps;
            if (++g_warmup_samples >= 5) g_ref_fps = g_warmup_max;
        } else {
            if (g_last_fps > g_ref_fps) g_ref_fps = g_last_fps;
            if (g_last_fps * 100 < g_ref_fps * 80) {
                if (++g_drop_hits >= 2) g_drop = true;
            } else {
                g_drop_hits = 0;
                if (g_last_fps * 100 >= g_ref_fps * 90) g_drop = false;
            }
        }
    }

    // 自校验：启动至今计数从未变化（g_last_move 仍等于启动时刻且超过阈值）
    // = 符号没被调用（OTA 改了偏移/库路径）。只查「从未变化」——渲染中途的
    // 静止画面是合法 0 帧，不触罚。
    auto since_move =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_move).count();
    if (since_move >= kDeadMs &&
        g_last_move.time_since_epoch().count() == g_started.time_since_epoch().count()) {
        log_line(LogLevel::Warn,
                 "帧探针启动 20 秒无计数（符号偏移可能已失效），停用并回退卡顿率阶梯");
        frame_probe_stop();
        mark_gave_up();
        set_capability("frame", "degraded");
    }
}

void frame_probe_reset() {
    frame_probe_stop();
    g_gave_up = false;
    g_gave_up_ms = 0;
}

int frame_probe_last_fps() { return (g_active && g_last_fps >= 0) ? g_last_fps : -1; }

bool frame_probe_drop() { return g_active && g_drop; }

int frame_probe_ref_fps() { return g_active ? g_ref_fps : -1; }

int frame_probe_recent_max() { return g_active ? recent_max_fps() : -1; }

int frame_probe_age_ms() {
    if (!g_active) return -1;
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_started)
        .count();
}

std::string frame_probe_state() {
    if (!g_active) return g_gave_up ? "degraded" : "off";
    return "ok";
}

}  // namespace xdd
