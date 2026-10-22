#pragma once
#include <string>

namespace xdd {

struct ForegroundInfo {
    std::string pkg;            // 前台包名（空 = 未识别）
    int oom_adj = 9999;         // 命中进程的 oom_score_adj
    bool from_dumpsys = false;  // 结果是否来自 dumpsys 兜底
};

// 前台应用识别：/proc 扫描为主（零外部进程），dumpsys 兜底（内部节流 5s）
ForegroundInfo detect_foreground();

// 亮灭屏：dumpsys power（内部节流 5s，带超时）
bool screen_on();

// 场景判定 → family 名：
//   off          该应用被显式关闭干预
//   bg           熄屏
//   game         命中游戏集合
//   daily        其余（默认保守）
// perapp_mode 为 perapp 配置中显式指定的档位（空 = 未指定）
std::string resolve_family(const std::string& pkg, bool screen_on_now, const std::string& perapp_mode);

// 内置游戏包名前缀（保守列表；漏网应用由 perapp 配置补齐）
bool is_builtin_game(const std::string& pkg);

}  // namespace xdd
