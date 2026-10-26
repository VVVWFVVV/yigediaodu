# 一个调度 · 使用说明

## 这是什么

一个调度模块：自动识别前台应用，日用场景做省电治理，游戏场景做前台加速与稳帧。
守护进程是一个预编译的 arm64 二进制（设备端零编译），管理面板在 KernelSU 管理器里打开。

## 安装

1. KernelSU 管理器 → 模块 → 从本地安装 → 选择 `dist/WY925976742-v*.zip`
2. 重启
3. 模块页面点「打开」进入 WebUI；点「动作」输出只读体检

> 与旧「风驰调度」可共存，但同时干预 CPU 的模块建议只开一个。

## 面板四个标签

| 标签 | 内容 |
|---|---|
| 总览 | 在管哪个应用 / 生效哪个档 / 干预是否真的生效（绿=生效 黄=降级 灰=未开启）+ 实时温度频率 + 省电观察曲线 + 最近日志 |
| 应用 | 按应用锁定档位（如只给某个游戏用游戏档，其余应用不受影响） |
| 设置 | 常用开关（总开关/稳帧/温控联动/各能力维度）+ 折叠的高级参数 |
| 自检 | 一键跑全部探针，每条显示命令原文、耗时、退出码、输出原文，可一键复制 |

## 档位

| 档位 | 行为 |
|---|---|
| 日用（默认） | 后台进程压小核簇 + IO/内存轻调，**不动前台频率**，保证流畅不添乱 |
| 日用增强 | 在日用基础上轻抬前台最低频，适合觉得滑动卡顿时用 |
| 游戏 | 自动套用：抬大小核最低频 + 关键线程绑大核 + 稳帧 + 温控封顶 |
| 后台/熄屏 | 自动套用：激进后台治理 |
| 不干预 | perapp 里指定后该应用完全不受影响 |

场景是自动的：前台是游戏（内置大厂游戏包名 + 你配置的应用）走游戏档，其余走日用档，熄屏走后台档。
总览上的三个大按钮是「立即手动套用」，用于试用；之后场景变化仍会自动调度。

## 判断有没有效果

- **总览的能力徽标**：每个干预维度显示 已生效/已降级/未开启。写了节点但没生效会显示「已降级」并注明原因，不再靠猜。
- **省电观察**：总览底部滚动记录温度与频率。关闭模块（顶栏开关）前后各记录几分钟对比即可。
- **稳帧**：游戏内看总览「稳帧档」，卡顿率超阈值会自动升档，带滞回防震荡。

## 配置

配置在模块目录 `config/` 下，行式 `key=value`，WebUI 改配置即时生效（无需重启）：

- `settings.conf`：全局设置与能力开关
- `profiles/*.conf`：五个档位的具体干预
- `perapp/<包名>.conf`：按应用锁定档位（`mode=game` 等）

导出的方法：设置页 → 导出配置。

### per-app 独立调优

per-app 配置支持**覆盖档位同名参数**——`mode` 决定套用哪个档位，其余键覆盖该档位的参数值。例如：

```ini
# config/perapp/com.tencent.tmgp.pubgmhd.conf
# 套用游戏档
mode=game

# 覆盖：大核最低频提高（游戏档默认 1958400）
cpu.min_freq.p1=2000000

# 覆盖：IO 预读更大（游戏档默认 192）
io.read_ahead_kb=256

# 新增：UE4 游戏线程绑核（档位没有的键直接追加）
thread.bind.RenderThread=6-7
thread.bind.RHIThread=6-7
thread.bind.GameThread=0-5
```

**覆盖规则**：per-app 配置中除 `mode`/`family` 外的键，按名匹配覆盖档位同名参数；档位没有的键直接追加。每应用可独立调优，不影响其他应用。

**支持的参数**（与档位配置格式一致）：
- `cpu.min_freq.p0` / `cpu.min_freq.p1`：各簇最低频（写前自动吸附到可用频率表）
- `gpu.min_freq`：GPU 频率下限（KGSL devfreq，抹掉深 dip 防帧时间尖峰；`0`=不干预；写前自动吸附）
- `cpuset.top_app_cpus` / `cpuset.background_cpus`：cpuset 核心分配
- `io.read_ahead_kb`：IO 预读缓冲区
- `mem.swappiness`：swap 倾向（越低越省电）
- `mem.dirty_ratio` / `mem.dirty_background_ratio`：脏页写回阈值（游戏档调大，突发写入吞吐）
- `mem.drop_caches`：`1`=切到该档时释放页缓存（游戏启动准备；一次性动作）
- `thread.bind.<线程名前缀>=<CPU列表>`：线程绑核
- `fps.steps.p0` / `fps.steps.p1`：**每游戏独立稳帧阶梯**（覆盖全局 settings.conf 阶梯；逗号分隔）
- `fps.enable` / `fps.sample_ms` / `fps.jank_threshold` / `fps.hold_ms` / `fps.startup_grace_ms`：稳帧参数（per-app 可覆盖全局）
- `fps.frame_probe` / `fps.target_fps` / `fps.low_ratio` / `frame.lib_path` / `frame.symbol_off`：帧监控探针参数；`frame.symbol_off` 仅作 ELF 自动解析失败时的兜底

### 帧监控探针（v1.2.0）

频率维度由本模块自己的稳帧阶梯管理，不再捆绑第三方 fas-rs。帧信号有两路：

- **adreno uprobe 快信号**：挂 vendor `libEGL_adreno.so` 的 `eglSwapBuffers`，每秒读 `uprobe_profile` 计数差分；与健康基线比，连续 2 次低于 80% 判骤降。计数含设备上其它 GL 消费者，**绝对值可能高于游戏内帧率**，面板在无 framestats 时会标注“（探针估）”，该数值仅作参考。
- **gfxinfo framestats 慢信号**：每 5 秒解析一次 `dumpsys gfxinfo <pkg> framestats` 的 `FrameCompleted` 时间戳，得到**应用真实 FPS**（不受其它 GL 消费者叠加影响），同时保留卡顿率。很多 Unity/Vulkan/SurfaceView 游戏的 framestats 为空，此时自动回退探针绝对帧率判据；`Total frames rendered < 30` 的启动残片直接忽略。
- **探针绝对值只在“最近 12 秒确实接近目标帧率”时使用**：和平精英大厅/加载只有 28-111fps，而 target=144，这些低帧不会触发升档；只有游戏真正跑起来后再掉帧才升档。
- **温控**：只用 CPU/GPU 温区（`cpu-*`/`cpuss-*`/`gpuss-*`）最高温；封顶/解除最小驻留 15 秒，避免温度尖峰导致频率来回跳。
- **启动宽限**：游戏启动/加载的前 60 秒不因 jank 或低帧升档；探针绝对值的宽限为 120 秒。明日方舟等游戏约 1-2 分钟才进对局，加载界面 10-30fps 不是真实掉帧。

升档判据为「卡顿率超标或真实 FPS 低或探针骤降」任一；降档要求卡顿率低且真实帧率健康。符号偏移在启动探针时从 ELF `.dynsym` 自动解析（`eglSwapBuffers`），OTA/换库后无需手改配置；解析失败才用 `frame.symbol_off` 兜底。探针启动 20 秒仍无计数会判定符号失效并回退纯卡顿率阶梯。

## 前台识别

以 `/proc` 扫描 + **top-app cgroup 判定**：真正的前台应用进程必在 top-app cpuset（`/proc/<pid>/cgroup` 含 `top-app`），取其中 oom_score_adj 最小者；无 top-app 候选才退回 adj 启发式。这避免了 `media.hwcodec` 等低 adj 服务进程被误判为前台（v1.0 实测踩坑）。

## 权限自愈、WebUI 兼容与防冻结（v1.0.4）

**权限自愈**：真机实测 KernelSU 安装后模块内文件可能全是 0644（安装器的 `set_perm` 在该环境不生效）。三层防护：① `customize.sh` 在 `set_perm` 之后用 `chmod` 直设并硬校验二进制；② `post-fs-data.sh` 与 `service.sh` 开机时把模块根目录 `.sh`、`script/*.sh`、守护二进制统一 `chmod 0755`；③ **WebUI 与脚本调用显式经 `/system/bin/sh` 执行**，不依赖执行位。

**WebUI file-fetch + 十二形态探测 + API 真相 dump**：不同 KSU 管理器版本的 `ksu.exec`/`ksu.spawn` 形态不一致。十二种候选，首位 **file-fetch**（`ksu.exec` 重定向输出到 `webroot/.fc_out`，`fetch()` 经 WebUI 本地 HTTP 服务器读取——唯一能绕过 fire-and-forget 的通道）。file-fetch 采用**唯一结束标记协议**（v1.0.15）：每次请求生成唯一 reqId，命令为 `sh <脚本> <参数> > .fc_out 2>&1; echo __FCDONE_<reqId>__ >> .fc_out`，轮询只有见到本请求的标记才认账（标记前内容即本命令输出）——陈旧内容（上个命令残留）与半写残片一律不被接受，根治页面切换时的竞态；请求经串行队列执行，同一时刻只有一个命令操作 `.fc_out`。其余形态：两参 / 回调 / 直连 / 单字符串 / spawn 直连 / spawn 经 sh / 纯脚本路径 / 全局回调 / 延迟读全局 / 对象参数 / sh -c 包装。bridge 启动时用 version 命令依次探测并缓存可用形态；「无输出」错误会带出**深度反射**：返回对象 typeof/ownKeys/原型方法名 + 20 个候选 getter 采样 + `ksu.exec.toString()` 源码 + 完整 `moduleInfo()` + `getPackageInfo`/`listPackages` + 全局命名空间全量 + `window.chrome` + `webkit.messageHandlers` + console 拦截 ——截图一张即可精确匹配真实签名与输出通道。

**版本号一致性**：二进制自报版本（`xindiaodud --version` / `--status` 的 version 字段）由 `tools/build.ps1` 在每次构建前从 `module.prop` 自动同步到 `src/main.cpp` 的 `kVersion`——升版本只需改 `module.prop`；`test/run_tests.sh` 有断言兜底（二进制版本与 module.prop 不一致则测试失败）。教训：v1.0.5~v1.0.14 曾六轮忘记同步，二进制陈旧版本号（1.0.4）覆盖了面板的正确版本显示。

**防冻结与自管理**（v1.0.3 真机卡死的根因是 ColorOS 后台冻结）：
- daemon 与监督者启动即设 `oom_score_adj = -1000`，防后台 freeze/防杀
- **监督者 mkdir 原子锁单实例**（`supervisor.lock`）：KSU 短跑两次 service.sh 时只有一个实例能活——双监督者互踩是开机日志爆刷的根因
- **开机大清底只跑一次**：监督者启动时遍历 /proc 杀光所有残留 xindiaodud（不限 daemon.pid）；放在启动段而非循环内，杜绝死亡螺旋（A 杀 B 的 daemon → B 重启又被 A 挡 → 循环）
- daemon **单实例三保险**：flock 独占（锁文件建不起来直接退出）+ daemon.pid 互斥（排除自身 pid）+ 大清底兜底
- daemon **内部看门狗线程**：主循环 60 秒无 tick 主动退出（退出码 42），监督者立即重启；退出用 `wait` 回收 zombie（diag 不会再误报「运行中」）
- **permissive 翻后读回确认**：写 enforce=0 后立即读回确认真的为 0 才重试写入，恢复后同样读回确认
- **WebUI API 真相诊断**：exec 四形态（两参/回调/直连/单字符串）探测全失败时，错误消息带上 `ksu.exec` 形参个数、对象键列表与各形态返回摘要——截图一张即可按真实签名适配

## 设备节点说明（一加平板 2 Pro / sm8750 / ColorOS 16）

- CPU 频率：`/sys/devices/system/cpu/cpufreq/policyN/scaling_min_freq`（p0=中核簇 cpu0-5，p1=大核簇 cpu6-7）。**频率吸附**：写入前读取 `scaling_available_frequencies`，把目标值取整到最近合法值——`min_freq` 向上取整（不低于目标，保性能），`max_freq` 封顶向下取整（绝不超过目标，保温控）。真机实测（一加平板 2 Pro）：policy0 合法频率为 384/556/748/960/1152/1363/1555/1785/1996/2227/2400/2745/2918/3072/3321/3532 MHz，policy6 为 1017/1209/1401/1689/1958/2246/2438/2649/2841/3072/3283/3513/3801/4089/4204/4320 MHz。**配置里的频率值必须是合法值**——非法值（如 1708800）会被内核钳位，写后回读不一致触发能力降级（v1.0.13 实测踩坑，v1.0.14 修正配置值 + 引擎吸附双保险）；每次吸附在日志留 INFO（`频率吸附: <节点> <原值> -> <新值>`）
- cpuset：该内核 cpuset 挂载带 `cpuset_v2_mode`，**文件名是 v2 命名的 `cpus`/`mems`**（不是 v1 的 `cpuset.cpus`），实际挂载点在 `/dev/cpuset`。模块按四候选顺序写入（`/dev/cpuset/<组>/cpuset.cpus` → `/dev/cpuset/<组>/cpus` → `/sys` 两个兜底），首个成功即用；**不做存在性读探测**（SELinux 会拦截 ksu 域读取 cgroup，探测会说谎）；**不会创建节点文件**（写入用无 O_CREAT 的 open，路径错误时明确 ENOENT 失败并落入下一候选）。ColorOS 的 SELinux 会拦截对 cgroup 的读写，模块采用**瞬时 permissive**：写前读 `/sys/fs/selinux/enforce` 原值 → 临时写 0 → 写入 → 恢复原值，全程毫秒级且一定恢复；每次触发在日志里留 INFO（如 `nd_write 经瞬时 permissive 成功: /dev/cpuset/background/cpus（enforce 1→0→1）`）。若仍失败，cpuset 能力显示「已降级」，其余干预不受影响
- 绑核：`sched_setaffinity` + `/proc/<tid>/status` 回读自证。线程名**前缀匹配**，UE4 的 `TaskGraph` 前缀覆盖全部变体（`TaskGraphNP 0`/`TaskGraphThread`/`TaskGraphWorker`——真机实测和平精英的线程名是 `TaskGraphNP 0`，v1.0.13 用 `TaskGraphThread` 前缀匹配不到它）
- 温控：只按 CPU/GPU 温区（`cpu-*`/`cpuss-*`/`gpuss-*`）最高温**封顶** `scaling_max_freq`，**绝不停温控服务**（停服务有失控风险）；电池/充电/PMIC 最高温仅在面板展示，不参与 CPU 封顶（v1.2.0 修正：旧版取全机最高温，真机 pmih 61℃ 会把 40℃ 的 CPU 误封顶）
- sched_ext：只读探测，不加载（内核版本耦合 + 与厂商 WALT 冲突，风险不可控）

## 从源码构建（可选）

```powershell
# Windows：一键 host 测试构建 + NDK 设备构建
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 打包
powershell -ExecutionPolicy Bypass -File tools\pack.ps1
```

host 测试在 WSL 里对模拟设备树跑 169 项用例；前端有 36 项 Node 冒烟测试。
改 `src/*.cpp` 后重新跑 `build.ps1` 即可，测试全过才允许打包。
