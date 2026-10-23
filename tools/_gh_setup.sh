#!/usr/bin/env bash
export PATH="/c/Program Files/GitHub CLI:$PATH"
cd /d/新调度 || exit 1

echo "=== git add + commit:"
git add -A
git status --short | head -25
git commit -m "v1.0.17: FAS 帧感知调度集成

- 捆绑 fas-rs（eBPF 版 v2.8.0，GPL-3.0）：build.ps1 构建时自动下载
- 配置桥：per-app fas.enable/fas.target_fps 自动生成 games.toml
- 分层不重叠：fas-rs 管频率(scaling_min_freq)，模块管绑核/cpuset/IO/内存/温控/GPU
- 让位条件是「当前应用托管」而非「fas-rs 在跑」（非 fas 应用走内置阶梯）
- 模式仲裁：温控→powersave，连续3次卡顿→performance，否则 balance
- 进程托管：fork/setsid/exec + pid 文件 + 监督自愈 + 大清底覆盖
- 频率基线快照/还原：fas 退出后 min_freq 不留高频耗电
- WebUI：总览帧感知徽标 + 应用页接管小字 + fcctl list fas 字段
- 修复：fps 升档加保持窗口滞回（防卡顿持续时几秒冲顶档）
- 测试：150 host（+14 fas A/B 对照与 toml 桥用例）+ 36 冒烟" 2>&1 | tail -3

echo ""
echo "=== push:"
git push 2>&1

echo ""
echo "=== 创建 Release v1.0.17:"
gh release create v1.0.17 \
    --title "v1.0.17" \
    --notes "## 一个调度 v1.0.17 — FAS 帧感知调度

### 新功能：FAS 帧感知稳帧
捆绑 fas-rs（eBPF 版 v2.8.0，GPL-3.0）——监视实际渲染帧、在帧 deadline 前预调频率，比 jank% 反应式阶梯更精准。

- **分层不重叠**：fas-rs 管频率维度（scaling_min_freq），本模块继续管绑核/cpuset/IO/内存/温控/GPU
- **每游戏独立**：per-app 配置 fas.enable + fas.target_fps，守护进程自动生成 games.toml
- **模式仲裁**：温控触发→powersave；连续 3 次采样卡顿超标→performance；否则 balance
- **智能回退**：内核不支持 eBPF 时自动回退内置稳帧阶梯
- **eBPF 版无 hook 注入**，无反作弊检测风险

### 预设
原神/穿越火线/王者 fas.enable=1（120fps）、明日方舟=1（60fps）、**和平精英默认 0**（保守，想试手动改 1）

### 其他改进
- 修复稳帧升档无滞回（卡顿持续时几秒内冲到顶档）
- host 模式不再尝试启动 fas-rs（防测试阻塞）

### 测试
150 项 host（含 fas A/B 对照：让位/接管标记/配置桥生成）+ 36 项前端冒烟

### 安装
1. 下载 WY925976742-v1.0.17.zip
2. KernelSU → 模块 → 从本地安装（旧版可直接覆盖更新）
3. 重启" \
    "dist/WY925976742-v1.0.17.zip" 2>&1

echo ""
echo "=== 确认:"
gh release list 2>&1 | head -3
