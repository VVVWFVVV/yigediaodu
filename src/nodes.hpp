#pragma once
#include <string>
#include <vector>

namespace xdd {

// ============================================================
// 全模块唯一节点读写面：其他代码禁止直接 open sysfs
// 所有函数接收逻辑绝对路径（"/sys/..."），内部经 nd_path 前缀化
// ============================================================

// 读取节点内容（去首尾空白）；失败返回 false
bool nd_read(const std::string& path, std::string& out);

// 读取整数节点
bool nd_read_int(const std::string& path, long long& out);

// 节点是否存在
bool nd_exists(const std::string& path);

// 写入并回读自证：写后立即读回比对，不一致重试一次；
// 打不开且 SELinux enforcing 时自动走一次瞬时 permissive 重试
// （读 enforce 原值→写 0→重试→恢复原值；不做存在性前置判断——
//  SELinux 拦读时 nd_exists 不可信），仍失败记 WARN 返回 false
bool nd_write(const std::string& path, const std::string& value);

// 裸写（无回读、无基线、无重试）：仅用于「一次性动作」型节点——
// 如 /proc/sys/vm/drop_caches（读回恒 0，nd_write 的回读必失败）。
// 不创建文件（open 无 O_CREAT）；成功返回 true
bool raw_write_node(const std::string& path, const std::string& value);

// cpufreq 策略组信息
struct PolicyInfo {
    std::string dir;    // policy 目录逻辑路径（/sys/devices/system/cpu/cpufreq/policyN）
    std::string cpus;   // related_cpus 内容，如 "0-5"
    long long min_freq = 0;  // cpuinfo_min_freq
    long long max_freq = 0;  // cpuinfo_max_freq
};

// 枚举 cpufreq 策略组：优先 policy* 目录，缺失时回退 cpu*/cpufreq 按 related_cpus 归并
std::vector<PolicyInfo> detect_policies();

// 最高温度（毫度）与温区名
struct TempInfo {
    int max_milli = 0;
    std::string zone;   // 最高温区 type
};
bool read_max_temp(TempInfo& out);

}  // namespace xdd
