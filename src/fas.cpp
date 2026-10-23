#include "fas.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
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

// per-app 配置目录（模块自身文件：真实路径，绝不加 XD_ROOT 前缀）
std::string perapp_dir() { return module_dir() + "/config/perapp"; }

// fas-rs 模式节点（设备状态路径：逻辑路径，nd_* 调用会加前缀）
const char* kModeNode = "/dev/fas_rs/mode";

// 运行时路径（逻辑路径，使用处经 nd_path/nd_read/nd_write 前缀化）
const char* kTomlPath = "/data/adb/WY925976742/config/fas_games.toml";
const char* kPidPath = "/data/adb/WY925976742/state/fas.pid";
const char* kLogPath = "/data/adb/WY925976742/log/fas.log";
const char* kCfgDir = "/data/adb/WY925976742/config";

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

}  // namespace

std::string fas_binary_path() { return module_dir() + "/libs/arm64-v8a/fas-rs"; }

// 模块文件存在性：真实路径直查（module_dir 在两种模式下都是真实路径）
bool fas_binary_exists() { return ::access(fas_binary_path().c_str(), F_OK) == 0; }

std::string fas_toml_path() { return kTomlPath; }

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
    toml << "# 由一个调度自动生成（v1.0.17），请勿手改\n"
         << "# 改法：编辑 config/perapp/<包名>.conf 的 fas.enable / fas.target_fps\n"
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

    // 与现有文件相同则跳过（返回 -1，调用方无需重启 fas-rs）
    std::string existing;
    if (nd_read(fas_toml_path(), existing) && existing == content) return -1;

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
    return (int)games.size();
}

bool fas_running() { return nd_exists(kModeNode); }

bool fas_start() {
    if (fas_running()) return true;

    // host 模式（测试）不启动：真机二进制在模拟树上跑不起来（无 eBPF/
    // 真 /dev），只会白等 3 秒超时还阻塞 daemon 启动。测试用 mock 树里的
    // /dev/fas_rs/mode 节点直接模拟「fas-rs 在跑」即可。
    if (host_mode()) return false;

    std::string bin = fas_binary_path();
    if (!fas_binary_exists()) {
        // 二进制未打进模块（构建时下载失败或用户裁剪）——静默回退
        return false;
    }

    // 确保 games.toml 最新
    fas_generate_toml();
    if (!nd_exists(fas_toml_path())) {
        log_line(LogLevel::Warn, "fas games.toml 不存在，跳过 fas-rs 启动");
        return false;
    }

    // 接管前快照 min_freq 基线（只读），退出时还原
    fas_snapshot_freq_baseline();

    // fork + exec（子进程 setsid 脱离终端，stdout/stderr 重定向到日志）
    int logfd = ::open(nd_path(kLogPath).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    pid_t pid = ::fork();
    if (pid < 0) {
        if (logfd >= 0) ::close(logfd);
        log_line(LogLevel::Warn, "fas-rs fork 失败");
        return false;
    }
    if (pid == 0) {
        // 子进程
        ::setsid();
        if (logfd >= 0) {
            ::dup2(logfd, STDOUT_FILENO);
            ::dup2(logfd, STDERR_FILENO);
            if (logfd > STDERR_FILENO) ::close(logfd);
        }
        // 模块文件（二进制）真实路径；toml 是运行时路径，需 nd_path 前缀
        std::string toml_real = nd_path(fas_toml_path());
        ::execl(bin.c_str(), bin.c_str(), "run", toml_real.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    if (logfd >= 0) ::close(logfd);
    write_fas_pid((int)pid);

    // 等模式节点出现（fas-rs 就绪标志），最多 3 秒
    for (int i = 0; i < 30; ++i) {
        if (fas_running()) {
            log_line(LogLevel::Info, "fas-rs 帧感知调度已启动（pid " + std::to_string((int)pid) + "）");
            return true;
        }
        ::usleep(100 * 1000);
    }
    log_line(LogLevel::Warn, "fas-rs 启动超时（内核 eBPF 支持？），回退内置稳帧阶梯");
    return false;
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
