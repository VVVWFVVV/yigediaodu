#pragma once
#include <string>

namespace xdd {

// ============================================================
// fas-rs 帧感知调度托管（v1.0.17）
//
// fas-rs 是第三方用户态帧感知调度守护进程（eBPF 版，GPL-3.0，
// https://github.com/shadow3aaa/fas-rs），由本模块捆绑并由守护进程托管：
//   - 频率维度（scaling_min_freq）交给 fas-rs——它监视实际渲染帧，
//     在帧 deadline 前预调频率，比我们的 jank% 反应式阶梯更精准
//   - 其余干预（绑核/cpuset/IO/内存/温控/GPU）仍由本模块负责，两层不重叠
//   - per-app 配置是唯一配置源：fas.enable / fas.target_fps 两个键，
//     由守护进程生成 fas-rs 的 games.toml
// ============================================================

// 二进制路径（<模块目录>/libs/arm64-v8a/fas-rs，模块文件=真实路径）
std::string fas_binary_path();

// 二进制是否存在（真实路径直查，不过 XD_ROOT 前缀）
bool fas_binary_exists();

// games.toml 路径（运行时目录 config/fas_games.toml，守护进程生成）
std::string fas_toml_path();

// 该应用是否在 per-app 配置中启用了 fas（读 fas.enable 键）
bool fas_enabled_for(const std::string& pkg);

// 生成 games.toml：扫描 config/perapp/*.conf 中 fas.enable=1 的应用。
// 内容同时双写：内部运行时目录（原子写）+ fas-rs 的 USER_CONFIG
// /sdcard/Android/fas-rs/games.toml（就地写保留 inode，供其 inotify 热加载）——
// 该文件是 fas-rs 的权威配置且启动时必读，缺失会导致它 panic（v1.0.19）。
// 返回纳入管理的应用数；两处内容都已相同返回 -1（无需重启 fas-rs）。
int fas_generate_toml();

// 重置 fas-rs 失败计数为 0（v1.0.20）。
// 计数器跨重启 persisted 曾导致 3 次失败后 fas-rs 永不再启动（根因修复后
// 也被挡），调用点：daemon 启动与 fas_poll 节点就绪。
void fas_reset_retry_count();

// 进程管理 ----------------------------------------------------
// 启动 fas-rs（异步非阻塞，v1.0.18）。已在跑返回 true；二进制缺失返回 false。
// fork 后立即返回，节点就绪由 fas_poll() 在主循环轮询确认。
// 失败节流：连续失败达阈值后本会话不再自动启动；两次尝试间隔至少 5 分钟。
bool fas_start();
// 轮询 pending 启动状态（主循环周期调用）：
//   节点就绪返回 true；无 pending 或仍在等待返回 false；
//   超时（5 秒节点未出现）则清理进程、累计失败计数并返回 false。
bool fas_poll();
// 停止 fas-rs 并把频率节点还原到接管前的基线值
void fas_stop();
// fas-rs 是否在运行（/dev/fas_rs/mode 节点存在——它由 fas-rs 运行时创建）
bool fas_running();

// 模式仲裁 ----------------------------------------------------
// 写 /dev/fas_rs/mode：powersave（热）/ balance（默认）/ performance（卡顿）。
// 写入失败静默（节点不存在或只读时不影响其他干预）。
void fas_set_mode(const std::string& mode);

// 频率基线 ----------------------------------------------------
// 接管 min_freq 前快照当前值（只读不写），退出时写回——
// 防止 fas-rs 退出后 min_freq 留在高频值耗电
void fas_snapshot_freq_baseline();
void fas_restore_freq_baseline();

// 状态（live 快照 / 自检用）
struct FasStatus {
    bool running = false;
    std::string mode;      // 当前模式（读不到为空）
    int managed = 0;       // games.toml 管理的应用数
};
FasStatus fas_status();

}  // namespace xdd
