# 一个调度

> KernelSU 日用省电与游戏稳帧双场景调度模块

![version](https://img.shields.io/badge/version-v1.2.2-blue)
![platform](https://img.shields.io/badge/platform-Android%2015%2B-green)
![kernel](https://img.shields.io/badge/kernel-6.6%2B-orange)
![root](https://img.shields.io/badge/root-KernelSU-red)

**一个调度**（id=`WY925976742`）是一款 Android 调度模块，通过智能识别前台应用自动切换调度档位，实现**日用省电**与**游戏稳帧**的双场景优化。

## 功能特性

### 调度干预
- **CPU 频率下限**：按档位抬高大小核最低频，写前自动吸附到可用频率表（防内核钳位）
- **帧监控探针**：捆绑 adreno uprobe 帧计数 + gfxinfo framestats 真实帧率校准——掉帧升档从快、降档双信号确认；符号偏移运行时从 ELF 自动解析，OTA 不失效
- **cpuset 核心分配**：前台全核、后台收紧（v1/v2 命名自适应）
- **线程绑核**：前缀匹配 + `sched_setaffinity` + 回读自证（UE4/Unity 全变体覆盖）
- **IO 预读优化**：游戏档大预读加速地图/资源加载
- **内存调优**：swap 倾向 + 脏页写回阈值
- **稳帧阶梯**：基于 gfxinfo 卡顿率的自动升降频（滞回防抖），**每游戏可独立配置**
- **温控封顶**：超温自动降频（滞回恢复），封顶值向下吸附到合法频率

### per-app 独立调优
- 每个应用可锁定档位（`mode=game` / `daily-plus` 等）
- 支持覆盖档位同名参数，每应用独立调优互不影响
- 内置 7 个应用配置（和平精英/原神/王者/穿越火线/明日方舟/哔哩哔哩×2）

### WebUI 面板（四标签）
- **总览**：实时状态（前台应用/档位/温度/频率/帧率/干预能力）
- **应用**：per-app 配置管理（查看/删除）
- **日志**：守护进程日志（自动滚动）
- **自检**：7 项系统探针（root/运行状态/前台识别/亮灭屏/守护/快照/体检）

### 可靠性
- **防冻结**：`oom_score_adj=-1000` 防 ColorOS 后台冻结
- **单实例**：flock + pid 双重互斥
- **看门狗**：60s 心跳检测，异常自动退出重启
- **基线快照**：所有干预可还原（`--restore`）
- **写后回读自证**：每次写入自动验证，不一致自动重试（3 次）
- **SELinux 瞬时 permissive**：写入被拦时自动翻转一次（读原值→写 0→重试→恢复）
- **频率吸附**：非法频率值自动取整到可用频率表（min 向上/max 向下）

## 支持设备

| 项目 | 要求 |
|---|---|
| 设备 | 一加平板 2 Pro（sm8750）已验证；其他 ARM64 设备理论兼容 |
| 系统 | Android 15+（ColorOS 16 已验证） |
| 内核 | 6.6+ |
| Root | KernelSU（已验证）；Magisk/APatch 理论兼容 |

## 安装

1. 前往 [Releases](../../releases) 下载最新 `WY925976742-v*.zip`
2. KernelSU 管理器 → 模块 → 从本地安装 → 选择 zip
3. 重启

> **注意**：如果之前安装过旧版「新调度」（id=`xindiaodu`），请先卸载旧版再安装本版本。

### 升级时如何对待旧配置

刷入新版时，安装脚本默认**保留旧配置**（`settings.conf`、`profiles/`、`perapp/`），并自动备份到 `/data/adb/WY925976742/config_backup/<时间>/`。

安装时可用音量键选择：

- **音量上**：保留旧配置（推荐；10 秒无操作默认）
- **音量下**：覆盖为新版默认配置

也可以非交互地显式确认覆盖：

```sh
adb shell su -c 'mkdir -p /data/adb/WY925976742/state && touch /data/adb/WY925976742/state/overwrite_config'
```

再刷入 zip，安装器检测到 `overwrite_config` 后使用新版默认配置，并在用完后删除该标记。

## 配置

配置文件位于模块目录 `config/`，行式 `key=value`，WebUI 修改即时生效：

```
config/
├── settings.conf           # 全局设置与能力开关
├── profiles/               # 调度档位
│   ├── daily.conf          #   日用（省电）
│   ├── daily-plus.conf     #   日用增强（轻抬频率抗卡顿）
│   ├── game.conf           #   游戏（稳帧+绑核+IO 优化）
│   ├── bg.conf             #   后台（收紧）
│   └── off.conf            #   关闭（不干预）
└── perapp/                 # 按应用配置
    ├── com.tencent.tmgp.pubgmhd.conf   # 和平精英
    ├── com.miHoYo.Yuanshen.conf         # 原神
    ├── com.tencent.tmgp.cf.conf         # 穿越火线
    └── ...                              # 更多
```

### per-app 配置示例

```ini
# config/perapp/com.tencent.tmgp.pubgmhd.conf
# 套用游戏档
mode=game

# 覆盖档位参数
cpu.min_freq.p0=1152000
cpu.min_freq.p1=1958400

# 线程绑核（前缀匹配）
thread.bind.RenderThread=6-7
thread.bind.RHIThread=6-7
thread.bind.TaskGraph=0-5

# 每游戏独立稳帧阶梯
fps.steps.p0=384000,1152000,1785600
fps.steps.p1=1017600,1401600,1958400
```

完整参数说明见 [docs/usage.md](docs/usage.md)。

## 构建

### 环境要求
- Windows + WSL2 Ubuntu（host 测试）
- Android NDK 30+（device 交叉编译）
- CMake + Ninja
- Node.js（前端冒烟测试）

### 一键构建

```powershell
# host 测试 + device 构建 + 全量测试
powershell -ExecutionPolicy Bypass -File tools\build.ps1

# 仅 host 测试
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -HostOnly

# 打包
powershell -ExecutionPolicy Bypass -File tools\pack.ps1
```

### 测试
- **host 测试**（169 项）：模拟设备树，验证配置解析/节点写入/绑核/频率吸附/稳帧阶梯/per-app 覆盖等
- **前端冒烟**（36 项）：Node.js mock DOM，验证 WebUI 四页面渲染与 API 交互

## 目录结构

```
├── module.prop                 # 模块元信息
├── service.sh                  # 开机启动（监督者）
├── post-fs-data.sh             # 早期初始化
├── customize.sh                # 安装脚本
├── action.sh                   # 管理器动作按钮
├── uninstall.sh                # 卸载清理
├── system.prop                 # 系统属性
├── META-INF/                   # 刷入引导
├── src/                        # C++17 守护进程
│   ├── main.cpp                #   入口（daemon/apply/restore/probe）
│   ├── engine.cpp              #   调度引擎（档位落地/频率吸附/绑核）
│   ├── monitor.cpp              #   前台识别（top-app cgroup）
│   ├── fps.cpp                  #   稳帧阶梯 + 温控封顶 + framestats 真实 FPS
│   ├── frame_probe.cpp          #   adreno uprobe 帧探针
│   ├── elf_symbol.cpp           #   ELF 动态符号解析（eglSwapBuffers 偏移）
│   ├── nodes.cpp               #   节点读写（回读自证/permissive 重试）
│   ├── config.cpp              #   配置解析
│   ├── shell.cpp               #   外部命令（dumpsys 等）
│   ├── state.cpp               #   运行时状态
│   ├── log.cpp                 #   日志
│   └── paths.cpp               #   路径前缀（host/device 统一）
├── script/                     # POSIX sh 骨架
│   ├── fcctl.sh                #   控制入口（JSON API）
│   ├── diag.sh                 #   体检工具
│   ├── setup.sh                #   初始化
│   └── pathinfo.sh             #   路径规范
├── webroot/                    # WebUI（零依赖 ES5）
│   ├── index.html
│   └── assets/                 #   bridge.js / api.js / app.js / pages.js
├── config/                     # 配置文件
├── test/                       # 测试
│   ├── run_tests.sh            #   host 测试（169 项）
│   ├── mock_tree.sh            #   模拟设备树
│   └── js_smoke.js             #   前端冒烟（36 项）
├── tools/                      # 构建/打包工具
└── docs/                       # 文档
```

## 致谢

- [KernelSU](https://github.com/tiann/KernelSU) — Root 框架
- 所有测试反馈的用户

---

**作者**：晚吟 | **版本**：v1.2.2
