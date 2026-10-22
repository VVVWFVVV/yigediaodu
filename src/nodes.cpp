#include "nodes.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <unistd.h>

#include "log.hpp"
#include "paths.hpp"

namespace xdd {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool read_file_raw(const std::string& real_path, std::string& out) {
    std::ifstream f(real_path);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 单次写入尝试的三态结果
enum class WriteErr { None, Open, Write, Mismatch };

const char* err_name(WriteErr e) {
    switch (e) {
        case WriteErr::None: return "ok";
        case WriteErr::Open: return "open";
        case WriteErr::Write: return "write";
        case WriteErr::Mismatch: return "mismatch";
    }
    return "?";
}

// 测试接缝：XDD_TEST_PERM_DENY 非空时，对路径含该子串的写入模拟 EACCES
// （表现为打不开）。仅 host 测试设置；设备端不设置，生产行为零影响。
// 特例：enforce 读到 "0" 时放行——用于验证 permissive 重试链路。
bool sim_deny_active(const std::string& real) {
    const char* sim = ::getenv("XDD_TEST_PERM_DENY");
    if (!sim || !*sim) return false;
    if (real.find(sim) == std::string::npos) return false;
    std::string cur;
    if (read_file_raw(nd_path("/sys/fs/selinux/enforce"), cur) && trim(cur) == "0") {
        return false;  // 已处于 permissive，不再模拟
    }
    return true;
}

// 测试接缝：XDD_TEST_READ_DENY 非空时，对路径含该子串的读取模拟 EACCES。
// 用途：复现真机场景——SELinux 拦截 ksu 域对 cgroup 的读取，
// 导致存在性探测（nd_exists/nd_read）说谎返回 false（v1.0.1 踩坑）。
bool sim_read_deny(const std::string& real) {
    const char* sim = ::getenv("XDD_TEST_READ_DENY");
    return sim && *sim && real.find(sim) != std::string::npos;
}

// open 失败时的 errno：在 try_write_node 打开失败的当场保存，避免后续
// 文件操作覆盖 errno，也避免 Mismatch 路径打出误导性的 "0 (Success)"
int g_last_open_errno = 0;

WriteErr try_write_node(const std::string& real, const std::string& value) {
    if (sim_deny_active(real)) {
        g_last_open_errno = EACCES;
        return WriteErr::Open;
    }
    // 关键：O_WRONLY 不带 O_CREAT——内核节点不存在时必须失败（ENOENT），
    // 绝不允许 ofstream 那样把文件创建出来（会掩盖路径错误，
    // 让候选链停在错的路经——v1.0.4 mock 实测踩坑）
    int fd = ::open(real.c_str(), O_WRONLY | O_TRUNC);
    if (fd < 0) {
        g_last_open_errno = errno;
        return WriteErr::Open;
    }
    const std::string data = value + "\n";
    const ssize_t w = ::write(fd, data.data(), data.size());
    ::close(fd);
    if (w != (ssize_t)data.size()) return WriteErr::Write;
    // 写后回读比对（自证生效）
    std::string back;
    if (read_file_raw(real, back) && trim(back) == trim(value)) return WriteErr::None;
    return WriteErr::Mismatch;
}

std::string last_open_errno_text() {
    return std::to_string(g_last_open_errno) + " (" + std::strerror(g_last_open_errno) + ")";
}

// enforce 节点直写（绝不经 nd_write，避免与 permissive 逻辑递归）
// 不用 O_TRUNC：sysfs 节点对 O_TRUNC 的兼容性未验证（v1.0.5 加固）
bool set_enforce_raw(const std::string& v) {
    const std::string real = nd_path("/sys/fs/selinux/enforce");
    int fd = ::open(real.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const std::string data = v + "\n";
    const ssize_t w = ::write(fd, data.data(), data.size());
    ::close(fd);
    return w == (ssize_t)data.size();
}

// 读回 enforce 当前值（trim 后），失败返回 false
bool read_enforce(std::string& out) {
    return nd_read("/sys/fs/selinux/enforce", out) && !out.empty();
}

}  // namespace

// 裸写（无回读/基线/重试）：一次性动作型节点专用。
// 用途：/proc/sys/vm/drop_caches（读回恒 0）这类「写即触发」节点——
// 走 nd_write 会因回读不一致而误判失败。同 set_enforce_raw：
// open 无 O_CREAT（节点不存在必须失败，绝不创建文件）。
bool raw_write_node(const std::string& path, const std::string& value) {
    const std::string real = nd_path(path);
    int fd = ::open(real.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const std::string data = value + "\n";
    const ssize_t w = ::write(fd, data.data(), data.size());
    ::close(fd);
    return w == (ssize_t)data.size();
}

bool nd_read(const std::string& path, std::string& out) {
    if (sim_read_deny(nd_path(path))) return false;  // 测试接缝：模拟 SELinux 拦读
    return read_file_raw(nd_path(path), out) ? (out = trim(out), true) : false;
}

bool nd_read_int(const std::string& path, long long& out) {
    std::string s;
    if (!nd_read(path, s)) return false;
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtoll(s.c_str(), &end, 10);
    return end && *end == '\0';
}

bool nd_exists(const std::string& path) {
    if (sim_read_deny(nd_path(path))) return false;  // 测试接缝：模拟 SELinux 拦读
    std::ifstream f(nd_path(path));
    return f.good();
}

bool nd_write(const std::string& path, const std::string& value) {
    const std::string real = nd_path(path);

    // Mismatch：重试 3 次（v1.0.12 真机：还原 policy6 频率时系统短暂锁住
    // 节点，单次重试不足以覆盖窗口；每次间隔 10ms 让系统喘口气）
    for (int attempt = 1; attempt <= 4; ++attempt) {
        WriteErr err = try_write_node(real, value);
        if (err == WriteErr::None) return true;

        if (err == WriteErr::Open) {
            // ENOENT：节点不存在——cpuset 候选链的正常探测行为
            // （v1 名 cpuset.cpus 在 v2 命名设备上不存在），静默返回 false
            // 让调用方尝试下一候选；不刷 WARN 日志，也不白翻 permissive
            // （文件不存在，翻 SELinux 模式也无济于事）
            if (g_last_open_errno == ENOENT) {
                return false;
            }
            // 打不开：SELinux enforcing 时走一次瞬时 permissive 重试。
            // 关键：不能用 nd_exists(path) 做前置判断——SELinux 拦读时
            // nd_exists 本身不可信（v1.0.1 真机：ksu 域读 cgroup 被拦，
            // 存在性探测误判 false，permissive 从未触发，cpuset 直接降级）。
            // 只要 enforce 可读且原值非 0，就试一次；代价仅是节点真的
            // 不存在时多一次毫秒级 permissive 窗口，无副作用。
            std::string orig;
            if (read_enforce(orig) && orig != "0") {
                // 翻 0 后必须读回确认真的进了 permissive（v1.0.4 真机：
                // 多 daemon 互踩/写未生效时重试照样 EACCES，日志看不出原因）
                std::string chk;
                if (set_enforce_raw("0") && read_enforce(chk) && chk == "0") {
                    WriteErr e2 = try_write_node(real, value);
                    // 恢复原值，并读回确认恢复成功
                    std::string back;
                    bool restored = set_enforce_raw(orig) &&
                                    read_enforce(back) && back == orig;
                    if (e2 == WriteErr::None) {
                        log_line(LogLevel::Info,
                                 "nd_write 经瞬时 permissive 成功: " + path +
                                     "（enforce " + orig + "→0→" + (restored ? orig : "?") + "）");
                        return true;
                    }
                    log_line(LogLevel::Warn,
                             "nd_write 瞬时 permissive 仍失败: " + path +
                                 "（原因 " + err_name(e2) + " " + last_open_errno_text() +
                                 (restored ? "" : "，enforce 恢复后读回不符") + "）");
                } else {
                    log_line(LogLevel::Warn,
                             std::string("nd_write enforce 翻 0 未生效（读回=") +
                                 (chk.empty() ? "读不到" : chk) + "），放弃 permissive 重试: " + path);
                }
            }
            log_line(LogLevel::Warn, "nd_write 打不开节点: " + path + "（errno " + last_open_errno_text() + "）");
            return false;
        }
        if (err == WriteErr::Write) {
            log_line(LogLevel::Warn, "nd_write 写入失败: " + path);
            return false;
        }
        // Mismatch：重试（间隔 10ms，让热控/调度器写完它自己的值）
        if (attempt < 4) {
            log_line(LogLevel::Warn, "nd_write 回读不一致，重试 " + std::to_string(attempt) +
                                         "/3: " + path + " 期望=" + value);
            ::usleep(10 * 1000);
        } else {
            log_line(LogLevel::Warn, "nd_write 重试 3 次后仍不一致: " + path + " 期望=" + value);
        }
    }
    return false;
}

std::vector<PolicyInfo> detect_policies() {
    std::vector<PolicyInfo> out;
    const std::string base = "/sys/devices/system/cpu/cpufreq";

    // 1) policy* 目录
    DIR* d = ::opendir(nd_path(base).c_str());
    if (d) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.rfind("policy", 0) != 0) continue;
            std::string dir = base + "/" + name;
            PolicyInfo p;
            p.dir = dir;
            nd_read(dir + "/related_cpus", p.cpus);
            nd_read_int(dir + "/cpuinfo_min_freq", p.min_freq);
            nd_read_int(dir + "/cpuinfo_max_freq", p.max_freq);
            if (!p.cpus.empty()) out.push_back(p);
        }
        ::closedir(d);
    }

    // 2) 回退：cpu*/cpufreq 按 related_cpus 归并
    if (out.empty()) {
        const std::string cbase = "/sys/devices/system/cpu";
        d = ::opendir(nd_path(cbase).c_str());
        if (d) {
            struct dirent* e;
            while ((e = ::readdir(d)) != nullptr) {
                std::string name = e->d_name;
                if (name.rfind("cpu", 0) != 0) continue;
                std::string dir = cbase + "/" + name + "/cpufreq";
                std::string cpus;
                if (!nd_read(dir + "/related_cpus", cpus) || cpus.empty()) continue;
                bool found = false;
                for (auto& p : out) {
                    if (p.cpus == cpus) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    PolicyInfo p;
                    p.dir = dir;
                    p.cpus = cpus;
                    nd_read_int(dir + "/cpuinfo_min_freq", p.min_freq);
                    nd_read_int(dir + "/cpuinfo_max_freq", p.max_freq);
                    out.push_back(p);
                }
            }
            ::closedir(d);
        }
    }

    // 按首 CPU 编号排序，保证输出稳定
    std::sort(out.begin(), out.end(), [](const PolicyInfo& a, const PolicyInfo& b) {
        auto first = [](const std::string& s) {
            size_t i = 0;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
            return std::atoi(s.substr(0, i).c_str());
        };
        return first(a.cpus) < first(b.cpus);
    });
    return out;
}

bool read_max_temp(TempInfo& out) {
    const std::string base = "/sys/class/thermal";
    DIR* d = ::opendir(nd_path(base).c_str());
    if (!d) return false;

    bool any = false;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string dir = base + "/" + name;
        long long t = 0;
        if (!nd_read_int(dir + "/temp", t)) continue;
        int milli = static_cast<int>(t);
        if (!any || milli > out.max_milli) {
            out.max_milli = milli;
            nd_read(dir + "/type", out.zone);
            any = true;
        }
    }
    ::closedir(d);
    return any;
}

}  // namespace xdd
