#include "monitor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdlib.h>
#include <unistd.h>

#include "log.hpp"
#include "nodes.hpp"
#include "paths.hpp"
#include "shell.hpp"

namespace xdd {

namespace {

// 内置游戏包名前缀：覆盖主流大厂；漏网应用由 perapp 配置显式指定 game
const char* kGamePrefixes[] = {
    "com.tencent.tmgp",   "com.tencent.ig",     "com.tencent.lolm",
    "com.tencent.jkchess","com.tencent.KiHan",  "com.tencent.czn",
    "com.tencent.nrc",    "com.tencent.ngr",    "com.tencent.mf.uam",
    "com.miHoYo.",        "com.HoYoverse.",     "com.netease.",
    "com.kurogame.",      "com.levelinfinite.", "com.garena.",
    "com.pubg.",          "com.rekoo.pubgm",    "com.proximabeta.",
    "com.hypergryph.",    "com.roblox.",        "com.ea.gp.",
    "com.soulgamechst.",  "com.idreamsky.",     "com.longe.",
    "com.hottagames.",    "com.manjuu.",
};

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 形如 Android 包名：含点、仅包名字符、不以点开头、长度合理
bool looks_like_package(const std::string& s) {
    if (s.size() < 5 || s.size() > 128) return false;
    if (s.find('.') == std::string::npos) return false;
    if (s[0] == '.' || s[0] == '/') return false;
    for (char c : s) {
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_')) return false;
    }
    return true;
}

// /proc 读取统一走 nd_read（含 XD_ROOT 前缀与 trim），此处只做行/段切分
bool read_first_line(const std::string& path, std::string& out) {
    std::string raw;
    if (!nd_read(path, raw)) return false;
    size_t nl = raw.find('\n');
    out = trim(nl == std::string::npos ? raw : raw.substr(0, nl));
    return true;
}

// cmdline 以 NUL 分段，取第一段
std::string read_cmdline(const std::string& path) {
    std::string raw;
    if (!nd_read(path, raw)) return std::string();
    size_t nul = raw.find('\0');
    std::string first = (nul == std::string::npos) ? raw : raw.substr(0, nul);
    return trim(first);
}

// dumpsys 输出里提取 mResumedActivity 的包名
std::string parse_resumed_pkg(const std::string& text) {
    size_t pos = text.find("mResumedActivity");
    if (pos == std::string::npos) return std::string();
    size_t u0 = text.find(" u0 ", pos);
    if (u0 == std::string::npos) return std::string();
    u0 += 4;
    size_t slash = text.find('/', u0);
    if (slash == std::string::npos) return std::string();
    std::string pkg = text.substr(u0, slash - u0);
    return trim(pkg);
}

using Clock = std::chrono::steady_clock;

// 节流辅助：距上次调用不足 interval_ms 返回 false
bool throttled(Clock::time_point& last, int interval_ms) {
    auto now = Clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (ms < interval_ms) return true;
    last = now;
    return false;
}

Clock::time_point g_last_ds_activity{};
Clock::time_point g_last_ds_power{};
bool g_screen_cached = true;

}  // namespace

bool is_builtin_game(const std::string& pkg) {
    for (const char* pre : kGamePrefixes) {
        if (pkg.rfind(pre, 0) == 0) return true;
    }
    return false;
}

std::string resolve_family(const std::string& pkg, bool screen_on_now, const std::string& perapp_mode) {
    // perapp 显式档位优先（含 off）
    if (!perapp_mode.empty()) return perapp_mode;
    if (!screen_on_now) return "bg";
    if (pkg.empty()) return "daily";
    return is_builtin_game(pkg) ? "game" : "daily";
}

ForegroundInfo detect_foreground() {
    ForegroundInfo best;    // min-adj 兜底候选
    ForegroundInfo topbest; // top-app cgroup 候选（真正前台应用）

    // ---- /proc 扫描 ----
    // 双候选结构：Android 前台应用的进程必在 top-app cpuset（cgroup 行含
    // "top-app"），而媒体/转码等服务进程（如 media.hwcodec）adj 虽低却不在
    // top-app——旧版只按 min-adj 选取会误报。top 候选集非空时只用它，
    // 为空才退回 min-adj 启发式。
    // 注意：read_* 内部走 nd_read（会拼 XD_ROOT 前缀），必须传逻辑路径。
    const std::string proc = nd_path("/proc");
    DIR* d = ::opendir(proc.c_str());
    if (d) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            const char* n = e->d_name;
            if (*n == '\0' || !std::isdigit((unsigned char)*n)) continue;
            int pid = std::atoi(n);
            if (pid <= 0) continue;

            std::string cmdline = read_cmdline("/proc/" + std::string(n) + "/cmdline");
            if (!looks_like_package(cmdline)) continue;

            std::string adj_s;
            if (!read_first_line("/proc/" + std::string(n) + "/oom_score_adj", adj_s)) continue;
            int adj = std::atoi(adj_s.c_str());

            // 前台/可见阈值：adj <= 200
            if (adj > 200) continue;

            // cgroup 判定：内容含 "top-app" 即真正前台应用
            std::string cg;
            bool is_top = false;
            if (nd_read("/proc/" + std::string(n) + "/cgroup", cg) &&
                cg.find("top-app") != std::string::npos) {
                is_top = true;
            }

            // 同分保持现状（不来回跳），仅更优时替换
            ForegroundInfo& slot = is_top ? topbest : best;
            bool better = slot.pkg.empty() || adj < slot.oom_adj;
            if (better) {
                slot.pkg = cmdline;
                slot.oom_adj = adj;
                slot.from_dumpsys = false;
            }
        }
        ::closedir(d);
    }

    if (!topbest.pkg.empty()) return topbest;
    if (!best.pkg.empty()) return best;

    // ---- dumpsys 兜底（节流 5s，带超时） ----
    if (!throttled(g_last_ds_activity, 5000)) {
        auto sr = shell_run("dumpsys activity activities", 3000);
        if (!sr.timed_out) {
            std::string pkg = parse_resumed_pkg(sr.out);
            if (!pkg.empty()) {
                best.pkg = pkg;
                best.oom_adj = 0;
                best.from_dumpsys = true;
            }
        }
    }
    return best;
}

bool screen_on() {
    // 亮屏时 5s 查一次即可；息屏时缩短到 2s，保证亮屏后尽快恢复前台识别。
    // 息屏期间主循环不扫 /proc，这里的一次 dumpsys 是唯一周期开销。
    int interval = g_screen_cached ? 5000 : 2000;
    if (throttled(g_last_ds_power, interval)) return g_screen_cached;
    auto sr = shell_run("dumpsys power", 3000);
    if (sr.timed_out) return g_screen_cached;  // 超时沿用上次结论，不猜测
    g_screen_cached = (sr.out.find("mWakefulness=Awake") != std::string::npos);
    return g_screen_cached;
}

}  // namespace xdd
