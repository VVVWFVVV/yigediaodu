// ============================================================
// 一个调度 · 守护进程入口
// CLI 契约（fcctl.sh / service.sh 只依赖这些子命令）：
//   --daemon            守护模式：主循环 + 心跳
//   --status            单行 JSON 状态（WebUI 只读面）
//   --selfcheck         启动自检，可读报告，失败非零退出
//   --restore           按 baseline 快照还原节点
//   --apply <profile>   立即应用指定档位
//   --version           版本信息
//   --probe fg/screen/family/fps/fas   诊断探针
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <pthread.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>

#include "config.hpp"
#include "engine.hpp"
#include "fas.hpp"
#include "fps.hpp"
#include "log.hpp"
#include "monitor.hpp"
#include "nodes.hpp"
#include "paths.hpp"
#include "shell.hpp"
#include "state.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
static std::atomic<bool> g_stop_atomic{false};

void on_signal(int) {
    g_stop = 1;
    g_stop_atomic.store(true);  // 看门狗线程据此退出
}

// 二进制自报版本：必须与 module.prop 的 version= 一致。
// tools/build.ps1 在每次构建前会自动从 module.prop 同步此行（防漂移）；
// 若手动编译忘记同步，run_tests.sh 的版本一致性断言会抓到。
const char* kVersion = "1.0.21";

void usage() {
    std::fprintf(stderr,
                 "用法: xindiaodud <--daemon|--status|--selfcheck|--restore|--apply <profile>|--version>\n");
}

int cmd_version() {
    std::printf("xindiaodud %s (%s)\n", kVersion, xdd::host_mode() ? "host" : "device");
    return 0;
}

// ---------------- 启动自检 ----------------
// 任何一步失败都输出可读原因并以非零退出，让监督者与用户知道发生了什么
int cmd_selfcheck() {
    int fail = 0;
    std::printf("[selfcheck] 模式: %s\n", xdd::host_mode() ? "host(模拟设备树)" : "device(真实设备)");
    std::printf("[selfcheck] 模块目录: %s\n", xdd::module_dir().c_str());

    // 1) 全局配置可解析
    std::string sp = xdd::settings_path();
    std::ifstream sf(sp);
    if (sf.is_open()) {
        auto pr = xdd::conf_parse_file(sp);
        if (!pr.bad_lines.empty()) {
            std::printf("[selfcheck] FAIL settings.conf 有 %zu 行无法解析:\n", pr.bad_lines.size());
            for (const auto& l : pr.bad_lines) std::printf("    %s\n", l.c_str());
            ++fail;
        } else {
            std::printf("[selfcheck] OK settings.conf 解析通过（%zu 项）\n", pr.kv.size());
        }
    } else {
        std::printf("[selfcheck] WARN settings.conf 不存在（将由骨架脚本补种）: %s\n", sp.c_str());
    }

    // 2) cpufreq 策略组可探测
    auto policies = xdd::detect_policies();
    if (policies.empty()) {
        std::printf("[selfcheck] FAIL 未探测到 cpufreq 策略组\n");
        ++fail;
    } else {
        std::printf("[selfcheck] OK 探测到 %zu 个 cpufreq 策略组:\n", policies.size());
        for (const auto& p : policies) {
            std::printf("    %s cpus=%s min=%lld max=%lld\n", p.dir.c_str(), p.cpus.c_str(),
                        p.min_freq, p.max_freq);
        }
    }

    // 3) 温度可读
    xdd::TempInfo ti;
    if (xdd::read_max_temp(ti)) {
        std::printf("[selfcheck] OK 最高温度 %d.%03d℃ (zone=%s)\n", ti.max_milli / 1000,
                    ti.max_milli % 1000, ti.zone.c_str());
    } else {
        std::printf("[selfcheck] WARN 温度节点不可读\n");
    }

    // 4) 运行时目录可写
    std::string home = xdd::runtime_dir();
    FILE* probe = std::fopen((home + "/.probe").c_str(), "w");
    if (probe) {
        std::fclose(probe);
        std::remove((home + "/.probe").c_str());
        std::printf("[selfcheck] OK 运行时目录可写: %s\n", home.c_str());
    } else {
        std::printf("[selfcheck] WARN 运行时目录不可写（骨架脚本应已创建）: %s\n", home.c_str());
    }

    // 4b) GPU devfreq 探测（v1.0.16：gpu.min_freq 依赖；缺失仅 WARN，
    // 游戏档的 GPU 下限会降级，其余干预不受影响）
    {
        const std::string& gdir = xdd::gpu_devfreq_dir();
        if (gdir.empty()) {
            std::printf("[selfcheck] WARN 未探测到 GPU devfreq（游戏档 GPU 频率下限将降级）\n");
        } else {
            std::string avail;
            xdd::nd_read(gdir + "/available_frequencies", avail);  // nd_read 已 trim
            std::printf("[selfcheck] OK GPU devfreq: %s（%s）\n", gdir.c_str(),
                        avail.empty() ? "频率表不可读" : (avail.substr(0, 60) + "...").c_str());
        }
    }

    // 5) 外部命令通道（dumpsys）可达且带超时
    auto sr = xdd::shell_run("dumpsys power", 3000);
    if (sr.timed_out) {
        std::printf("[selfcheck] WARN dumpsys 采样超时（设备端可能较慢，不影响核心调度）\n");
    } else if (sr.ok) {
        std::printf("[selfcheck] OK 外部命令通道可用（dumpsys power）\n");
    } else {
        std::printf("[selfcheck] WARN dumpsys power 退出码 %d\n", sr.exit_code);
    }

    if (fail > 0) {
        std::printf("[selfcheck] FAIL（%d 项失败）\n", fail);
        return 1;
    }
    std::printf("[selfcheck] PASS\n");
    return 0;
}

// ---------------- 诊断探针（fcctl/diag 与测试共用） ----------------
// --probe fg              当前识别到的前台包
// --probe screen          当前亮灭屏（on/off）
// --probe family <pkg>    给定包名的场景判定结果
int cmd_probe(const std::string& what, const std::string& arg) {
    if (what == "fg") {
        auto fg = xdd::detect_foreground();
        std::printf("{\"ok\":true,\"data\":{\"pkg\":\"%s\",\"oom_adj\":%d,\"from_dumpsys\":%s}}\n",
                    fg.pkg.c_str(), fg.oom_adj, fg.from_dumpsys ? "true" : "false");
        return 0;
    }
    if (what == "screen") {
        std::printf("{\"ok\":true,\"data\":\"%s\"}\n", xdd::screen_on() ? "on" : "off");
        return 0;
    }
    if (what == "family") {
        if (arg.empty()) {
            std::fprintf(stderr, "--probe family 需要包名\n");
            return 2;
        }
        bool scr = xdd::screen_on();
        std::string fam = xdd::resolve_family(arg, scr, "");
        std::printf("{\"ok\":true,\"data\":{\"pkg\":\"%s\",\"screen\":\"%s\",\"family\":\"%s\"}}\n",
                    arg.c_str(), scr ? "on" : "off", fam.c_str());
        return 0;
    }
    if (what == "fps") {
        // 稳帧阶梯诊断（v1.0.16 A2）：模拟 daemon 切换路径——
        // settings + game 档 + perapp 的 fps.* 键叠加后输出最终阶梯。
        // arg = 包名（可选；有 perapp 配置时叠加其 fps.* 覆盖）
        auto pol = xdd::detect_policies();
        xdd::Profile base;
        xdd::load_profile("game", base);
        xdd::Conf over;
        for (const auto& kv : base.kv) over.push_back(kv);
        xdd::Profile pa;
        if (!arg.empty() && xdd::load_perapp(arg, pa)) {
            for (const auto& kv : pa.kv) over.push_back(kv);
        }
        xdd::FpsConfig f2;
        xdd::load_fps_config(f2, pol, &over);
        std::printf("{\"ok\":true,\"data\":{\"enable\":%s,\"steps\":\"%s\",\"pkg\":\"%s\"}}\n",
                    f2.enable ? "true" : "false", xdd::fps_steps_summary().c_str(),
                    arg.c_str());
        return 0;
    }
    if (what == "fas") {
        // fas-rs 托管状态诊断（v1.0.17）：运行状态/模式/管理应用数/路径
        xdd::FasStatus st = xdd::fas_status();
        std::printf("{\"ok\":true,\"data\":{\"running\":%s,\"mode\":\"%s\",\"managed\":%d,"
                    "\"toml\":\"%s\",\"binary_exists\":%s}}\n",
                    st.running ? "true" : "false", st.mode.c_str(), st.managed,
                    xdd::fas_toml_path().c_str(),
                    xdd::fas_binary_exists() ? "true" : "false");
        return 0;
    }
    std::fprintf(stderr, "未知探针: %s（支持 fg / screen / family / fps / fas）\n", what.c_str());
    return 2;
}

// ---------------- 状态（单行 JSON，统一信封：字段一律在 data 内） ----------------
int cmd_status() {
    xdd::CurrentState cur;
    bool has_cur = xdd::read_current(cur);

    std::string caps;
    for (const char* name : {"cpu", "cpuset", "threads", "gpu", "io", "mem", "thermal", "fps"}) {
        std::string v = xdd::get_capability(name);
        if (v.empty()) continue;
        if (!caps.empty()) caps += ",";
        caps += std::string("\"") + name + "\":\"" + v + "\"";
    }

    std::printf("{\"ok\":true,\"data\":{\"version\":\"%s\",\"mode\":\"%s\"", kVersion,
                xdd::host_mode() ? "host" : "device");
    if (has_cur) {
        std::printf(",\"pkg\":\"%s\",\"profile\":\"%s\",\"mode_state\":\"%s\"", cur.pkg.c_str(),
                    cur.profile.c_str(), cur.mode.c_str());
    }
    if (!caps.empty()) std::printf(",\"caps\":{%s}", caps.c_str());
    std::printf("}}\n");
    return 0;
}

// ---------------- 还原 ----------------
int cmd_restore() {
    xdd::log_open(xdd::nd_path("/data/adb/WY925976742/log/daemon.log"));
    xdd::restore_all();
    xdd::log_line(xdd::LogLevel::Info, "--restore 完成");
    return 0;
}

// ---------------- 应用档位 ----------------
int cmd_apply(const std::string& profile) {
    if (profile.empty()) {
        std::fprintf(stderr, "--apply 需要档位名\n");
        return 2;
    }
    xdd::log_open(xdd::nd_path("/data/adb/WY925976742/log/daemon.log"));
    xdd::Profile p;
    if (!xdd::load_profile(profile, p)) {
        std::printf("{\"ok\":false,\"msg\":\"档位不存在: %s\"}\n", profile.c_str());
        return 1;
    }
    // 前台包名（用于绑核与 per-app 覆盖）；未识别则跳过
    std::string pkg;
    auto fg = xdd::detect_foreground();
    if (!fg.pkg.empty()) pkg = fg.pkg;

    // per-app 参数覆盖：前台应用有配置时，除 mode/family 外的键覆盖档位同名参数
    // （与守护循环一致，便于手动测试每应用调优效果）
    if (!pkg.empty()) {
        xdd::Profile perapp;
        if (xdd::load_perapp(pkg, perapp)) {
            for (const auto& kv : perapp.kv) {
                if (kv.first == "mode" || kv.first == "family") continue;
                bool found = false;
                for (auto& pkv : p.kv) {
                    if (pkv.first == kv.first) {
                        pkv.second = kv.second;
                        found = true;
                        break;
                    }
                }
                if (!found) p.kv.push_back(kv);
            }
            // v1.0.17：与 daemon 一致——fas-rs 在管该应用的频率时让位，
            // 否则手动 apply 会写下频率下限挡住 fas-rs 的降频空间
            if (xdd::fas_running() && xdd::fas_enabled_for(pkg)) {
                xdd::set_freq_defer_fas(true);
            }
        }
    }

    auto r = xdd::apply_profile(p, pkg);
    xdd::CurrentState cs;
    cs.pkg = pkg.empty() ? std::string("—") : pkg;
    cs.profile = profile;
    cs.mode = "fg";
    cs.since = (long)::time(nullptr);
    xdd::write_current(cs);
    std::printf("{\"ok\":true,\"data\":{\"profile\":\"%s\",\"applied\":%d,\"degraded\":%zu}}\n",
                profile.c_str(), r.applied, r.degraded.size());
    return 0;
}

// ---------------- 防冻结 / 单实例 / 内部看门狗 ----------------
// 真机教训（v1.0.3 四测）：C++ daemon 从未设 oom_score_adj（旧 sh 版有），
// ColorOS 的后台 cgroup freezer 把 daemon 冻在 nanosleep——心跳停更、
// 日志停更、进程看着还活着，监督者也未兜住，最终双 daemon + pid 跳变。
// 对策：启动即设 -1000 防冻结/防杀；flock 单实例；看门狗线程兜底。
// g_stop_atomic 在文件头部声明（信号处理器要用）。
namespace {

std::atomic<long long> g_last_tick{0};
std::atomic<bool> g_watchdog_on{false};

// ---- fas-rs 托管状态（v1.0.17，仅 daemon 主循环用）----
static bool g_fas_managed_prev = false;   // 上一轮 fas 是否在管频率（变化时强制重落地）
static int g_fas_perf_hits = 0;           // 连续卡顿采样计数（performance 滞回）
static std::string g_fas_mode;            // 当前仲裁出的 fas 模式（空=未设置）

// 本编译单元内的小工具：去首尾空白
std::string trim_copy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 落 lock_yield 标记：互斥退让（另一个 daemon 持锁）时写（v1.0.20）。
// 曾经的坑：冲突退出误用 stopping（"按请求停止"语义），监督者把它当
// 请求停止——重置崩溃计数 + 1 秒重拉，形成无限循环且崩溃自禁被绕过
// （v1.0.19 真机刷屏 600+ 条/11 分钟）。现拆出独立标记，语义精确化。
void touch_lock_yield() {
    FILE* f = std::fopen(xdd::nd_path("/data/adb/WY925976742/state/lock_yield").c_str(), "w");
    if (f) std::fclose(f);
}

// 注意：/proc/self 是当前进程自身，绝不能走 nd_path 的 XD_ROOT 前缀
void set_oom_adj() {
    FILE* f = std::fopen("/proc/self/oom_score_adj", "w");
    if (!f) return;  // 失败不致命，仅失去防冻结保护
    std::fputs("-1000\n", f);
    std::fclose(f);
}

void* watchdog_thread(void*) {
    while (!g_stop_atomic.load()) {
        ::sleep(5);
        if (!g_watchdog_on.load()) continue;
        long long last = g_last_tick.load();
        if (last <= 0) continue;
        long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now - last > 60000) {
            // 主循环 60s 无 tick：大概率又被冻结或卡死。主动退出（码 42），
            // 让监督者重启；比永远躺着强。
            xdd::log_line(xdd::LogLevel::Error,
                          "看门狗：主循环 60 秒无 tick，主动退出（退出码 42，监督者将重启）");
            ::_exit(42);
        }
    }
    return nullptr;
}

}  // namespace

// ---------------- 守护模式：主循环 ----------------
// 识别 -> 场景判定 -> 去抖 -> 还原旧档 -> 落地新档 -> 稳帧 -> 温控 -> 快照推送
int cmd_daemon() {
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    std::signal(SIGHUP, on_signal);

    // 1) 防冻结第一件事（在任何可能卡住的初始化之前）
    set_oom_adj();

    xdd::log_open(xdd::nd_path("/data/adb/WY925976742/log/daemon.log"));
    xdd::log_line(xdd::LogLevel::Info,
                  std::string("守护进程启动 pid=") + std::to_string(::getpid()) +
                      " 模式=" + (xdd::host_mode() ? "host" : "device"));

    // 2) 单实例三保险（双 daemon 会互相覆盖节点并互踩 enforce，
    //    v1.0.3/v1.0.4 真机出现过三个 daemon 并存）
    // 2a. flock 独占：open 失败绝不裸奔（v1.0.4 的"跳过保护"是放水口）
    int lock_fd = ::open(xdd::nd_path("/data/adb/WY925976742/daemon.lock").c_str(),
                         O_WRONLY | O_CREAT, 0600);
    if (lock_fd < 0) {
        xdd::log_line(xdd::LogLevel::Error,
                      std::string("无法创建 daemon.lock（errno ") + std::to_string(errno) +
                          "），拒绝无保护启动");
        return 1;
    }
    if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        xdd::log_line(xdd::LogLevel::Warn, "另一个守护实例持有 daemon.lock，本次退出");
        touch_lock_yield();  // 互斥退让：监督者转入退避观察，不计崩溃
        return 0;
    }
    // fd 不关闭：持锁直到进程退出（内核自动释放）

    // 2b. daemon.pid 互斥双保险：flock 锁绑定 inode，锁文件若被删后重建，
    // 新旧实例锁的是不同 inode 会同时"持锁"——pid 文件互斥兜底。
    // 注意：/proc/<pid>/cmdline 查的是真实进程，绝不能走 nd_path 前缀
    // （host 模式下前缀会指向模拟树的假 /proc，真进程永远查不到）。
    // 关键：必须排除自身 pid——service.sh 在启动 daemon 后会预写
    // daemon.pid=本进程 pid，不排除就会「启动即被自己弹掉」，被监督者
    // 连续计崩溃直至自禁（v1.0.5 复核发现的 device-only 高危缺陷）。
    {
        std::string pid_s;
        if (xdd::nd_read("/data/adb/WY925976742/daemon.pid", pid_s)) {
            pid_s = trim_copy(pid_s);
            if (!pid_s.empty() && pid_s != std::to_string(::getpid())) {
                std::ifstream pf("/proc/" + pid_s + "/cmdline", std::ios::binary);
                if (pf.is_open()) {
                    std::string cmdline((std::istreambuf_iterator<char>(pf)),
                                        std::istreambuf_iterator<char>());
                    if (cmdline.find("xindiaodud") != std::string::npos) {
                        xdd::log_line(xdd::LogLevel::Warn,
                                      "daemon.pid 指向的进程仍在运行 (pid " + pid_s + ")，本次退出");
                        touch_lock_yield();  // 互斥退让：监督者转入退避观察，不计崩溃
                        return 0;
                    }
                }
            }
        }
    }

    // 3) 启动即写心跳（监督者判活依据）
    xdd::write_heartbeat();
    g_last_tick.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count());

    // 3b) 重置 fas-rs 失败计数（v1.0.20）：兑现日志承诺「下次重启 daemon 重置」。
    // 曾经的坑：计数器只增不减、跨重启 persisted，3 次失败后即使根因已修复
    // （如 v1.0.19 补写 USER_CONFIG）fas-rs 也永不再自动启动。
    xdd::fas_reset_retry_count();

    // ---- 启动自检：配置与节点不可用时不进入主循环，留可读原因 ----
    {
        auto policies0 = xdd::detect_policies();
        if (policies0.empty()) {
            xdd::log_line(xdd::LogLevel::Error, "自检失败：未探测到 cpufreq 策略组，退出");
            return 1;
        }
    }

    xdd::ModuleSettings s;
    xdd::load_settings(s);
    xdd::FpsConfig fcfg;
    xdd::ThermalConfig tcfg;
    auto policies = xdd::detect_policies();
    xdd::load_fps_config(fcfg, policies);
    xdd::load_thermal_config(tcfg, policies);

    // v1.0.17：fas games.toml 生成（配置桥，始终生成——无二进制时也可供检查）；
    // 二进制存在才启动 fas-rs；内核 eBPF 不支持时启动超时失败，
    // 静默回退内置稳帧阶梯。
    // v1.0.21：开机 /sdcard FUSE 可能未就绪——daemon（post-fs-data 阶段）常早于
    // 存储挂载完成，此时生成 USER_CONFIG 必然双写失败（2026-10-24 19:41:02 真机
    // 复现：2 条误导性 WARN + retry_count 误 +1）。首次生成前有界等待存储就绪
    // （上限 30s，1s 步进，可被停止信号打断；超时则照旧走，失败路径自愈）。
    if (!xdd::nd_exists("/sdcard/Android")) {
        xdd::log_line(xdd::LogLevel::Info, "等待 /sdcard 存储就绪（首次 fas 配置生成）…");
        for (int i = 0; i < 30 && !g_stop_atomic.load(); ++i) {
            if (xdd::nd_exists("/sdcard/Android")) break;
            ::sleep(1);
        }
    }
    // v1.0.21：清理上次会话的崩溃残留（stale mode 节点 + 僵尸进程）再继续，
    // 否则残留节点会让本次启动的 fas_running() 判活失真
    xdd::fas_reap_and_check();
    {
        int fasn = xdd::fas_generate_toml();
        if (fasn > 0) {
            xdd::log_line(xdd::LogLevel::Info,
                          "fas games.toml 已生成（" + std::to_string(fasn) + " 个应用）");
        }
        if (xdd::fas_binary_exists()) {
            // toml 有变且 fas-rs 已在跑（daemon 重启场景）：先停再起，
            // 让它加载新配置（顺带把频率现场还原后再接管）
            if (fasn >= 0 && xdd::fas_running()) xdd::fas_stop();
            xdd::fas_start();
        } else {
            xdd::log_line(xdd::LogLevel::Info,
                          "fas-rs 二进制不存在，帧感知调度未启用（用内置稳帧阶梯）");
        }
    }

    // 4) 看门狗线程
    {
        pthread_t tid;
        if (::pthread_create(&tid, nullptr, watchdog_thread, nullptr) == 0) {
            ::pthread_detach(tid);
            g_watchdog_on.store(true);
        }
    }

    std::string cur_key;    // 当前生效目标（pkg|family）
    std::string cand_key;   // 候选目标
    int hit = 0;            // 候选连续命中次数
    int cooldown_left = 0;  // 切换冷却剩余轮数
    int idle_rounds = 0;    // 连续无切换轮数（空闲退避）
    long long last_switch_ms = 0;
    long long next_live = 0;

    const std::string reload_flag = xdd::nd_path("/data/adb/WY925976742/state/reload.flag");
    const std::string pause_flag = xdd::nd_path("/data/adb/WY925976742/state/pause");

    auto now_ms = []() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    while (!g_stop) {
        xdd::write_heartbeat();
        g_last_tick.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
        long long now = now_ms();

        // ---- v1.0.18 fas-poll：检查 pending 启动是否就绪/超时 ----
        xdd::fas_poll();

        // ---- 暂停 / 全局关闭：还原后待命 ----
        bool paused = (::access(pause_flag.c_str(), F_OK) == 0);
        if (paused || !s.enabled) {
            if (xdd::has_intervention()) {
                xdd::restore_all();
                xdd::log_line(xdd::LogLevel::Info, paused ? "已暂停，节点已还原" : "全局开关关闭，节点已还原");
            }
            // v1.0.17：暂停/关闭时停掉 fas-rs（用户要求不干预时频率也归位）
            if (xdd::fas_running()) xdd::fas_stop();
            g_fas_managed_prev = false;
            cur_key.clear();
            ::sleep(1);
            continue;
        }

        // ---- 热重载（WebUI/fcctl 落 reload 标记） ----
        if (::access(reload_flag.c_str(), F_OK) == 0) {
            std::remove(reload_flag.c_str());
            xdd::load_settings(s);
            policies = xdd::detect_policies();
            xdd::load_fps_config(fcfg, policies);
            xdd::load_thermal_config(tcfg, policies);
            // v1.0.17：per-app fas 配置可能变了——重新生成 games.toml，
            // 内容有变且 fas-rs 在跑则重启加载新配置
            {
                int n = xdd::fas_generate_toml();
                if (n >= 0 && xdd::fas_running()) {
                    xdd::fas_stop();
                    xdd::fas_start();
                    xdd::log_line(xdd::LogLevel::Info, "fas games.toml 已更新，fas-rs 已重启");
                }
            }
            xdd::log_line(xdd::LogLevel::Info, "配置热重载完成");
            // 配置变了：强制重新落地当前目标
            cur_key.clear();
            hit = 0;
        }

        // ---- 前台识别与场景判定 ----
        auto fg = xdd::detect_foreground();
        bool scr = xdd::screen_on();

        // 加载 per-app 配置：mode 决定档位族，其余键覆盖档位同名参数
        // （如 cpu.min_freq.p0、thread.bind.* 等，实现每应用独立调优）
        xdd::Profile perapp;
        bool has_perapp = false;
        std::string perapp_mode;
        if (!fg.pkg.empty()) {
            has_perapp = xdd::load_perapp(fg.pkg, perapp);
            if (has_perapp) {
                std::string m;
                if (xdd::conf_get(perapp.kv, "mode", m)) perapp_mode = m;
            }
        }

        // ---- v1.0.21：僵尸回收 + fas-rs stale 崩溃恢复（假接管根治）----
        // 必须在托管判定之前：崩溃残留被清掉后，下面的 fas_running() 才会
        // 诚实返回 false，频率维度及时交还内置阶梯
        if (xdd::fas_reap_and_check()) {
            cur_key.clear();  // 频率现场已被还原，强制重新落地
        }

        // ---- v1.0.18 fas-rs 托管：当前应用是否把频率维度交给帧感知调度 ----
        // 条件三合一：perapp fas.enable=1 + 二进制在 + fas-rs 在跑。
        // 配置启用但 fas-rs 没跑（崩溃/启动失败）：尝试拉起（异步非阻塞）；
        // 拉起失败自动回落我们的阶梯与频率下限。节流与超时由 fas_start/fas_poll 内部处理。
        bool fas_managed = false;
        if (has_perapp && xdd::fas_enabled_for(fg.pkg)) {
            if (xdd::fas_running()) {
                fas_managed = true;
            } else if (xdd::fas_binary_exists()) {
                xdd::fas_start();  // 异步：fork 后立即返回，节点就绪由 fas_poll 确认
                fas_managed = xdd::fas_running();
            }
        }
        if (fas_managed != g_fas_managed_prev) {
            // 接管状态变化：强制重新落地（让位/恢复频率下限）
            cur_key.clear();
            g_fas_managed_prev = fas_managed;
            xdd::log_line(xdd::LogLevel::Info,
                          fas_managed ? "频率维度移交 fas-rs（帧感知接管）"
                                      : "fas-rs 退出接管，恢复内置频率干预");
        }
        xdd::set_freq_defer_fas(fas_managed);

        std::string family = xdd::resolve_family(fg.pkg, scr, perapp_mode);
        std::string key = fg.pkg + "|" + family;

        // ---- 去抖 + 冷却 ----
        if (key == cand_key) {
            ++hit;
        } else {
            cand_key = key;
            hit = 1;
        }
        if (cooldown_left > 0) --cooldown_left;

        bool cooldown_ok = (now - last_switch_ms >= (long long)s.cooldown_ms);
        if (key != cur_key && hit >= s.debounce && cooldown_ok) {
            // ---- 执行切换：先还原旧档，再落地新档 ----
            xdd::restore_all();

            if (family != "off") {
                xdd::Profile p;
                if (xdd::load_profile(family, p)) {
                    // per-app 参数覆盖：除 mode/family 外的键覆盖档位同名参数
                    if (has_perapp) {
                        for (const auto& kv : perapp.kv) {
                            if (kv.first == "mode" || kv.first == "family") continue;
                            bool found = false;
                            for (auto& pkv : p.kv) {
                                if (pkv.first == kv.first) {
                                    pkv.second = kv.second;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) p.kv.push_back(kv);
                        }
                    }
                    // v1.0.16 A2：稳帧阶梯按 档位→perapp 顺序叠加重建
                    // （每游戏独立阶梯：fps.* 键只被 load_fps_config 消费，
                    //   apply_profile 会忽略；顺序=档位在前 perapp 在后，后者优先）
                    {
                        xdd::Conf fpsOver;
                        for (const auto& kv : p.kv) fpsOver.push_back(kv);
                        if (has_perapp) {
                            for (const auto& kv : perapp.kv) fpsOver.push_back(kv);
                        }
                        xdd::load_fps_config(fcfg, policies, &fpsOver);
                    }
                    xdd::apply_profile(p, fg.pkg);
                    xdd::CurrentState cs;
                    cs.pkg = fg.pkg.empty() ? std::string("—") : fg.pkg;
                    cs.profile = family;
                    cs.mode = scr ? "fg" : "bg";
                    cs.since = (long)::time(nullptr);
                    xdd::write_current(cs);
                } else {
                    xdd::log_line(xdd::LogLevel::Warn, "档位配置缺失: " + family);
                }
            } else {
                xdd::clear_current();
            }

            cur_key = key;
            last_switch_ms = now;
            idle_rounds = 0;
            cooldown_left = s.cooldown_ms / (s.poll_ms > 0 ? s.poll_ms : 1);
            if (cooldown_left < 1) cooldown_left = 1;
            xdd::log_line(xdd::LogLevel::Info, "切换 -> 应用=" + (fg.pkg.empty() ? "未识别" : fg.pkg) +
                                                   " 档位=" + family);
        }

        // ---- 稳帧与温控（仅游戏族；单一温度源） ----
        if (cur_key.find("|game") != std::string::npos && fcfg.enable) {
            xdd::TempInfo ti;
            int temp = xdd::read_max_temp(ti) ? ti.max_milli : 0;
            xdd::fps_tick(fg.pkg, fcfg, policies, temp);
            if (s.f_thermal) {
                xdd::thermal_tick(tcfg, policies, temp);
            }
        }

        // ---- v1.0.17 fas 模式仲裁 ----
        // 温控触发 → powersave（让帧感知调度收敛性能）；
        // 连续 3 次采样卡顿超标 → performance；否则 balance。
        // 滞回计数防模式抖动；写节点失败静默（不影响其他干预）。
        if (fas_managed) {
            std::string want = "balance";
            if (xdd::thermal_capped()) {
                want = "powersave";
                g_fas_perf_hits = 0;
            } else {
                double jank = xdd::fps_last_jank();
                if (jank >= 0 && jank >= (double)fcfg.jank_threshold) {
                    if (++g_fas_perf_hits >= 3) want = "performance";
                } else {
                    g_fas_perf_hits = 0;
                }
            }
            if (want != g_fas_mode) {
                xdd::fas_set_mode(want);
                g_fas_mode = want;
                xdd::log_line(xdd::LogLevel::Info, "fas 模式 -> " + want);
            }
        } else if (!g_fas_mode.empty()) {
            g_fas_mode.clear();
        }

        // ---- live 快照（按时间窗） ----
        if (now >= next_live) {
            xdd::TempInfo ti;
            int temp = xdd::read_max_temp(ti) ? ti.max_milli : 0;
            std::string freqs;
            for (size_t i = 0; i < policies.size(); ++i) {
                std::string cur;
                xdd::nd_read(policies[i].dir + "/scaling_cur_freq", cur);
                if (cur.empty()) xdd::nd_read(policies[i].dir + "/scaling_min_freq", cur);
                if (!freqs.empty()) freqs += ",";
                freqs += cur.empty() ? "0" : cur;
            }
            xdd::write_live({
                {"PKG", fg.pkg.empty() ? "—" : fg.pkg},
                {"FAMILY", family},
                {"TEMP", std::to_string(temp / 1000)},
                {"FREQ", freqs},
                {"FPS_LEVEL", std::to_string(xdd::fps_current_level())},
                {"THERM", xdd::thermal_capped() ? "1" : "0"},
                {"FAS", fas_managed ? "1" : "0"},
                {"FASMODE", g_fas_mode.empty() ? "-" : g_fas_mode},
            });
            next_live = now + s.live_push_ms;
        }

        // ---- 休眠：常规周期，连续无切换退避到 idle ----
        if (key == cur_key) {
            ++idle_rounds;
        } else {
            idle_rounds = 0;
        }
        int sleep_ms = (idle_rounds >= 5 && s.idle_ms > s.poll_ms) ? s.idle_ms : s.poll_ms;
        // 分段睡：每段 ≤1s 并刷新心跳。
        // 监督者按心跳真实年龄判活（阈值 12s），守护循环含 dumpsys（最长 3s）
        // 与空闲退避（最长 5s），不分段最坏间隔约 8s——分段把它压到 1s 级，
        // 误杀风险归零；同时 SIGTERM 响应也更快。
        int slept = 0;
        while (slept < sleep_ms && !g_stop) {
            int seg = sleep_ms - slept;
            if (seg > 1000) seg = 1000;
            if (seg >= 1000) {
                ::sleep(seg / 1000);
            } else {
                ::usleep((useconds_t)(seg * 1000));
            }
            slept += seg;
            xdd::write_heartbeat();
            g_last_tick.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
        }
    }

    // ---- 退出：停看门狗、还原节点 ----
    g_watchdog_on.store(false);
    g_stop_atomic.store(true);
    if (xdd::has_intervention()) {
        xdd::restore_all();
    }
    // v1.0.17：退出时停 fas-rs（防模块停用后它还在调频率）
    if (xdd::fas_running()) xdd::fas_stop();
    xdd::log_line(xdd::LogLevel::Info, "守护进程退出");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "--version") return cmd_version();
    if (cmd == "--selfcheck") return cmd_selfcheck();
    if (cmd == "--status") return cmd_status();
    if (cmd == "--restore") return cmd_restore();
    if (cmd == "--apply") return cmd_apply(argc > 2 ? argv[2] : "");
    if (cmd == "--daemon") return cmd_daemon();
    if (cmd == "--probe") return cmd_probe(argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "");

    std::fprintf(stderr, "未知命令: %s\n", cmd.c_str());
    usage();
    return 2;
}
