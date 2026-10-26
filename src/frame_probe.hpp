#pragma once
#include <string>

#include "config.hpp"

namespace xdd {

// ============================================================
// 帧监控探针（v1.1.0）
//
// 路线（2026-10-25 真机实测跑通）：tracefs uprobe 挂 vendor Adreno 驱动的
// eglSwapBuffers（每帧必走），每秒读 uprobe_profile 计数差分 = 实时 FPS。
// 这台设备上 perf_event_open+BPF 路线在内核层被阉割、
// /system/lib64 图形栈 uprobe 注册被 EBUSY 挡、动态事件不进任何可读
// 缓冲（tracing_on=1 也不行、trace_pipe 被 Perfetto 常驻占住）——计数差分
// 是唯一可用的帧信号消费途径。代价：只有 FPS、没有每帧时间戳（卡顿帧定位
// 仍靠 gfxinfo framestats）。
//
// 计数为全进程叠加：仅前台为单一游戏时≈游戏帧率（SurfaceFlinger 合成不
// 混入此计数，实测 60fps 游戏得 60.67fps）。后台有其他 GL 应用时计数偏大
// ——偏大会被判定「帧率健康」，是保守方向，不会误升档。
//
// 自校验：启动 5 秒计数从未变化 → 符号偏移失效（OTA/换设备）→ 自动停用
// 并回退卡顿率阶梯，不会把「没信号」当「0 帧」。
// ============================================================

// 帧探针配置（settings.conf：fps.frame_probe / fps.target_fps / fps.low_ratio
// 与 frame.lib_path / frame.symbol_off；perapp 可用同名键覆盖——与 fps.*
// 阶梯共用同一套 overrides 通道）
struct FrameProbeConfig {
    bool enable = false;
    std::string lib_path = "/vendor/lib64/libEGL_adreno.so";
    long long symbol_off = 0x9ed0;  // eglSwapBuffers（2026-10-25 真机实测）
    int target_fps = 60;            // 目标帧率（perapp 与 fps.target_fps 同值）
    int low_ratio = 90;             // 百分比：FPS < target*low_ratio/100 视为掉帧
    int min_hold_samples = 2;       // 连续低 FPS 采样次数才判掉帧（防抖）
};

bool load_frame_probe_config(FrameProbeConfig& out, const Conf* overrides = nullptr);

// 生命周期：游戏族前台时 start，离开即 stop（卸载 uprobe 事件）
bool frame_probe_start(const FrameProbeConfig& cfg);
void frame_probe_stop();
bool frame_probe_active();

// 配置热重载：卸载并允许重新探测（清掉 g_gave_up 的粘性失败状态）
void frame_probe_reset();

// 主循环每次迭代调用（内部 1s 节流）：采样计数差分并自校验
void frame_probe_tick();

// 最近一次有效 FPS（-1=探针未激活/尚无数据）；fps 阶梯决策用
int frame_probe_last_fps();

// 探针快信号：相对健康基线的骤降（连续 2 次低于基线 80%）。
// 绝对值有设备级偏差，不能直接和目标帧率比；真实 FPS 由 gfxinfo 提供。
bool frame_probe_drop();

// 当前健康基线（原始计数 FPS；-1=预热中/未激活）。用于判断探针绝对值
// 是否可信：基线太低（游戏还在加载/黑屏）时不能用绝对值判掉帧。
int frame_probe_ref_fps();

// 最近 12 秒的探针 FPS 峰值。用于判断“当前是否真的在出帧”：
// 大厅/加载可能先短暂高帧再长期低帧，历史最大值（ref）会污染判据。
int frame_probe_recent_max();

// 探针启动至今的毫秒数（-1=未激活）。游戏启动/加载阶段需要宽限，
// 避免把加载界面的低帧误判成掉帧。
int frame_probe_age_ms();

// 探针状态（capability/live 用）："off" / "ok" / "degraded"
std::string frame_probe_state();

}  // namespace xdd
