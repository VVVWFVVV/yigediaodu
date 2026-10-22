#include "engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>

#include "log.hpp"
#include "monitor.hpp"
#include "paths.hpp"
#include "state.hpp"

namespace xdd {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool truthy(const std::string& v) {
    return v == "1" || v == "true" || v == "True" || v == "TRUE" || v == "yes" || v == "on";
}

std::string baseline_file() {
    return nd_path("/data/adb/WY925976742/baseline/nodes.conf");
}

// cpuset 候选路径（有序）：真机实测（一加平板 sm8750）该内核 cpuset 挂载带
// cpuset_v2_mode，文件名是 v2 命名（cpus/mems）而非 v1 的 cpuset.cpus；
// 且 Android 实际挂载点在 /dev/cpuset，老内核/其他 ROM 在 /sys。
// 四候选按序尝试，首个写入成功即用；基线按实际成功路径记录，还原一致。
// 不做存在性读探测——SELinux 可能拦读导致误判（v1.0.1 踩坑）。
std::vector<std::string> cpuset_candidates(const std::string& grp) {
    std::vector<std::string> out;
    out.push_back("/dev/cpuset/" + grp + "/cpuset.cpus");                    // v1 命名（通用）
    out.push_back("/dev/cpuset/" + grp + "/cpus");                           // v2 命名（本设备）
    out.push_back("/sys/devices/system/cpu/cpuset/" + grp + "/cpuset.cpus"); // 老内核兜底 v1
    out.push_back("/sys/devices/system/cpu/cpuset/" + grp + "/cpus");        // 老内核兜底 v2
    return out;
}

// 基线快照：首次写某节点前记录当前值（同一节点只记一次）
void baseline_snapshot(const std::string& path) {
    std::ifstream in(baseline_file());
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(path + "=", 0) == 0) return;  // 已记录
    }
    in.close();

    std::string cur;
    if (!nd_read(path, cur)) return;  // 读不到当前值不记（还原时无意义）

    FILE* f = std::fopen(baseline_file().c_str(), "a");
    if (!f) {
        log_line(LogLevel::Warn, "基线快照写入失败: " + path);
        return;
    }
    std::fprintf(f, "%s=%s\n", path.c_str(), cur.c_str());
    std::fclose(f);
}

// 带基线快照的写入：成功返回 true
bool write_with_baseline(const std::string& path, const std::string& value) {
    baseline_snapshot(path);
    return nd_write(path, value);
}

// 解析 cpu_set 掩码字符串（"6-7" / "0,2" / "0-3,6-7"）
cpu_set_t parse_cpu_list(const std::string& s) {
    cpu_set_t set;
    CPU_ZERO(&set);
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim(part);
        size_t dash = part.find('-');
        try {
            if (dash == std::string::npos) {
                int c = std::stoi(part);
                if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
            } else {
                int a = std::stoi(part.substr(0, dash));
                int b = std::stoi(part.substr(dash + 1));
                for (int c = a; c <= b && c < CPU_SETSIZE; ++c) CPU_SET(c, &set);
            }
        } catch (...) {
            // 忽略无法解析的段
        }
    }
    return set;
}

// 找包名对应的 pid 列表（/proc 扫描）
std::vector<int> find_pkg_pids(const std::string& pkg) {
    std::vector<int> out;
    const std::string proc = nd_path("/proc");
    DIR* d = ::opendir(proc.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (*n == '\0' || !std::isdigit((unsigned char)*n)) continue;
        // nd_read 内部会拼 XD_ROOT 前缀，这里传逻辑路径
        std::string raw;
        if (!nd_read("/proc/" + std::string(n) + "/cmdline", raw)) continue;
        size_t nul = raw.find('\0');
        std::string first = (nul == std::string::npos) ? raw : raw.substr(0, nul);
        if (trim(first) == pkg) out.push_back(std::atoi(n));
    }
    ::closedir(d);
    return out;
}

}  // namespace

// 频率吸附：将目标频率取整到 <dir>/<avail_node> 中最近的可用值。
// 真机教训（v1.0.12 实测）：配置值 1708800 不在真机频率表中（policy6 合法
// 相邻值 1689→1958 MHz），内核把非法值钳位/拒绝，写后回读不一致触发
// 「能力降级: cpu」——稳帧阶梯与频率下限全部失效。吸附后即使不同设备频率表
// 不同也能自动适配；读不到可用列表时返回原值（不改变行为）。
// round_up=true 向上取整（min_freq 下限，不低于目标）；
// round_up=false 向下取整（max_freq 封顶，绝不超过目标）。
long long snap_to_available_freq(const std::string& dir, long long target,
                                 bool round_up, const std::string& avail_node) {
    std::string avail;
    if (!nd_read(dir + "/" + avail_node, avail) || avail.empty()) {
        return target;  // 读不到列表：保持原值
    }
    std::vector<long long> freqs;
    std::istringstream iss(avail);
    std::string tok;
    while (iss >> tok) {
        char* end = nullptr;
        long long v = std::strtoll(tok.c_str(), &end, 10);
        if (end && *end == '\0' && v > 0) freqs.push_back(v);
    }
    if (freqs.empty()) return target;

    if (round_up) {
        // 向上取整：找 >= target 的最小可用值（不低于目标，避免性能损失）
        long long best = 0;
        for (long long f : freqs) {
            if (f >= target && (best == 0 || f < best)) best = f;
        }
        if (best == 0) {
            // 目标高于所有可用值：取最大可用值（上限保护）
            best = *std::max_element(freqs.begin(), freqs.end());
        }
        return best;
    }
    // 向下取整：找 <= target 的最大可用值（封顶绝不超过目标）
    long long best = 0;
    for (long long f : freqs) {
        if (f <= target && f > best) best = f;
    }
    if (best == 0) {
        // 目标低于所有可用值：取最小可用值（已是最低，封顶无意义但不报错）
        best = *std::min_element(freqs.begin(), freqs.end());
    }
    return best;
}

// GPU devfreq 目录探测（静态只探测一次）：sm8750 的 Adreno GPU 在
// /sys/class/kgsl/kgsl-3d0/devfreq（节点名无 scaling_ 前缀：min_freq/
// max_freq/available_frequencies/governor）。个别设备路径不同，
// 退而扫描 /sys/class/kgsl/*/devfreq。都没有返回空串（调用方降级处理）。
const std::string& gpu_devfreq_dir() {
    static const std::string dir = [] {
        const char* fixed = "/sys/class/kgsl/kgsl-3d0/devfreq";
        if (nd_exists(fixed + std::string("/available_frequencies"))) {
            return std::string(fixed);
        }
        // 扫描兜底
        const std::string base = "/sys/class/kgsl";
        DIR* d = ::opendir(nd_path(base).c_str());
        if (!d) return std::string();
        struct dirent* e;
        std::string found;
        while ((e = ::readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.rfind("kgsl-", 0) != 0) continue;
            std::string cand = base + "/" + name + "/devfreq";
            if (nd_exists(cand + "/available_frequencies")) {
                found = cand;
                break;
            }
        }
        ::closedir(d);
        return found;
    }();
    return dir;
}

bool load_settings(ModuleSettings& out) {
    auto pr = conf_parse_file(settings_path());
    if (!pr.bad_lines.empty()) {
        log_line(LogLevel::Warn, "settings.conf 有 " + std::to_string(pr.bad_lines.size()) +
                                     " 行无法解析，已跳过这些行");
    }
    const Conf& kv = pr.kv;

    std::string v;
    if (conf_get(kv, "enabled", v)) out.enabled = truthy(v);
    if (conf_get(kv, "poll_interval_ms", v)) out.poll_ms = std::atoi(v.c_str());
    if (conf_get(kv, "poll_idle_ms", v)) out.idle_ms = std::atoi(v.c_str());
    if (conf_get(kv, "switch_debounce", v)) out.debounce = std::atoi(v.c_str());
    if (conf_get(kv, "switch_cooldown_ms", v)) out.cooldown_ms = std::atoi(v.c_str());
    if (conf_get(kv, "default_family", v) && !v.empty()) out.default_family = v;
    if (conf_get(kv, "live_push_ms", v)) out.live_push_ms = std::atoi(v.c_str());

    if (conf_get(kv, "feature.cpu", v)) out.f_cpu = truthy(v);
    if (conf_get(kv, "feature.cpuset", v)) out.f_cpuset = truthy(v);
    if (conf_get(kv, "feature.threads", v)) out.f_threads = truthy(v);
    if (conf_get(kv, "feature.gpu", v)) out.f_gpu = truthy(v);
    if (conf_get(kv, "feature.io", v)) out.f_io = truthy(v);
    if (conf_get(kv, "feature.mem", v)) out.f_mem = truthy(v);
    if (conf_get(kv, "feature.thermal", v)) out.f_thermal = truthy(v);

    // 边界修正
    if (out.poll_ms < 200) out.poll_ms = 200;
    if (out.poll_ms > 5000) out.poll_ms = 5000;
    if (out.idle_ms < out.poll_ms) out.idle_ms = out.poll_ms;
    if (out.idle_ms > 5000) out.idle_ms = 5000;
    if (out.debounce < 1) out.debounce = 1;
    if (out.live_push_ms < 2000) out.live_push_ms = 2000;
    return true;
}

bool load_profile(const std::string& name, Profile& out) {
    std::string path = profile_path(name);
    std::ifstream f(path);
    if (!f.is_open()) {
        log_line(LogLevel::Warn, "档位配置不存在: " + name);
        return false;
    }
    auto pr = conf_parse_file(path);
    out.name = name;
    out.family = conf_get_or(pr.kv, "family", name);
    out.kv = pr.kv;
    if (!pr.bad_lines.empty()) {
        log_line(LogLevel::Warn, "档位 " + name + " 有 " + std::to_string(pr.bad_lines.size()) +
                                     " 行无法解析");
    }
    return true;
}

bool load_perapp(const std::string& pkg, Profile& out) {
    std::string path = perapp_path(pkg);
    std::ifstream f(path);
    if (!f.is_open()) return false;
    auto pr = conf_parse_file(path);
    out.name = pkg;
    out.family = conf_get_or(pr.kv, "family", "");
    out.kv = pr.kv;
    return true;
}

ApplyResult apply_profile(const Profile& prof, const std::string& pkg) {
    ApplyResult r;
    ModuleSettings s;
    load_settings(s);

    auto degrade = [&r](const char* cap) {
        for (const auto& d : r.degraded) {
            if (d == cap) return;
        }
        r.degraded.push_back(cap);
    };

    // 策略组数量决定 p0/p1 键是否可用
    auto policies = detect_policies();
    auto policy_count = policies.size();

    for (const auto& kv : prof.kv) {
        const std::string& key = kv.first;
        const std::string& val = kv.second;

        // ---- CPU 频率 ----
        if (key.rfind("cpu.min_freq.p", 0) == 0 && s.f_cpu) {
            int idx = std::atoi(key.substr(key.size() - 1).c_str());
            if (idx < (int)policy_count) {
                std::string node = policies[idx].dir + "/scaling_min_freq";
                // 频率吸附：目标值不在可用频率表时向上取整到合法值，
                // 避免内核钳位导致写后回读不一致（v1.0.12 实测）
                char* end = nullptr;
                long long target = std::strtoll(val.c_str(), &end, 10);
                std::string effective = val;
                if (end && *end == '\0' && target > 0) {
                    long long snapped = snap_to_available_freq(policies[idx].dir, target);
                    if (snapped != target) {
                        effective = std::to_string(snapped);
                        log_line(LogLevel::Info, "频率吸附: " + node + " " + val +
                                                     " -> " + effective +
                                                     "（原值不在可用频率表）");
                    }
                }
                if (write_with_baseline(node, effective)) {
                    ++r.applied;
                    set_capability("cpu", "ok");
                } else {
                    degrade("cpu");
                }
            }
        } else if (key == "cpu.governor" && s.f_cpu) {
            bool any = false;
            for (const auto& p : policies) {
                if (write_with_baseline(p.dir + "/scaling_governor", val)) any = true;
            }
            if (any) {
                ++r.applied;
                set_capability("cpu", "ok");
            } else {
                degrade("cpu");
            }
        }

        // ---- cpuset ----
        else if (key == "cpuset.background_cpus" && s.f_cpuset) {
            bool ok = true;
            bool any = false;
            for (const char* grp : {"background", "system-background"}) {
                // 候选路径按序尝试：/dev/cpuset 优先，/sys 兜底
                for (const auto& node : cpuset_candidates(grp)) {
                    if (write_with_baseline(node, val)) {
                        any = true;
                        break;
                    }
                }
                if (!any) ok = false;
                any = false;
            }
            if (ok) {
                ++r.applied;
                set_capability("cpuset", "ok");
            } else {
                degrade("cpuset");
            }
        } else if (key == "cpuset.top_app_cpus" && s.f_cpuset) {
            bool done = false;
            for (const auto& node : cpuset_candidates("top-app")) {
                if (write_with_baseline(node, val)) {
                    done = true;
                    break;
                }
            }
            if (done) {
                ++r.applied;
                set_capability("cpuset", "ok");
            } else {
                degrade("cpuset");
            }
        }

        // ---- IO ----
        else if (key == "io.read_ahead_kb" && s.f_io) {
            bool any = false;
            const std::string blk = nd_path("/sys/block");
            DIR* d = ::opendir(blk.c_str());
            if (d) {
                struct dirent* e;
                while ((e = ::readdir(d)) != nullptr) {
                    std::string name = e->d_name;
                    if (name == "." || name == "..") continue;
                    std::string node = "/sys/block/" + name + "/queue/read_ahead_kb";
                    if (nd_exists(node) && write_with_baseline(node, val)) any = true;
                }
                ::closedir(d);
            }
            if (any) {
                ++r.applied;
                set_capability("io", "ok");
            } else {
                degrade("io");
            }
        }

        // ---- 内存 ----
        else if (key == "mem.swappiness" && s.f_mem) {
            if (write_with_baseline("/proc/sys/vm/swappiness", val)) {
                ++r.applied;
                set_capability("mem", "ok");
            } else {
                degrade("mem");
            }
        }
        // 脏页写回阈值（游戏档调大：资源解压/贴图加载的突发写入吞吐）
        else if (key == "mem.dirty_ratio" && s.f_mem) {
            if (write_with_baseline("/proc/sys/vm/dirty_ratio", val)) {
                ++r.applied;
                set_capability("mem", "ok");
            } else {
                degrade("mem");
            }
        } else if (key == "mem.dirty_background_ratio" && s.f_mem) {
            if (write_with_baseline("/proc/sys/vm/dirty_background_ratio", val)) {
                ++r.applied;
                set_capability("mem", "ok");
            } else {
                degrade("mem");
            }
        }
        // 游戏启动前释放页缓存（一次性动作，不走基线/回读：
        // /proc/sys/vm/drop_caches 读回恒 0，nd_write 的回读必失败）
        else if (key == "mem.drop_caches" && s.f_mem) {
            if (truthy(val)) {
                if (raw_write_node("/proc/sys/vm/drop_caches", "1")) {
                    ++r.applied;
                    set_capability("mem", "ok");
                    log_line(LogLevel::Info, "已释放页缓存（游戏启动准备）");
                } else {
                    degrade("mem");
                    log_line(LogLevel::Warn, "drop_caches 写入失败");
                }
            }
            // 0/false：不干预，跳过
        }

        // ---- GPU（v1.0.16：频率下限，抹掉深 dip 防帧时间尖峰） ----
        else if (key == "gpu.min_freq" && s.f_gpu) {
            const std::string& gdir = gpu_devfreq_dir();
            if (gdir.empty()) {
                degrade("gpu");
                log_line(LogLevel::Warn, "未探测到 GPU devfreq 节点，gpu.min_freq 跳过");
            } else {
                char* end = nullptr;
                long long target = std::strtoll(val.c_str(), &end, 10);
                if (target <= 0) {
                    // 0 或非法：不干预（轻量游戏关闭 GPU 下限用）
                } else {
                    // 吸附到合法 GPU 频率（KGSL 节点无 scaling_ 前缀）
                    long long snapped = snap_to_available_freq(gdir, target, true,
                                                               "available_frequencies");
                    std::string effective = std::to_string(snapped);
                    if (snapped != target) {
                        log_line(LogLevel::Info, "GPU 频率吸附: " + val + " -> " + effective);
                    }
                    // 写前检测只读：部分设备（如 OPD2413/ColorOS）的
                    // KGSL devfreq min_freq 是内核只读（0444），root 也不可写。
                    // 只读时优雅跳过（INFO），不降级——设备限制不是故障
                    std::string min_node = gdir + "/min_freq";
                    if (::access(nd_path(min_node).c_str(), W_OK) != 0) {
                        log_line(LogLevel::Info,
                                 "GPU devfreq min_freq 节点只读（设备限制），GPU 频率下限跳过");
                    } else if (write_with_baseline(min_node, effective)) {
                        ++r.applied;
                        set_capability("gpu", "ok");
                    } else {
                        degrade("gpu");
                    }
                }
            }
        }

        // ---- 绑核白名单（收集后统一执行） ----
        // thread.bind.<线程名前缀>=<核集合>
    }

    // ---- 绑核 ----
    if (s.f_threads) {
        std::vector<std::pair<std::string, std::string>> rules;
        for (const auto& kv : prof.kv) {
            if (kv.first.rfind("thread.bind.", 0) == 0) {
                rules.emplace_back(kv.first.substr(12), kv.second);
            }
        }
        if (!rules.empty() && !pkg.empty()) {
            if (bind_threads(pkg, rules)) {
                ++r.applied;
                set_capability("threads", "ok");
            } else {
                degrade("threads");
            }
        }
    }

    // 降级能力落 state
    for (const auto& cap : r.degraded) {
        set_capability(cap, "degraded");
        log_line(LogLevel::Warn, "能力降级: " + cap + "（档位 " + prof.name + "）");
    }

    log_line(LogLevel::Info, "档位落地: " + prof.name + " 应用=" + std::to_string(r.applied) +
                                 " 降级=" + std::to_string(r.degraded.size()));
    return r;
}

bool bind_threads(const std::string& pkg,
                  const std::vector<std::pair<std::string, std::string>>& rules) {
    if (rules.empty()) return false;

    auto pids = find_pkg_pids(pkg);
    if (pids.empty()) {
        log_line(LogLevel::Warn, "绑核：未找到进程 " + pkg);
        return false;
    }

    int bound = 0;
    for (int pid : pids) {
        // opendir 用已前缀化路径；nd_read/nd_write 内部会再拼前缀，必须传逻辑路径
        const std::string task_dir = nd_path("/proc/" + std::to_string(pid) + "/task");
        const std::string task_logic = "/proc/" + std::to_string(pid) + "/task";
        DIR* d = ::opendir(task_dir.c_str());
        if (!d) continue;
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            const char* n = e->d_name;
            if (*n == '\0' || !std::isdigit((unsigned char)*n)) continue;
            int tid = std::atoi(n);

            // 读线程名（统一走 nd_read）
            std::string comm_raw;
            if (!nd_read(task_logic + "/" + n + "/comm", comm_raw)) continue;
            std::string comm = trim(comm_raw);

            for (const auto& rule : rules) {
                if (comm.rfind(rule.first, 0) != 0) continue;  // 前缀匹配
                cpu_set_t want = parse_cpu_list(rule.second);

                int rc = ::sched_setaffinity(tid, sizeof(want), &want);
                if (rc == 0) {
                    // 回读自证：Cpus_allowed_list
                    std::string st;
                    bool verified = false;
                    if (nd_read(task_logic + "/" + n + "/status", st)) {
                        size_t pos = st.find("Cpus_allowed_list:");
                        if (pos != std::string::npos) {
                            size_t eol = st.find('\n', pos);
                            std::string got = trim(st.substr(
                                pos + std::strlen("Cpus_allowed_list:"),
                                eol == std::string::npos ? std::string::npos : eol - pos));
                            verified = (got == rule.second);
                        }
                    }
                    if (verified) {
                        ++bound;
                        log_line(LogLevel::Info, "绑核成功: " + comm + " -> " + rule.second);
                    } else {
                        log_line(LogLevel::Warn, "绑核回读不一致: " + comm + " 期望 " + rule.second);
                    }
                } else if (host_mode()) {
                    // host 模拟回路：模拟 tid 上 setaffinity 必然失败，
                    // 写入模拟 status 再回读，验证除系统调用外的全部逻辑
                    std::string st = task_logic + "/" + n + "/status";
                    nd_write(st, "Cpus_allowed_list:\t" + rule.second);
                    std::string back;
                    bool ok_sim = false;
                    if (nd_read(st, back)) {
                        size_t pos = back.find("Cpus_allowed_list:");
                        if (pos != std::string::npos) {
                            size_t eol = back.find('\n', pos);
                            std::string got = trim(back.substr(
                                pos + std::strlen("Cpus_allowed_list:"),
                                eol == std::string::npos ? std::string::npos : eol - pos));
                            ok_sim = (got == rule.second);
                        }
                    }
                    if (ok_sim) {
                        ++bound;
                        log_line(LogLevel::Info, "绑核（模拟）: " + comm + " -> " + rule.second);
                    }
                } else {
                    log_line(LogLevel::Warn, "绑核失败: " + comm + " (" + std::to_string(pid) + ")");
                }
                break;  // 一个线程只按第一条命中的规则绑
            }
        }
        ::closedir(d);
    }

    return bound > 0;
}

void restore_all() {
    std::ifstream in(baseline_file());
    if (!in.is_open()) {
        // 没有基线也要把状态清干净：否则 WebUI 会一直显示残留的"已生效"
        clear_current();
        for (const char* cap : {"cpu", "cpuset", "threads", "gpu", "io", "mem", "thermal", "fps"}) {
            set_capability(cap, "off");
        }
        return;
    }
    std::string line;
    int restored = 0;
    int failed = 0;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string path = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (nd_write(path, val)) {
            ++restored;
        } else {
            ++failed;
            log_line(LogLevel::Warn, "还原失败: " + path + " = " + val);
        }
    }
    in.close();

    std::remove(baseline_file().c_str());
    clear_current();
    for (const char* cap : {"cpu", "cpuset", "threads", "gpu", "io", "mem", "thermal", "fps"}) {
        set_capability(cap, "off");
    }
    log_line(LogLevel::Info, "还原完成: 成功=" + std::to_string(restored) +
                                 " 失败=" + std::to_string(failed));
}

bool has_intervention() {
    std::ifstream f(baseline_file());
    return f.good();
}

}  // namespace xdd
