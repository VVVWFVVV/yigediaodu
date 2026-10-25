#include "fas.hpp"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "config.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "nodes.hpp"
#include "paths.hpp"
#include "state.hpp"

namespace xdd {

namespace {

using Clock = std::chrono::steady_clock;

// ==================== fas-rs 启动节流与状态管理 ====================
// v1.0.18: 失败后进入 5 分钟冷却期，避免每轮切应用都重试导致刷屏
static std::atomic<long long> g_fas_last_attempt_ms{0};
// pending 启动的起始时刻（>0 表示有一次「已 fork 但节点未就绪」的启动在途）
static std::atomic<long long> g_fas_pending_ms{0};
static std::atomic<int> g_fas_pending_pid{0};
static const long long FAS_RETRY_COOLDOWN_MS = 5 * 60 * 1000;  // 5 分钟
static const long long FAS_START_TIMEOUT_MS = 5000;            // 单次启动等待节点上限 5 秒
static const int FAS_MAX_FAILURES = 3;  // 连续失败阈值（超过则停止自动重试）

// v1.0.21：「已连续失败 N 次，暂停自动启动」日志的本会话一次性哨兵。
// 修复的刷屏：fas_start() 的计数器检查曾每次调用都打日志，而该检查在
// attempt 时间更新之前返回——冷却过期后主循环（约每秒一次）持续触发，
// 游戏前台期间每秒一条。计数器清零（daemon 重启/节点就绪）时同步复位。
static bool g_fas_strike_logged = false;

// 读/写 fas.retry_count
static int read_fas_retry_count() {
    std::string path = nd_path("/data/adb/WY925976742/state/fas.retry_count");
    std::ifstream f(path);
    if (!f.is_open()) return 0;
    int count = 0;
    f >> count;
    return count > 0 ? count : 0;
}

static void write_fas_retry_count(int count) {
    std::string path = nd_path("/data/adb/WY925976742/state/fas.retry_count");
    std::ofstream f(path);
    if (f.is_open()) f << count << "\n";
}

// 清理孤儿进程（pid 文件存在但进程已结束）
// per-app 配置目录（模块自身文件：真实路径，绝不加 XD_ROOT 前缀）
std::string perapp_dir() { return module_dir() + "/config/perapp"; }

// fas-rs 模式节点（设备状态路径：逻辑路径，nd_* 调用会加前缀）
const char* kModeNode = "/dev/fas_rs/mode";

// 运行时路径（逻辑路径，使用处经 nd_path/nd_read/nd_write 前缀化）
const char* kTomlPath = "/data/adb/WY925976742/config/fas_games.toml";
const char* kPidPath = "/data/adb/WY925976742/state/fas.pid";
const char* kLogPath = "/data/adb/WY925976742/log/fas.log";
const char* kCfgDir = "/data/adb/WY925976742/config";

// fas-rs 硬编码的用户配置（v1.0.19）：main.rs:50 `const USER_CONFIG`。
// Config::new 第一步即 fs::read_to_string(USER_CONFIG)?——文件缺失或 TOML 非法
// 都会让 run() 返回 Err，在 main.rs:66 被 unwrap 触发 panic（真机日志特征：
// panic location main.rs:66 + reason Unknown，且没有 "Config watcher started"）。
// 该文件同时是 fas-rs 的权威配置（ConfigThread 持续读取并整体替换 live config），
// 必须写完整合法内容而非空壳。写入用就地写（保留 inode）：fas-rs 以 inotify
// 监视该路径，原子 rename 会换 inode 导致其热加载失效。
const char* kUserTomlPath = "/sdcard/Android/fas-rs/games.toml";
// /sdcard 为 FUSE，早期/异常时可能不可写；/data/media/0 是同一存储的真实路径，作回退
const char* kUserTomlFallbackPath = "/data/media/0/Android/fas-rs/games.toml";

bool truthy_val(const std::string& v) {
    return v == "1" || v == "true" || v == "True" || v == "TRUE" || v == "yes" || v == "on";
}

// 目标帧率 -> 帧率候选数组（fas-rs 运行时按实际渲染帧率动态匹配）
// 例：120 -> "30, 60, 90, 120"；144 -> "60, 90, 120, 144"；60 -> "30, 60"
std::string fps_candidates(int target) {
    std::vector<int> cands;
    if (target <= 30) {
        cands = {30};
    } else if (target <= 60) {
        cands = {30, 60};
    } else if (target <= 90) {
        cands = {30, 60, 90};
    } else if (target <= 120) {
        cands = {30, 60, 90, 120};
    } else {
        cands = {60, 90, 120, target};
    }
    std::string out;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (i > 0) out += ", ";
        out += std::to_string(cands[i]);
    }
    return out;
}

// 读 pid 文件中的 pid（无效或进程不含 fas-rs 返回 -1，防 pid 复用）
int read_fas_pid() {
    std::ifstream f(nd_path(kPidPath));
    if (!f.is_open()) return -1;
    long pid = -1;
    f >> pid;
    if (pid <= 0) return -1;
    std::string cmd;
    if (!nd_read("/proc/" + std::to_string(pid) + "/cmdline", cmd)) return -1;
    if (cmd.find("fas-rs") == std::string::npos) return -1;
    return (int)pid;
}

void write_fas_pid(int pid) {
    FILE* f = std::fopen(nd_path(kPidPath).c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%d\n", pid);
    std::fclose(f);
}

// v1.0.21：扫 /proc 找活着的 fas-rs 进程——无 pid 文件或 pid 失效时的领养兜底
// （daemon 重启后 pid 文件丢失、或 pid 被复用指向别的进程的场景）。
// 僵尸进程的 /proc/<pid>/cmdline 为空，天然不会被匹配（判活自动排除僵尸）。
// 返回 pid，找不到返回 -1。
int find_fas_process() {
    DIR* d = ::opendir(nd_path("/proc").c_str());
    if (!d) return -1;
    struct dirent* e;
    int found = -1;
    while ((e = readdir(d)) != nullptr) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        std::string cmd;
        if (!nd_read(std::string("/proc/") + e->d_name + "/cmdline", cmd)) continue;
        if (cmd.find("fas-rs") != std::string::npos) {
            found = std::atoi(e->d_name);
            break;
        }
    }
    ::closedir(d);
    return found;
}

// 清理孤儿进程（pid 文件存在但进程已结束）
void cleanup_orphan_fas() {
    int pid = read_fas_pid();
    if (pid <= 0) return;
    std::string proc_path = "/proc/" + std::to_string(pid) + "/cmdline";
    if (!nd_exists(proc_path)) {
        ::unlink(nd_path(kPidPath).c_str());
        log_line(LogLevel::Info, "fas-rs 孤儿进程清理：pid " + std::to_string(pid) + " 已消失");
    }
}

// 递归创建逻辑路径的父目录（逐级 mkdir，EEXIST 忽略；host 模式经 nd_path 落 mock 树）
bool mkdirs_for(const std::string& logic_path) {
    std::string real = nd_path(logic_path);
    size_t pos = real.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return true;  // 根目录无需创建
    std::string dir = real.substr(0, pos);
    // 逐级创建（从根开始，天然支持多级缺失）
    std::string cur;
    size_t i = 0;
    while (i < dir.size()) {
        size_t next = dir.find('/', i + 1);
        if (next == std::string::npos) next = dir.size();
        cur = dir.substr(0, next);
        if (!cur.empty() && cur != "/") {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        i = next;
    }
    return true;
}

// 就地写（fopen "w" 截断 + 写 + fflush + fsync）：保留 inode，供 fas-rs 的
// inotify 监视收到 CLOSE_WRITE 事件做热加载。内部 toml 无监视者，仍用原子 rename。
bool write_in_place(const std::string& logic_path, const std::string& content) {
    if (!mkdirs_for(logic_path)) return false;
    FILE* f = std::fopen(nd_path(logic_path).c_str(), "w");
    if (!f) return false;
    bool ok = std::fputs(content.c_str(), f) != EOF;
    if (ok && std::fflush(f) != 0) ok = false;
    if (ok) {
        int fd = ::fileno(f);
        if (fd >= 0) ::fsync(fd);  // 断电安全：中途掉电也不会留下写了一半的配置
    }
    std::fclose(f);
    return ok;
}

// 把生成的 games.toml 内容写到 fas-rs 的 USER_CONFIG：
//   1) 优先 /sdcard/Android/fas-rs/games.toml（fas-rs 实际读取路径）
//   2) /sdcard 写失败（FUSE 未就绪等）回退 /data/media/0/同路径（同一存储真实路径）
// 返回实际写入成功的逻辑路径；都失败返回空串。
// 注意：写入成功路径必须与 fas-rs 读取路径一致——/sdcard 与 /data/media/0
// 在设备上指向同一文件，哪个能写就用哪个，无需两个都写。
std::string fas_write_user_toml(const std::string& content) {
    if (write_in_place(kUserTomlPath, content)) return kUserTomlPath;
    if (write_in_place(kUserTomlFallbackPath, content)) {
        log_line(LogLevel::Warn,
                 "sdcard 不可写，fas-rs 配置已回退写入 /data/media/0（功能不受影响）");
        return kUserTomlFallbackPath;
    }
    return "";
}

}  // namespace

std::string fas_binary_path() { return module_dir() + "/libs/arm64-v8a/fas-rs"; }

// 模块文件存在性：真实路径直查（module_dir 在两种模式下都是真实路径）
// v1.0.18 修正：改用 X_OK 同时检查文件存在与执行权限；若存在但无执行位，
// 自动 chmod 755 修复（KSU 安装后可能全 0644 真机实测），修复 fas-rs Permission denied
bool fas_binary_exists() {
    std::string path = fas_binary_path();
    if (::access(path.c_str(), F_OK) != 0) return false;
    if (::access(path.c_str(), X_OK) == 0) return true;
    // 存在但不可执行 → 自动修复
    if (::chmod(path.c_str(), 0755) == 0) {
        log_line(LogLevel::Info, "fas-rs 权限自愈：已恢复执行位");
        return true;
    }
    // chmod 失败则视为不可用
    return false;
}

std::string fas_toml_path() { return kTomlPath; }

// 重置 fas-rs 失败计数为 0（v1.0.20）。
// 修复：计数器曾只增不减、跨 daemon 重启 persisted——3 次失败后即使根因
// 已修复（如 v1.0.19 补写 USER_CONFIG）fas-rs 也永不再自动启动，而日志
// 一直承诺「下次重启 daemon 重置」。调用点：daemon 启动（cmd_daemon 持锁后）
// 与 fas_poll 节点就绪（成功后清零，崩溃后可重新获得完整重试额度）。
void fas_reset_retry_count() {
    write_fas_retry_count(0);
    g_fas_strike_logged = false;  // v1.0.21：计数器清零后允许再次提示暂停启动
}

bool fas_enabled_for(const std::string& pkg) {
    if (pkg.empty()) return false;
    Profile pa;
    if (!load_perapp(pkg, pa)) return false;
    std::string v;
    if (!conf_get(pa.kv, "fas.enable", v)) return false;
    return truthy_val(v);
}

int fas_generate_toml() {
    // 扫描 perapp 目录，收集 fas.enable=1 的应用
    std::vector<std::pair<std::string, int>> games;  // (pkg, target_fps)
    DIR* d = ::opendir(perapp_dir().c_str());  // 模块文件：真实路径，不加前缀
    if (d) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.size() < 6 || name.substr(name.size() - 5) != ".conf") continue;
            std::string pkg = name.substr(0, name.size() - 5);
            if (pkg.empty() || pkg[0] == '.') continue;
            if (!fas_enabled_for(pkg)) continue;
            int target = 60;
            Profile pa;
            if (load_perapp(pkg, pa)) {
                std::string fps;
                if (conf_get(pa.kv, "fas.target_fps", fps) && !fps.empty()) {
                    int t = std::atoi(fps.c_str());
                    if (t >= 30 && t <= 240) {
                        target = t;
                    } else {
                        log_line(LogLevel::Warn, "perapp " + pkg +
                                                     " fas.target_fps=" + fps +
                                                     " 越界（30-240），按 60 处理");
                    }
                }
            }
            games.emplace_back(pkg, target);
        }
        ::closedir(d);
    }
    std::sort(games.begin(), games.end());

    // 生成 TOML 内容
    std::ostringstream toml;
    toml << "# 由一个调度自动生成（v1.0.19），请勿手改\n"
         << "# 改法：编辑 config/perapp/<包名>.conf 的 fas.enable / fas.target_fps\n"
         << "# 本文件由守护进程同步写入 fas-rs 的用户配置 /sdcard/Android/fas-rs/games.toml\n"
         << "\n"
         << "[config]\n"
         << "keep_std = true\n"
         << "userspace_governor = false\n"
         << "scene_game_list = false\n"
         << "\n"
         << "[game_list]\n";
    for (const auto& g : games) {
        toml << "\"" << g.first << "\" = [" << fps_candidates(g.second) << "]\n";
    }
    toml << "\n"
         << "[powersave]\n"
         << "margin = 4\n"
         << "\n"
         << "[balance]\n"
         << "margin = 3\n"
         << "\n"
         << "[performance]\n"
         << "margin = 2\n"
         << "\n"
         << "[fast]\n"
         << "margin = 1\n";
    std::string content = toml.str();

    // 与现有文件相同则跳过（返回 -1，调用方无需重启 fas-rs）。
    // v1.0.19：去重扩展为「内部 toml 与 USER_CONFIG 都一致」——任一不符都要重写，
    // 否则 fas-rs 读到的仍是旧配置（USER_CONFIG 是它的权威配置）。
    std::string ex_internal, ex_user;
    bool same = nd_read(fas_toml_path(), ex_internal) && ex_internal == content &&
                nd_read(kUserTomlPath, ex_user) && ex_user == content;
    if (same) return -1;

    // 确保运行时 config 目录存在（首次运行/升级时可能没有）
    ::mkdir(nd_path(kCfgDir).c_str(), 0755);  // 已存在则 EEXIST，忽略

    // 原子写：tmp + rename
    std::string tmp = fas_toml_path() + ".tmp";
    FILE* f = std::fopen(nd_path(tmp).c_str(), "w");
    if (!f) {
        log_line(LogLevel::Warn, "fas games.toml 写入失败: " + tmp);
        return 0;
    }
    std::fputs(content.c_str(), f);
    std::fclose(f);
    ::rename(nd_path(tmp).c_str(), nd_path(fas_toml_path()).c_str());

    // v1.0.19：同步写 fas-rs 的 USER_CONFIG（就地写，保留 inode 供其 inotify 热加载）。
    // 该文件缺失时 fas-rs 启动即 panic（main.rs:66 unwrap），是必须同步的输出面。
    // 写失败不阻断 daemon：fas_start() 启动前会校验存在性，缺失则放弃启动并记失败。
    if (fas_write_user_toml(content).empty()) {
        log_line(LogLevel::Warn,
                 "fas-rs 用户配置写入失败（sdcard 与 /data/media/0 均不可写），"
                 "本次跳过 fas-rs 启动");
    }
    return (int)games.size();
}

// v1.0.21：判活 = mode 节点在 + fas-rs 进程真活着。
// 旧实现只查节点存在——fas-rs 崩溃后会留下残留节点，加上 daemon 从不 reap
// 形成的僵尸进程，已死的 fas-rs 会被判成「运行中」（假接管）：主循环把频率
// 维度让位给死人，游戏期间频率干预完全空缺（2026-10-24 真机两次复现，
// 现场：/dev/fas_rs/mode 残留 + state/fas.pid 指着僵尸 31568）。
// 进程判活走 read_fas_pid()（已含 cmdline 校验防 pid 复用；僵尸 cmdline 为
// 空天然通不过）；pid 文件缺失/失效时扫 /proc 领养孤儿（daemon 重启场景）。
bool fas_running() {
    if (!nd_exists(kModeNode)) return false;
    int pid = read_fas_pid();
    if (pid > 0) return true;
    pid = find_fas_process();
    if (pid > 0) {
        write_fas_pid(pid);  // 补写 pid 文件，后续判活走快路径
        return true;
    }
    return false;
}

// 检查 pending 启动状态：如果节点已就绪则清 pending；如果超时则记失败、清 pid。
// 返回 true 表示「已在运行」，false 表示「未就绪/超时」。
// 调用方可根据返回值决定是否记录失败计数（仅在真正超时失败时记）。
bool fas_poll() {
    if (fas_running()) {
        // 节点就绪：仅当此前有 pending 启动时才记录一次（避免稳态刷屏）
        if (g_fas_pending_ms.load() > 0) {
            g_fas_pending_ms.store(0);
            g_fas_pending_pid.store(0);
            // v1.0.20：成功就绪即清零失败计数——崩溃重启后可重新获得完整重试额度
            write_fas_retry_count(0);
            log_line(LogLevel::Info, "fas-rs 节点就绪，接管完成");
        }
        return true;
    }

    long long pending_ms = g_fas_pending_ms.load();
    if (pending_ms <= 0) return false;  // 无 pending 启动

    // 检查是否超时
    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now().time_since_epoch())
                        .count();
    if ((now - pending_ms) >= FAS_START_TIMEOUT_MS) {
        // 超时：清理 pid 文件，记录失败计数，清除 pending
        int pid = g_fas_pending_pid.load();
        ::unlink(nd_path(kPidPath).c_str());
        if (pid > 0) {
            ::kill((pid_t)pid, SIGKILL);
            ::waitpid((pid_t)pid, nullptr, WNOHANG);
        }
        g_fas_pending_ms.store(0);
        g_fas_pending_pid.store(0);
        write_fas_retry_count(read_fas_retry_count() + 1);
        log_line(LogLevel::Warn, "fas-rs 启动超时（5 秒内节点未就绪），回退内置稳帧阶梯");
        return false;
    }

    // 仍在等待中，未超时
    return false;
}

// 异步启动入口：立即返回，不阻塞主循环
// 返回 true 表示「已发起启动」（pending 状态）或「已在运行」；false 表示「放弃/跳过」
bool fas_start() {
    if (fas_running()) return true;

    // host 模式（测试）不启动
    if (host_mode()) return false;

    // 1) 清理潜在孤儿进程
    cleanup_orphan_fas();

    // 2) 检查二进制
    std::string bin = fas_binary_path();
    if (!fas_binary_exists()) {
        return false;  // 二进制不存在，静默回退
    }

    // 3) 节流检查：冷却期内直接返回 false（不打日志，避免刷屏）
    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now().time_since_epoch())
                        .count();
    long long last = g_fas_last_attempt_ms.load();
    if (last > 0 && (now - last) < FAS_RETRY_COOLDOWN_MS) {
        return false;  // 仍在冷却期
    }

    // 4) 检查失败计数阈值
    int failures = read_fas_retry_count();
    if (failures >= FAS_MAX_FAILURES) {
        // v1.0.21：一次性哨兵防刷屏（该检查在 attempt 时间更新前返回，
        // 冷却过期后主循环每秒都会走到这里）
        if (!g_fas_strike_logged) {
            g_fas_strike_logged = true;
            log_line(LogLevel::Info, "fas-rs 已连续失败 " + std::to_string(FAS_MAX_FAILURES) +
                                     " 次，本会话暂停自动启动（下次重启 daemon 重置）");
        }
        return false;
    }

    // 5) 记录本次尝试时间
    g_fas_last_attempt_ms.store(now);

    // 6) 确保 toml 最新
    fas_generate_toml();
    if (!nd_exists(fas_toml_path())) {
        log_line(LogLevel::Warn, "fas games.toml 不存在，跳过 fas-rs 启动");
        write_fas_retry_count(failures + 1);
        return false;
    }
    // 6b) v1.0.19：校验 fas-rs 的 USER_CONFIG 存在性。该文件缺失时 fas-rs 启动即
    // panic（main.rs:66 unwrap），等 5 秒超时才发现为时已晚——这里即时放弃并记失败。
    if (!nd_exists(kUserTomlPath)) {
        log_line(LogLevel::Warn,
                 "fas-rs 用户配置不存在（/sdcard/Android/fas-rs/games.toml），跳过启动");
        write_fas_retry_count(failures + 1);
        return false;
    }

    // 7) 备份 min_freq 基线（只在第一次启动时做）
    static bool baseline_taken = false;
    if (!baseline_taken) {
        fas_snapshot_freq_baseline();
        baseline_taken = true;
    }

    // 8) 后台 fork+exec（父进程立即写入 pid，子进程独立运行）
    int logfd = ::open(nd_path(kLogPath).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    pid_t pid = ::fork();
    if (pid < 0) {
        if (logfd >= 0) ::close(logfd);
        log_line(LogLevel::Warn, "fas-rs fork 失败");
        write_fas_retry_count(failures + 1);
        return false;
    }
    if (pid == 0) {
        // 子进程：独立后台运行 fas-rs
        ::setsid();
        if (logfd >= 0) {
            ::dup2(logfd, STDOUT_FILENO);
            ::dup2(logfd, STDERR_FILENO);
            if (logfd > STDERR_FILENO) ::close(logfd);
        }
        std::string toml_real = nd_path(fas_toml_path());
        ::execl(bin.c_str(), bin.c_str(), "run", toml_real.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    if (logfd >= 0) ::close(logfd);

    // 父进程立即写入 pid 文件（即使节点还未就绪），防止主循环重复启动
    write_fas_pid((int)pid);

    // 记录 pending 状态（用于后续轮询判断超时）
    long long start_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              Clock::now().time_since_epoch())
                              .count();
    g_fas_pending_ms.store(start_now);
    g_fas_pending_pid.store((int)pid);

    // 后续由主循环轮询 fas_running() 判断节点是否就绪
    log_line(LogLevel::Info, "fas-rs 启动中（pid " + std::to_string((int)pid) + "，等待节点就绪）");
    return true;  // pending
}

void fas_stop() {
    int pid = read_fas_pid();
    if (pid > 0) {
        ::kill((pid_t)pid, SIGTERM);
        // 等最多 1 秒优雅退出；waitpid 回收子进程防僵尸
        // （r=pid 已退出并回收；r<0 非本进程子进程——daemon 重启后的孤儿，
        //   靠 /proc 判断；r=0 还在跑，继续等）
        bool reaped = false;
        for (int i = 0; i < 10; ++i) {
            int st = 0;
            pid_t r = ::waitpid((pid_t)pid, &st, WNOHANG);
            if (r == (pid_t)pid) { reaped = true; break; }
            if (r < 0) break;
            ::usleep(100 * 1000);
        }
        if (!reaped && nd_exists("/proc/" + std::to_string(pid) + "/cmdline")) {
            ::kill((pid_t)pid, SIGKILL);
            ::waitpid((pid_t)pid, nullptr, WNOHANG);  // 尽力回收
        }
    }
    ::unlink(nd_path(kPidPath).c_str());
    // fas-rs 退出后把 min_freq 还原到接管前的值（防留在高频耗电）
    fas_restore_freq_baseline();
    log_line(LogLevel::Info, "fas-rs 已停止，频率节点已还原");
}

void fas_set_mode(const std::string& mode) {
    if (mode != "powersave" && mode != "balance" && mode != "performance" && mode != "fast") return;
    if (!fas_running()) return;
    nd_write(kModeNode, mode);
}

void fas_snapshot_freq_baseline() {
    // 对每个 cpufreq 策略组的 scaling_min_freq 记录当前值（只读，不写）
    for (const auto& p : detect_policies()) {
        baseline_snapshot_node(p.dir + "/scaling_min_freq");
    }
}

void fas_restore_freq_baseline() {
    // 从基线文件把所有 scaling_min_freq 条目写回
    baseline_restore_matching("scaling_min_freq");
}

// v1.0.21：僵尸回收 + stale 崩溃恢复（假接管根治）。主循环每次迭代、
// fas 托管判定之前调用；daemon 启动路径也调一次（清理上次会话残留）。
// 场景：fas-rs 启动后崩溃/被 kill——留下 /dev/fas_rs/mode 残留节点，pid
// 文件指向已死进程（甚至僵尸）。旧逻辑下 fas_running() 误判「运行中」，
// 频率维度被永久让位给死人。本函数：
//   1) waitpid(WNOHANG) 回收已死的 fas-rs 子进程（防僵尸堆积；
//      非本进程子进程时返回 -1，无副作用）
//   2) 确认 stale（节点在、进程不在）→ 还原频率基线（fas-rs 猝死前可能
//      已把 min_freq 调高）+ 清残留节点与 pid 文件 + 记一次失败（走既有
//      冷却/3 次阈值）+ 打明确诊断（已知根因指向设备树档案）
// 返回 true 表示发生了 stale 清理（频率现场已还原，调用方应重落地）。
bool fas_reap_and_check() {
    // 1) 回收已死子进程（僵尸的 /proc/<pid>/cmdline 为空，read_fas_pid 返回
    //    -1，waitpid 目标改用文件里的原始 pid 更稳——这里直接按 pid 文件值收）
    std::ifstream pf(nd_path(kPidPath));
    int file_pid = -1;
    if (pf.is_open()) {
        long v = -1;
        pf >> v;
        if (v > 0) file_pid = (int)v;
    }
    if (file_pid > 0) ::waitpid((pid_t)file_pid, nullptr, WNOHANG);

    // 2) 无残留节点：未接管或从未启动，正常
    if (!nd_exists(kModeNode)) return false;

    // 3) 节点在且进程活着（含孤儿领养）：正常接管中
    if (fas_running()) return false;

    // 4) stale：进程已死但节点残留——崩溃现场，清理并回退
    log_line(LogLevel::Warn,
             "fas-rs 已崩溃（mode 节点残留但进程不在），频率维度回退内置稳帧阶梯");
    log_line(LogLevel::Info,
             "诊断：fas-rs 在此设备不可用（已知根因：厂商内核 perf-uprobe+BPF 派发被"
             "阉割；Android 17 移除 queueBuffer 旧符号），详见 docs/device-tree-6d52ba83.md");
    fas_restore_freq_baseline();
    ::unlink(nd_path(kPidPath).c_str());
    ::unlink(nd_path(kModeNode).c_str());
    write_fas_retry_count(read_fas_retry_count() + 1);
    return true;
}

FasStatus fas_status() {
    FasStatus st;
    st.running = fas_running();
    if (st.running) {
        std::string m;
        if (nd_read(kModeNode, m) && !m.empty()) st.mode = m;
    }
    // 管理中的应用数 = games.toml 的 game_list 条目数
    std::string toml;
    if (nd_read(fas_toml_path(), toml)) {
        bool in_list = false;
        std::istringstream iss(toml);
        std::string line;
        while (std::getline(iss, line)) {
            std::string t = line;
            size_t b = t.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            t = t.substr(b);
            if (t.rfind("[game_list]", 0) == 0) { in_list = true; continue; }
            if (t.rfind("[", 0) == 0) { in_list = false; continue; }
            if (in_list && t[0] == '"') ++st.managed;
        }
    }
    return st;
}

}  // namespace xdd
