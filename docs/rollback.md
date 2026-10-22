# 一个调度 · 回滚手册

所有路径都从轻到重，优先用前面的。

## 1. 临时关闭干预（不卸载）

- **面板**：顶栏总开关拨到关（立即还原全部节点，守护进程待命）
- **命令行**：
  ```sh
  sh /data/adb/modules/WY925976742/script/fcctl.sh enable 0
  ```

## 2. 暂停 / 恢复

```sh
sh /data/adb/modules/WY925976742/script/fcctl.sh pause
sh /data/adb/modules/WY925976742/script/fcctl.sh resume
```

## 3. 守护进程异常（面板无响应/数据不更新）

```sh
# 看状态
sh /data/adb/modules/WY925976742/script/fcctl.sh daemon status
# 重启守护（监督者也会自动拉起）
sh /data/adb/modules/WY925976742/script/fcctl.sh daemon restart
```

连续崩溃 3 次模块会**自动禁用**并还原节点（防开机循环），标记在
`/data/adb/WY925976742/state/disabled_crash`，删除该文件即可重新启用。

## 4. 节点疑似没还原

```sh
# 按基线快照手动还原
/data/adb/modules/WY925976742/libs/arm64-v8a/xindiaodud --restore
```

## 5. 卸载模块

KernelSU/Magisk 管理器里删除模块 → 重启。
卸载脚本会先停进程、按基线还原节点、清理运行时目录 `/data/adb/WY925976742`。

## 6. 管理器也打不开（最坏情况）

重启时**按住音量下键**进入安全模式（KernelSU 不会加载模块），或：

```sh
# 通过 recovery / 另一台设备 adb：
adb shell touch /data/adb/modules/WY925976742/disable
adb reboot
```

`disable` 文件存在时模块完全待命，重启后进管理器删除即可。

## 7. 节点被改坏的症状与对照

| 症状 | 原因 | 处理 |
|---|---|---|
| 卡顿/掉帧 | 频率被压太低 | 关总开关或卸载，重启即恢复硬件默认 |
| 发热异常 | 稳帧档位过高 | 设置里关 `fps.enable`，或降低 `fps.steps.*` |
| 后台杀应用变猛 | cpuset/内存干预 | 关 `feature.cpuset` / `feature.mem` |
| 无任何效果 | 能力徽标全「未开启」 | 自检页看探针输出；确认 `enabled=1` 且能力开关为 1 |

每次开机会清空基线快照，由本轮首次写入重建——还原值一定来自硬件默认值。
