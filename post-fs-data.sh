#!/system/bin/sh
# ============================================================
# 一个调度 · 早期初始化
# 此阶段系统服务尚未就绪，只做目录准备与残留清理，
# 绝不在此写入任何内核节点（由 service.sh 在 boot_completed 后处理）
# ============================================================

MODDIR="${0%/*}"
XDD_HOME=/data/adb/WY925976742

mkdir -p "$XDD_HOME/log" "$XDD_HOME/state" "$XDD_HOME/baseline" "$XDD_HOME/tmp" 2>/dev/null
chmod 0700 "$XDD_HOME" 2>/dev/null

# 权限开机自愈：KSU 安装后模块内文件可能全是 0644（v1.0.1 真机实测，
# customize.sh 的 set_perm 在这台设备没保住），把根目录 .sh、script/*.sh
# 与守护二进制统一 chmod 0755。幂等，每次开机执行一次。
XDD_BIN="$MODDIR/libs/arm64-v8a/xindiaodud"
XDD_SELFHEAL=0
for f in "$MODDIR"/*.sh "$MODDIR"/script/*.sh "$XDD_BIN"; do
    if [ -f "$f" ] && [ ! -x "$f" ]; then
        chmod 0755 "$f" 2>/dev/null && XDD_SELFHEAL=1
    fi
done
if [ "$XDD_SELFHEAL" = "1" ]; then
    echo "$(date '+%m-%d %H:%M:%S') [post-fs-data] 权限自愈：已恢复执行位" \
        >> "$XDD_HOME/log/supervisor.log" 2>/dev/null
fi

# 清理上次异常退出留下的状态，避免误判进程存活
# supervisor.lock（mkdir 原子锁）必须整目录清掉：陈旧锁 + pid 复用
# 会让 service.sh 误判「监督者已在运行」而整轮不启动（v1.0.6 复核）
rm -rf "$XDD_HOME/supervisor.lock" 2>/dev/null
rm -f "$XDD_HOME/supervisor.pid" "$XDD_HOME/daemon.pid" 2>/dev/null
rm -f "$XDD_HOME/state/reload.flag" 2>/dev/null
rm -f "$XDD_HOME/tmp"/* 2>/dev/null

# v1.2.0 移除捆绑的帧感知调度后，清理旧版留在本模块运行时目录的产物。
# 只动本模块自己的路径，不碰 /sdcard 或其它应用的 fas-rs 配置。
rm -f "$XDD_HOME/state/fas.pid" "$XDD_HOME/state/fas.retry_count" 2>/dev/null
rm -f "$XDD_HOME/config/fas_games.toml" 2>/dev/null

# 关键：内核节点每次开机都会复位成硬件默认值，上一次开机快照的"基线"
# 对新的一轮来说已经失效（甚至可能记录的是被本模块改写后的值）。
# 因此每次开机清空基线，由本轮第一次写入时重新快照，保证还原值一定正确。
rm -f "$XDD_HOME/baseline"/* 2>/dev/null

# 崩溃自禁标记按开机清零：上一轮连续崩溃不代表本轮也有问题，给全新开始；
# 本轮内若再连续崩溃，监督者会重新立标记并自动禁用
rm -f "$XDD_HOME/state/disabled_crash" "$XDD_HOME/state/disabled_crash.reason" 2>/dev/null
