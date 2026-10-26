#include "fps.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "engine.hpp"
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
double g_last_jank = -1.0;      // -1=尚无采样数据
double g_last_real_fps = -1.0;  // gfxinfo framestats 算出的应用真实 FPS
long long g_last_gfxinfo_ms = 0;
long long g_last_total_frames = -1;  // 上次 gfxinfo 的总帧数（判断统计是否更新）
bool g_capped = false;
long long g_thermal_changed_ms = 0;  // 上次封顶/解除时刻（最小驻留时间防抖）
const long long kThermalDwellMs = 15000;
int g_fps_low_hits = 0;  // 连续低 FPS 采样计数（慢信号防抖）
bool g_stats_logged = false;
bool g_short_logged = false;
long long g_session_start_ms = 0;  // 本次应用切换时刻（启动宽限基准）
bool g_grace_logged = false;

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

// 解析 "Total frames rendered: 600" -> 600（-1=输出里没有）
long long parse_total_frames(const std::string& text) {
    size_t pos = text.find("Total frames rendered:");
    if (pos == std::string::npos) return -1;
    pos += std::strlen("Total frames rendered:");
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    char* end = nullptr;
    long long v = std::strtoll(text.c_str() + pos, &end, 10);
    return (end && end != text.c_str() + pos && v >= 0) ? v : -1;
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

std::string trim_str(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            out.push_back(trim_str(cur));
            cur.clear();
        } else if (c != '\r' && c != '\n') {
            cur += c;
        }
    }
    out.push_back(trim_str(cur));
    return out;
}

// 从 gfxinfo framestats 的 ---PROFILEDATA--- 段计算应用真实 FPS。
// 输出示例（一加平板 Android 17）：
//   Flags,...,SwapBuffers,FrameCompleted,DequeueBufferDuration,...
//   0,...,5289683107667,5289685373292,22604,...
// 取最近 120 帧的 FrameCompleted 时间戳差；返回 -1 表示没有可用帧。
double parse_fps_from_framestats(const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    int fc_idx = -1;
    std::vector<long long> times;

    while (std::getline(ss, line)) {
        std::string t = trim_str(line);
        if (t.empty()) continue;
        if (t.rfind("Flags,", 0) == 0) {
            auto cols = split_csv(t);
            int idx = -1;
            for (size_t i = 0; i < cols.size(); ++i) {
                if (cols[i] == "FrameCompleted") {
                    idx = (int)i;
                    break;
                }
            }
            if (idx >= 0) {
                fc_idx = idx;
                times.clear();  // 新 section：只统计最后一段窗口
            }
            continue;
        }
        if (fc_idx < 0 || !std::isdigit((unsigned char)t[0])) continue;
        auto cols = split_csv(t);
        if ((int)cols.size() <= fc_idx) continue;
        char* end = nullptr;
        long long v = std::strtoll(cols[fc_idx].c_str(), &end, 10);
        if (end && *end == '\0' && v > 0) times.push_back(v);
    }

    if (times.size() < 10) return -1.0;
    if (times.size() > 120) times.erase(times.begin(), times.end() - 120);
    long long span = times.back() - times.front();
    if (span <= 0) return -1.0;
    double fps = (double)(times.size() - 1) * 1e9 / (double)span;
    // 启动/切换瞬间可能只有 1-2 帧，算出的 1fps 不是有效帧率；
    // 低于 10fps 在真机上基本只可能是统计残片，直接判无效。
    if (fps < 10.0) return -1.0;
    if (fps > 300.0) fps = 300.0;
    return fps;
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
    if (conf_get(kv, "fps.startup_grace_ms", v)) out.startup_grace_ms = std::atoi(v.c_str());

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
            } else if (k == "fps.startup_grace_ms") {
                out.startup_grace_ms = std::atoi(val.c_str());
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
    if (out.startup_grace_ms < 0) out.startup_grace_ms = 0;
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

void fps_tick(const std::string& pkg, const FpsConfig& cfg, const FrameProbeConfig* fpc,
              const std::vector<PolicyInfo>& policies, int temp_milli) {
    (void)temp_milli;  // 温度决策在 thermal_tick，此处只用帧统计
    if (!cfg.enable || pkg.empty() || policies.empty() || g_steps.empty()) return;

    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_sample).count();
    if (elapsed < cfg.sample_ms) return;
    g_last_sample = now;

    // v1.2.0：gfxinfo 是重操作（root shell + 数百 KB 输出），探针在跑时
    // 降到 5s 一次；中间 2s 的 tick 只用探针快信号做决策。
    long long nowms = now_ms();
    int grace_ms = cfg.startup_grace_ms;
    // 测试接缝：host 测试用短睡眠，设置该环境变量跳过启动宽限
    if (const char* e = std::getenv("XDD_TEST_NO_STARTUP_GRACE")) {
        if (e && *e) grace_ms = 0;
    }
    bool startup = (g_session_start_ms > 0 && nowms - g_session_start_ms < grace_ms);
    if (startup && !g_grace_logged) {
        g_grace_logged = true;
        log_line(LogLevel::Info, "启动宽限中（" + std::to_string(grace_ms / 1000) +
                                     "s）：不因加载/菜单低帧升档");
    }
    long long gfx_interval = cfg.sample_ms;
    if (fpc && fpc->enable && frame_probe_active() && gfx_interval < 5000) {
        gfx_interval = 5000;
    }
    if (nowms - g_last_gfxinfo_ms >= gfx_interval) {
        auto sr = shell_run("dumpsys gfxinfo " + pkg + " framestats", 3000);
        if (sr.timed_out) {
            log_line(LogLevel::Warn, "稳帧采样超时，沿用上次帧统计");
        } else {
            long long total = parse_total_frames(sr.out);
            // 启动/切换瞬间 gfxinfo 可能只有个位数帧（和平精英实测 8 帧、5 个
            // 卡顿=62.5%），这不是有效帧统计；总量不足 30 帧时清掉，避免
            // 启动宽限结束后拿这份假卡顿率升档。
            bool fresh = (total < 0) || (total >= 30 && total >= g_last_total_frames);
            if (fresh) {
                double j = parse_jank_pct(sr.out);
                double f = parse_fps_from_framestats(sr.out);
                if (j >= 0) g_last_jank = j;
                if (f >= 0) g_last_real_fps = f;
                if (!g_stats_logged && (j >= 0 || f >= 0)) {
                    g_stats_logged = true;
                    log_line(LogLevel::Info, "帧统计: jank=" +
                                                 (j >= 0 ? std::to_string(j) : std::string("?")) +
                                                 "% real_fps=" +
                                                 (f >= 0 ? std::to_string((int)(f + 0.5))
                                                         : std::string("?")) +
                                                 " total=" + std::to_string(total));
                }
                if (total >= 0) g_last_total_frames = total;
            } else {
                if (total >= 0 && total < 30) {
                    g_last_jank = -1.0;
                    g_last_real_fps = -1.0;
                    if (!g_short_logged) {
                        g_short_logged = true;
                        log_line(LogLevel::Info,
                                 "帧统计不足（total=" + std::to_string(total) +
                                     "），忽略本次 jank/FPS，等游戏真正出帧");
                    }
                }
            }
            g_last_gfxinfo_ms = nowms;
        }
    }

    double jank = g_last_jank;
    double real_fps = g_last_real_fps;

    // 帧探针：1s 采样的原始计数 FPS。绝对值和设备上的其它 GL 消费者叠加，
    // 但很多游戏（Unity/Vulkan/SurfaceView）的 gfxinfo framestats 是空的，
    // 此时它是唯一可用的帧率信号；有 framestats 时才降级为相对基线快信号。
    int probe_fps = (fpc && fpc->enable) ? frame_probe_last_fps() : -1;
    int target = (fpc && fpc->target_fps >= 30) ? fpc->target_fps : 60;
    int low_ratio = (fpc && fpc->low_ratio >= 50) ? fpc->low_ratio : 90;
    int low_th = target * low_ratio / 100;
    int probe_recent = (fpc && fpc->enable) ? frame_probe_recent_max() : -1;
    int probe_age = (fpc && fpc->enable) ? frame_probe_age_ms() : -1;
    bool probe_drop = (fpc && fpc->enable) ? frame_probe_drop() : false;
    // 启动/加载阶段（明日方舟实测约 1-2 分钟才进对局）帧率天然很低，
    // 不能当作掉帧。探针相关的判据统一宽限 2 分钟；期间只保留卡顿率升档。
    bool warmup = (probe_age >= 0 && probe_age < 120000);
    if (warmup) probe_drop = false;
    // 探针绝对值只在“最近 12 秒确实接近目标帧率”时使用：和平精英大厅/
    // 加载时只有 28-111fps，而 target=144；若用历史最大值（启动 splash 可能
    // 170fps）做基线，2 分钟后仍会拿大厅低帧升档。
    bool probe_ready = probe_age >= 120000 && probe_recent >= target * 90 / 100;
    if (jank < 0 && real_fps < 0 && probe_fps < 0 && !probe_drop) return;

    // 真实 FPS 优先；拿不到（gfxinfo 无帧）时回退探针绝对值。
    bool fps_low_raw = real_fps >= 0 ? (real_fps < (double)low_th)
                                     : (probe_ready && probe_fps >= 0 && probe_fps < low_th);
    if (warmup && real_fps < 0) fps_low_raw = false;  // 仅探针路径宽限
    if (fps_low_raw) {
        ++g_fps_low_hits;
    } else {
        g_fps_low_hits = 0;
    }
    int need = (fpc && fpc->min_hold_samples > 0) ? fpc->min_hold_samples : 2;
    // 探针骤降只作加速器：必须同时有“真实 FPS 低”或“探针绝对值低”，
    // 否则 75→60 这种计数波动会在 60fps 目标下被误判为掉帧。
    bool probe_drop_low = probe_drop &&
                          ((real_fps >= 0 && real_fps < (double)target * 95 / 100) ||
                           (real_fps < 0 && probe_fps >= 0 && probe_fps < low_th));
    bool fps_low = (fps_low_raw && g_fps_low_hits >= need) || probe_drop_low;
    bool fps_healthy = false;
    if (real_fps >= 0) {
        fps_healthy = real_fps >= (double)target * 98 / 100;
    } else if (probe_ready && probe_fps >= 0) {
        // 探针计数天然高于/低于真实帧率，健康线放宽到 95%
        fps_healthy = probe_fps >= (double)target * 95 / 100;
    }
    fps_healthy = fps_healthy && !probe_drop;
    set_capability("frame", frame_probe_state());

    // 决策：超阈值升档（保持窗口已过才升——防卡顿持续时几秒内冲到顶档）；
    //       低于阈值一半且保持窗口已过才降档（滞回防震荡）。
    // 升档判据 = 卡顿率超标 **或** 真实 FPS 低 **或** 探针确认骤降（任一从快）；
    // 降档判据 = 卡顿率低 **且** 真实帧率健康——双信号确认才降。
    size_t max_lv = max_level();
    bool changed = false;
    if (!startup && ((jank >= 0 && jank >= (double)cfg.jank_threshold) || fps_low)) {
        if (max_lv > 0 && (size_t)(g_level + 1) < max_lv &&
            now_ms() - g_level_since_ms >= cfg.hold_ms) {
            ++g_level;
            g_level_since_ms = now_ms();
            changed = true;
            std::string why;
            if (jank >= 0 && jank >= (double)cfg.jank_threshold) {
                why = "卡顿率 " + std::to_string(jank) + "%";
            } else if (real_fps >= 0) {
                why = "实时帧率 " + std::to_string((int)(real_fps + 0.5)) + "fps";
            } else if (probe_fps >= 0) {
                why = "探针帧率 " + std::to_string(probe_fps) + "fps";
            } else {
                why = "帧探针骤降";
            }
            log_line(LogLevel::Info, "稳帧升档 -> " + std::to_string(g_level) + "（" + why + "）");
        }
    } else if (jank >= 0 && jank < (double)cfg.jank_threshold / 2.0 && fps_healthy) {
        if (g_level > 0 && now_ms() - g_level_since_ms >= cfg.hold_ms) {
            --g_level;
            g_level_since_ms = now_ms();
            changed = true;
            log_line(LogLevel::Info, "稳帧降档 -> " + std::to_string(g_level) +
                                         "（卡顿率 " + std::to_string(jank) +
                                         "%，帧率 " +
                                         std::to_string((int)((real_fps >= 0 ? real_fps
                                                                            : (double)probe_fps) +
                                                               0.5)) +
                                         "fps）");
        }
    }

    // 落地：每策略组最低频 = 该组阶梯[当前档]；档 0 视为不干预
    if (changed || g_level > 0) {
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

bool thermal_tick(const ThermalConfig& cfg, const std::vector<PolicyInfo>& policies,
                  int temp_milli, const std::string& zone) {
    if (!cfg.enable || policies.empty()) return g_capped;

    long long now = now_ms();
    bool can_toggle = (g_thermal_changed_ms == 0) ||
                      (now - g_thermal_changed_ms >= kThermalDwellMs);

    if (!g_capped && temp_milli >= cfg.cap_temp && can_toggle) {
        g_capped = true;
        g_thermal_changed_ms = now;
        for (size_t i = 0; i < policies.size(); ++i) {
            std::string node = policies[i].dir + "/scaling_max_freq";
            if (g_saved_max_freq[i].empty()) {
                nd_read(node, g_saved_max_freq[i]);  // 封顶前保存当前上限
            }
            if (i < cfg.cap_freq.size() && cfg.cap_freq[i] > 0) {
                // 封顶值向下吸附到合法频率（绝不超过目标，保证封顶有效）
                long long capped = snap_to_available_freq(policies[i].dir,
                                                          cfg.cap_freq[i], false);
                // v1.2.0：把封顶前的 max_freq 记入基线，否则切到非游戏档时
                // restore_all 找不到这个节点，封顶会永久残留（旧版 live 里
                // THERM=1/thermal=off 的矛盾即此）。
                baseline_snapshot_node(node);
                nd_write(node, std::to_string(capped));
            }
        }
        set_capability("thermal", "ok");
        log_line(LogLevel::Warn, "温控封顶生效：CPU/GPU " + std::to_string(temp_milli / 1000) +
                                     "℃（zone=" + (zone.empty() ? "?" : zone) + "）");
    } else if (g_capped && temp_milli <= cfg.recover_temp && can_toggle) {
        g_capped = false;
        g_thermal_changed_ms = now;
        for (size_t i = 0; i < policies.size(); ++i) {
            if (g_saved_max_freq[i].empty()) continue;
            nd_write(policies[i].dir + "/scaling_max_freq", g_saved_max_freq[i]);
            g_saved_max_freq[i].clear();
        }
        log_line(LogLevel::Info, "温控封顶解除：CPU/GPU " + std::to_string(temp_milli / 1000) +
                                     "℃（zone=" + (zone.empty() ? "?" : zone) + "）");
    }
    return g_capped;
}

int fps_current_level() { return g_level; }
bool thermal_capped() { return g_capped; }

void fps_reset() {
    g_level = 0;
    g_level_since_ms = 0;
    g_last_jank = -1.0;
    g_last_real_fps = -1.0;
    g_last_gfxinfo_ms = 0;
    g_last_total_frames = -1;
    g_fps_low_hits = 0;
    g_stats_logged = false;
    g_short_logged = false;
    g_session_start_ms = now_ms();
    g_grace_logged = false;
}

void thermal_reset() {
    g_capped = false;
    g_thermal_changed_ms = 0;
    for (auto& v : g_saved_max_freq) v.clear();
}

// 最近一次卡顿率（-1=无数据）
double fps_last_jank() { return g_last_jank >= 0 ? g_last_jank : -1.0; }

double fps_last_real_fps() { return g_last_real_fps; }

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
