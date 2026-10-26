#pragma once
#include <string>
#include <vector>

#include "config.hpp"
#include "frame_probe.hpp"
#include "nodes.hpp"

namespace xdd {

// 稳帧配置（settings.conf 中 fps.* 键）
struct FpsConfig {
    bool enable = true;
    int sample_ms = 2000;
    int jank_threshold = 5;  // 卡顿率阈值（百分比）
    int hold_ms = 5000;      // 阶梯保持窗口（滞回）
};

// 温控配置（settings.conf 中 thermal.* 键）
struct ThermalConfig {
    bool enable = true;
    int cap_temp = 48000;      // 毫度
    int recover_temp = 44000;  // 毫度（滞回）
    std::vector<long long> cap_freq;  // 各策略组封顶频率
};

// 载入稳帧配置：settings.conf 为基础；overrides 非空时，其中所有 fps.* 键
// 覆盖基础值（v1.0.16：档位配置与 perapp 配置按序叠加传入，实现每游戏
// 独立稳帧阶梯——override 中后面的键优先）
bool load_fps_config(FpsConfig& out, const std::vector<PolicyInfo>& policies,
                     const Conf* overrides = nullptr);
bool load_thermal_config(ThermalConfig& out, const std::vector<PolicyInfo>& policies);

// 稳帧采样与决策（内部按 sample_ms 节流）
// temp_milli 由调用方统一提供（与温控共用单一温度源）
// fpc 非空且启用时，实时 FPS（帧探针）作为第二判据：掉帧升档从快、降档
// 要求卡顿率与帧率双健康（保守，防单一信号误判）
void fps_tick(const std::string& pkg, const FpsConfig& cfg, const FrameProbeConfig* fpc,
              const std::vector<PolicyInfo>& policies, int temp_milli);

// 温控阈值封顶（只降不升、滞回恢复）；返回当前是否处于封顶状态
bool thermal_tick(const ThermalConfig& cfg, const std::vector<PolicyInfo>& policies, int temp_milli);

// 当前稳帧档位 / 温控封顶状态（live 快照用）
int fps_current_level();
bool thermal_capped();

// 最近一次卡顿率（百分比，-1=无数据）；fas 模式仲裁用
double fps_last_jank();

// 当前稳帧阶梯摘要（诊断/测试用）："p0:384000,1152000|p1:1017600,..."
std::string fps_steps_summary();

}  // namespace xdd
