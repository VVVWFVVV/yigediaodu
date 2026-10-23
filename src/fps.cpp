#include "fps.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "engine.hpp"
#include "fas.hpp"
#include "log.hpp"
#include "paths.hpp"
#include "shell.hpp"
#include "state.hpp"

namespace xdd {

namespace {

using Clock = std::chrono::steady_clock;

// 每策略组一套最低频阶梯（下标 0 = 不干预档）
std::vector<std::vector<long long>> g_steps;
// 温控封顶前保存的各策略组 scaling_max_freq
std::vector<std::string> g_saved_max_freq;

Clock::time_point g_last_sample{};
int g_level = 0;
long long g_level_since_ms = 0;
double g_last_jank = -1.0;  // -1=尚无采样数据
bool g_capped = false;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

std::vector<long long> parse_steps(const Conf& kv, const std::string& key) {
    std::vector<long long> out;
    std::string v;
    if (!conf_get(kv, key, v) || v.empty()) return out;
    std::stringstream ss(v);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (!part.empty()) out.push_back(std::atoll(part.c_str()));
    }
    return out;
}

// 解析 "Janky frames: 6 (1.00%)" -> 1.0
double parse_jank_pct(const std::string& text) {
    size_t pos = text.find("Janky frames:");
    if (pos == std::string::npos) return -1.0;
    size_t open = text.find('(', pos);
    size_t close = text.find(')', pos);
    if (open == std::string::npos || close == std::string::npos || close < open) return -1.0;
    std::string clean;
    for (size_t i = open + 1; i < close; ++i) {
        char c = text[i];
        if (std::isdigit((unsigned char)c) || c == '.') clean += c;
    }
    if (clean.empty()) return -1.0;
    return std::atof(clean.c_str());
}

// 可用档位上限 = 各组阶梯长度的最小值（避免某组越界）
size_t max_level() {
    size_t m = 0;
    for (const auto& s : g_steps) {
        if (s.empty()) continue;
        if (m == 0 || s.size() < m) m = s.size();
    }
    return m;
}

}  // namespace

bool load_fps_config(FpsConfig& out, const std::vector<PolicyInfo>& policies,
                     const Conf* overrides) {
    auto pr = conf_parse_file(settings_path());
    const Conf& kv = pr.kv;
    std::string v;

    if (conf_get(kv, "fps.enable", v)) out.enable = (v != "0" && v != "false");
    if (conf_get(kv, "fps.sample_ms", v)) out.sample_ms = std::atoi(v.c_str());
    if (conf_get(kv, "fps.jank_threshold", v)) out.jank_threshold = std::atoi(v.c_str());
    if (conf_get(kv, "fps.hold_ms", v)) out.hold_ms = std::atoi(v.c_str());

    g_steps.clear();
    for (size_t i = 0; i < policies.size(); ++i) {
        g_steps.push_back(parse_steps(kv, "fps.steps.p" + std::to_string(i)));
    }

    // v1.0.16：overrides（档位 + perapp 的 fps.* 键，按序叠加，后者优先）——
    // 逐条 apply 到已解析的基础值上，实现每游戏独立稳帧阶梯。
    // 只认 fps. 前缀的键；其余键（mode/cpu.* 等）由 apply_profile 消费，忽略。
    if (overrides) {
        for (const auto& kvp : *overrides) {
            const std::string& k = kvp.first;
            const std::string& val = kvp.second;
            if (k.rfind("fps.", 0) != 0) continue;
            if (k == "fps.enable") {
                out.enable = (val != "0" && val != "false");
            } else if (k == "fps.sample_ms") {
                out.sample_ms = std::atoi(val.c_str());
            } else if (k == "fps.jank_threshold") {
                out.jank_threshold = std::atoi(val.c_str());
            } else if (k == "fps.hold_ms") {
                out.hold_ms = std::atoi(val.c_str());
            } else if (k.rfind("fps.steps.p", 0) == 0 && k.size() > 11) {
                // fps.steps.p<N>：替换对应策略组阶梯（数字从索引 11 开始：
                // "fps.steps.p0" 共 12 字符，p 在索引 10，数字在 11）
                size_t idx = (size_t)std::atoi(k.substr(11).c_str());
                if (idx < g_steps.size()) {
                    std::stringstream ss(val);
                    std::string part;
                    std::vector<long long> steps;
                    while (std::getline(ss, part, ',')) {
                        if (!part.empty()) steps.push_back(std::atoll(part.c_str()));
                    }
                    if (!steps.empty()) g_steps[idx] = steps;
                }
            }
        }
    }

    if (out.sample_ms < 500) out.sample_ms = 500;
    if (out.jank_threshold < 1) out.jank_threshold = 1;
    if (out.hold_ms < 1000) out.hold_ms = 1000;
    return true;
}

bool load_thermal_config(ThermalConfig& out, const std::vector<PolicyInfo>& policies) {
    auto pr = conf_parse_file(settings_path());
    const Conf& kv = pr.kv;
    std::string v;
    if (conf_get(kv, "thermal.enable", v)) out.enable = (v != "0" && v != "false");
    if (conf_get(kv, "thermal.cap_temp", v)) out.cap_temp = std::atoi(v.c_str());
    if (conf_get(kv, "thermal.recover_temp", v)) out.recover_temp = std::atoi(v.c_str());

    out.cap_freq.clear();
    for (size_t i = 0; i < policies.size(); ++i) {
        std::string key = "thermal.cap.p" + std::to_string(i);
        std::string fv;
        out.cap_freq.push_back(conf_get(kv, key, fv) ? std::atoll(fv.c_str()) : 0);
    }
    if (out.recover_temp >= out.cap_temp) out.recover_temp = out.cap_temp - 2000;
    g_saved_max_freq.assign(policies.size(), std::string());
    return true;
}

void fps_tick(const std::string& pkg, const FpsConfig& cfg,
              const std::vector<PolicyInfo>& policies, int temp_milli) {
    (void)temp_milli;  // 温度决策在 thermal_tick，此处只用帧统计
    if (!cfg.enable || pkg.empty() || policies.empty() || g_steps.empty()) return;

    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_sample).count();
    if (elapsed < cfg.sample_ms) return;
    g_last_sample = now;

    // 采样帧统计（带超时，绝不让 dumpsys 拖住主循环）
    auto sr = shell_run("dumpsys gfxinfo " + pkg + " framestats", 3000);
    if (sr.timed_out) {
        log_line(LogLevel::Warn, "稳帧采样超时，本轮跳过");
        return;
    }
    double jank = parse_jank_pct(sr.out);
    if (jank < 0) return;  // 输出没有帧统计（应用可能不在前台渲染）
    g_last_jank = jank;

    // 决策：超阈值升档（保持窗口已过才升——防卡顿持续时几秒内冲到顶档）；
    //       低于阈值一半且保持窗口已过才降档（滞回防震荡）
    size_t max_lv = max_level();
    bool changed = false;
    if (jank >= (double)cfg.jank_threshold) {
        if (max_lv > 0 && (size_t)(g_level + 1) < max_lv &&
            now_ms() - g_level_since_ms >= cfg.hold_ms) {
            ++g_level;
            g_level_since_ms = now_ms();
            changed = true;
            log_line(LogLevel::Info, "稳帧升档 -> " + std::to_string(g_level) +
                                         "（卡顿率 " + std::to_string(jank) + "%）");
        }
    } else if (jank < (double)cfg.jank_threshold / 2.0) {
        if (g_level > 0 && now_ms() - g_level_since_ms >= cfg.hold_ms) {
            --g_level;
            g_level_since_ms = now_ms();
            changed = true;
            log_line(LogLevel::Info, "稳帧降档 -> " + std::to_string(g_level) +
                                         "（卡顿率 " + std::to_string(jank) + "%）");
        }
    }

    // 落地：每策略组最低频 = 该组阶梯[当前档]；档 0 视为不干预
    if (changed || g_level > 0) {
        if (freq_defer_fas()) {
            // v1.0.17：当前应用交给 fas-rs（帧感知调度）管频率——
            // 它监视实际渲染帧在 deadline 前预调，比我们的反应式阶梯更精准。
            // 我们仍统计卡顿（供 WebUI 与模式仲裁），但不写 scaling_min_freq。
            // 注意条件是「当前应用 fas 托管」而非「fas-rs 进程在跑」——
            // 非 fas 游戏（如 PUBG 默认关）即使 fas-rs 空闲运行也走我们的阶梯。
            set_capability("fps", "fas");
        } else {
            for (size_t i = 0; i < policies.size() && i < g_steps.size(); ++i) {
                const auto& steps = g_steps[i];
                if (steps.empty()) continue;
                long long f = steps[std::min((size_t)g_level, steps.size() - 1)];
                if (f <= 0) continue;
                // 频率吸附：阶梯值不在该策略组可用频率表时向上取整到合法值
                // （v1.0.12 实测：非法值被内核钳位，稳帧调频全部失效）
                long long snapped = snap_to_available_freq(policies[i].dir, f);
                nd_write(policies[i].dir + "/scaling_min_freq", std::to_string(snapped));
            }
            set_capability("fps", "ok");
        }
    }
}

bool thermal_tick(const ThermalConfig& cfg, const std::vector<PolicyInfo>& policies, int temp_milli) {
    if (!cfg.enable || policies.empty()) return g_capped;

    if (!g_capped && temp_milli >= cfg.cap_temp) {
        g_capped = true;
        for (size_t i = 0; i < policies.size(); ++i) {
            std::string node = policies[i].dir + "/scaling_max_freq";
            if (g_saved_max_freq[i].empty()) {
                nd_read(node, g_saved_max_freq[i]);  // 封顶前保存当前上限
            }
            if (i < cfg.cap_freq.size() && cfg.cap_freq[i] > 0) {
                // 封顶值向下吸附到合法频率（绝不超过目标，保证封顶有效）
                long long capped = snap_to_available_freq(policies[i].dir,
                                                          cfg.cap_freq[i], false);
                nd_write(node, std::to_string(capped));
            }
        }
        set_capability("thermal", "ok");
        log_line(LogLevel::Warn, "温控封顶生效：温度 " + std::to_string(temp_milli / 1000) + "℃");
    } else if (g_capped && temp_milli <= cfg.recover_temp) {
        g_capped = false;
        for (size_t i = 0; i < policies.size(); ++i) {
            if (g_saved_max_freq[i].empty()) continue;
            nd_write(policies[i].dir + "/scaling_max_freq", g_saved_max_freq[i]);
            g_saved_max_freq[i].clear();
        }
        log_line(LogLevel::Info, "温控封顶解除：温度 " + std::to_string(temp_milli / 1000) + "℃");
    }
    return g_capped;
}

int fps_current_level() { return g_level; }
bool thermal_capped() { return g_capped; }

// 最近一次卡顿率（-1=无数据）；fas 模式仲裁用
double fps_last_jank() { return g_last_jank >= 0 ? g_last_jank : -1.0; }

// 阶梯摘要："p0:384000,1152000|p1:1017600,..."（诊断/测试用）
std::string fps_steps_summary() {
    std::string out;
    for (size_t i = 0; i < g_steps.size(); ++i) {
        if (!out.empty()) out += "|";
        out += "p" + std::to_string(i) + ":";
        for (size_t j = 0; j < g_steps[i].size(); ++j) {
            if (j > 0) out += ",";
            out += std::to_string(g_steps[i][j]);
        }
    }
    return out;
}

}  // namespace xdd
