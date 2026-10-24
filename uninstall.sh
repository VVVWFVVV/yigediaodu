#!/system/bin/sh
# ============================================================
# 一个调度 · 卸载
# 还原被本模块改写过的内核节点，然后清理运行时目录。
# 由 Magisk / KernelSU 在删除模块时执行。
# ============================================================

MODDIR="${0%/*}"
[ -f "$MODDIR/module.prop" ] || MODDIR="/data/adb/modules/WY925976742"
. "$MODDIR/script/pathinfo.sh"

# 先停守护进程与监督者，避免边还原边写入
if [ -f "$XDD_HOME/daemon.pid" ]; then
    PID=$(cat "$XDD_HOME/daemon.pid" 2>/dev/null)
    if [ -n "$PID" ] && [ -d "/proc/$PID" ]; then
        # 落停止标记：监督者据此判定为「按请求停止」，不计崩溃
        touch "$XDD_STATE/stopping" 2>/dev/null
        kill -TERM "$PID" 2>/dev/null
        sleep 1
        [ -d "/proc/$PID" ] && kill -9 "$PID" 2>/dev/null
    fi
fi
# 监督者 pid 在 supervisor.lock/pid（v1.0.6 mkdir 原子锁）；兼容旧路径
if [ -f "$XDD_HOME/supervisor.lock/pid" ]; then
    SPID=$(cat "$XDD_HOME/supervisor.lock/pid" 2>/dev/null)
elif [ -f "$XDD_HOME/supervisor.pid" ]; then
    SPID=$(cat "$XDD_HOME/supervisor.pid" 2>/dev/null)
else
    SPID=""
fi
if [ -n "$SPID" ] && [ -d "/proc/$SPID" ]; then
    kill -TERM "$SPID" 2>/dev/null
    sleep 1
    [ -d "/proc/$SPID" ] && kill -9 "$SPID" 2>/dev/null
fi

# 按基线还原节点（守护二进制内部完成：读 baseline/ 快照逐项写回）
if [ -x "$XDD_BIN" ]; then
    "$XDD_BIN" --restore >/dev/null 2>&1
    echo " 一个调度：内核节点已按基线还原"
else
    echo " 一个调度：守护二进制缺失，跳过节点还原（重启后内核节点自动恢复默认）"
fi

# fas-rs 兜底清理（v1.0.17）：守护进程退出时会杀它，这里是双保险
if [ -f "$XDD_STATE/fas.pid" ]; then
    FPID=$(cat "$XDD_STATE/fas.pid" 2>/dev/null)
    [ -n "$FPID" ] && kill -9 "$FPID" 2>/dev/null
fi
killall fas-rs 2>/dev/null
# 清理 fas-rs 用户配置（v1.0.19：daemon 双写到 /sdcard/Android/fas-rs/games.toml）
rm -rf /sdcard/Android/fas-rs /data/media/0/Android/fas-rs 2>/dev/null

rm -rf "$XDD_HOME" 2>/dev/null
echo " 一个调度：运行时目录已清理，卸载完成"
exit 0
