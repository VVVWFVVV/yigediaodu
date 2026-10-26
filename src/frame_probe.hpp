#pragma once
#include <string>

#include "config.hpp"

namespace xdd {

// ============================================================
// 帧监控探针（v1.1.0）
//
// 路线（2026-10-25 真机实测跑通）：tracefs uprobe 挂 vendor Adreno 驱动的
// eglSwapBuffers（每帧必走），每秒读 uprobe_profile 计数差分 = 实时 FPS。
// 这台设备上 perf_event_open+BPF 路线（aya/fas-rs 的唯一路线）在内核层被
// 阉割、/system/lib64 图形栈 uprobe 注册被 EBUSY 挡、动态事件不进任何可读
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
    int target_fps = 60;            // 目标帧率（perapp 与 fas.target_fps 同值）
    int low_ratio = 90;             // 百分比：FPS < target*low_ratio/100 视为掉帧
    int min_hold_samples = 2;       // 连续低 FPS 采样次数才判掉帧（防抖）
};

bool load_frame_probe_config(FrameProbeConfig& out, const Conf* overrides = nullptr);

// 生命周期：游戏族前台时 start，离开即 stop（卸载 uprobe 事件）
bool frame_probe_start(const FrameProbeConfig& cfg);
void frame_probe_stop();
bool frame_probe_active();

// 主循环每次迭代调用（内部 1s 节流）：采样计数差分并自校验
void frame_probe_tick();

// 最近一次有效 FPS（-1=探针未激活/尚无数据）；fps 阶梯决策用
int frame_probe_last_fps();

// 探针状态（capability/live 用）："off" / "ok" / "degraded"
std::string frame_probe_state();

}  // namespace xdd
