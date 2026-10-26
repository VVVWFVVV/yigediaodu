#pragma once
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "nodes.hpp"

namespace xdd {

// 档位配置（config/profiles/<name>.conf）
struct Profile {
    std::string name;
    std::string family;
    Conf kv;
};

// 模块全局设置（config/settings.conf）
struct ModuleSettings {
    bool enabled = true;
    int poll_ms = 1000;
    int idle_ms = 2000;
    int debounce = 2;
    int cooldown_ms = 800;
    std::string default_family = "daily";
    int live_push_ms = 5000;

    bool f_cpu = true;
    bool f_cpuset = true;
    bool f_threads = true;
    bool f_gpu = true;
    bool f_io = true;
    bool f_mem = true;
    bool f_thermal = true;
};

bool load_settings(ModuleSettings& out);

// 载入档位（profiles/<name>.conf）
bool load_profile(const std::string& name, Profile& out);

// 载入 perapp 配置（perapp/<pkg>.conf）；不存在返回 false
bool load_perapp(const std::string& pkg, Profile& out);

struct ApplyResult {
    std::vector<std::string> degraded;  // 降级的能力名（cpu/cpuset/threads/io/mem）
    int applied = 0;                    // 成功落地的动作数
};

// 应用档位：先快照基线（每个节点首次写入前），再逐项落地
ApplyResult apply_profile(const Profile& prof, const std::string& pkg);

// 还原全部干预：按基线快照写回，清基线与 current，能力标记全部置 off
void restore_all();

// 当前是否存在干预（基线文件存在）
bool has_intervention();

// 绑核：把 pkg 的白名单线程绑到指定核集合；host 模式走模拟回路
// rules: (线程名前缀, "6-7")
bool bind_threads(const std::string& pkg,
                  const std::vector<std::pair<std::string, std::string>>& rules);

// 频率吸附：将目标频率取整到可用频率表中最近的可用值；读不到列表时返回原值。
// round_up=true（默认，用于 min_freq 下限）：向上取整，不低于目标，避免性能
// 损失；round_up=false（用于 max_freq 封顶）：向下取整，绝不超过目标。
// avail_node 可用频率表节点名：CPU 策略组用 scaling_available_frequencies
// （默认），GPU devfreq 用 available_frequencies（KGSL 无 scaling_ 前缀）。
// 用途：写频率节点前调用，避免内核钳位非法频率值导致写后回读不一致
// （v1.0.12 真机实测：1708800 不在 sm8750 频率表被钳位降级）。
long long snap_to_available_freq(const std::string& dir, long long target,
                                 bool round_up = true,
                                 const std::string& avail_node = "scaling_available_frequencies");

// GPU devfreq 目录探测（静态一次）：优先 /sys/class/kgsl/kgsl-3d0/devfreq，
// 否则扫描 /sys/class/kgsl/*/devfreq 取第一个含可用频率表的；找不到返回空串。
const std::string& gpu_devfreq_dir();

// 基线快照（只读记录当前值，不写入）：首次写某节点前记录当前值。
void baseline_snapshot_node(const std::string& path);

}  // namespace xdd
