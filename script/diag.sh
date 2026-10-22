#!/system/bin/sh
# ============================================================
# 一个调度 · 只读体检
# 不改任何节点；输出设备/平台/守护/生效档/能力状态/温度/频率/日志。
# WebUI 自检页与 action.sh 共用本脚本。
# ============================================================

MODDIR="${0%/*}"
case "$MODDIR" in
    */script) MODDIR="${MODDIR%/script}" ;;
esac
[ -f "$MODDIR/module.prop" ] || MODDIR="/data/adb/modules/WY925976742"

XDD_HOME="${XDD_HOME:-/data/adb/WY925976742}"
XDD_BIN="$MODDIR/libs/arm64-v8a/xindiaodud"
XDD_CONFIG="$MODDIR/config"

VER=$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null | head -n 1)

echo "=============================================="
echo "  一个调度 ${VER:-v1.0} · 只读体检"
echo "=============================================="
echo " 设备    : $(getprop ro.product.marketname 2>/dev/null)$(getprop ro.product.model 2>/dev/null)"
echo " 平台    : $(getprop ro.soc.model 2>/dev/null)$(getprop ro.board.platform 2>/dev/null)"
echo " 内核    : $(uname -r 2>/dev/null)"
echo " 运行目录: $XDD_HOME"

# ---------------- 守护进程 ----------------
echo "----------------------------------------------"
DPID=$(cat "$XDD_HOME/daemon.pid" 2>/dev/null)
# 进程存活且非 zombie：/proc/<pid> 在 zombie 状态下仍存在，
# 会误报「运行中」（v1.0.5 真机：daemon 退出未被 wait 回收）
xd_alive() {
    [ -d "/proc/$1" ] || return 1
    ST=$(sed -n 's/^.*) \([A-Z]\) .*/\1/p' "/proc/$1/stat" 2>/dev/null)
    [ "$ST" != "Z" ]
}

if [ -n "$DPID" ] && xd_alive "$DPID"; then
    echo " 守护进程: 运行中 (pid $DPID)"
    [ -f "$XDD_HOME/state/heartbeat" ] && echo " 心跳    : $(date -r "$XDD_HOME/state/heartbeat" '+%H:%M:%S' 2>/dev/null)"
else
    [ -n "$DPID" ] && [ -d "/proc/$DPID" ] && echo " 守护进程: 已退出（zombie，pid $DPID 待回收）"
    [ -z "$DPID" ] || [ ! -d "/proc/$DPID" ] && echo " 守护进程: 未运行"
fi
[ -f "$XDD_HOME/state/disabled_crash" ] && echo " 自禁    : 连续崩溃已自动禁用（删除 state/disabled_crash 可恢复）"
[ -f "$XDD_HOME/state/pause" ] && echo " 暂停    : 已暂停"

# ---------------- 当前生效 ----------------
PKG=$(sed -n 's/^PKG=//p' "$XDD_HOME/state/current" 2>/dev/null | head -n 1)
PROF=$(sed -n 's/^PROFILE=//p' "$XDD_HOME/state/current" 2>/dev/null | head -n 1)
echo " 前台应用: ${PKG:-未识别}"
echo " 生效档位: ${PROF:-—}"

# ---------------- 能力状态 ----------------
if [ -f "$XDD_HOME/state/capability" ]; then
    echo " 能力状态:"
    while IFS='=' read -r k v; do
        [ -n "$k" ] && echo "   $k=$v"
    done < "$XDD_HOME/state/capability"
fi

# ---------------- 平台探测（二进制自检） ----------------
echo "----------------------------------------------"
if [ -x "$XDD_BIN" ]; then
    "$XDD_BIN" --selfcheck 2>&1 | while IFS= read -r line; do
        echo " $line"
    done
else
    echo " 守护二进制缺失: $XDD_BIN"
fi

# ---------------- sched_ext 只读探测 ----------------
echo "----------------------------------------------"
echo " sched_ext（只读探测，本模块不加载）:"
if [ -d /sys/kernel/sched_ext ]; then
    echo "  框架存在: $(cat /sys/kernel/sched_ext/enable 2>/dev/null)"
    OPS=$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)
    echo "  当前调度器: ${OPS:-（未挂载）}"
else
    echo "  框架不存在（正常，不影响本模块）"
fi

# ---------------- 配置摘要 ----------------
echo "----------------------------------------------"
echo " 档位配置:"
for f in "$XDD_CONFIG"/profiles/*.conf; do
    [ -f "$f" ] || continue
    echo "  $(basename "$f" .conf)"
done
echo " 已配应用: $(ls "$XDD_CONFIG"/perapp/*.conf 2>/dev/null | wc -l) 个"

# ---------------- 最近日志 ----------------
echo "----------------------------------------------"
if [ -f "$XDD_HOME/log/daemon.log" ]; then
    echo " 最近日志:"
    tail -n 10 "$XDD_HOME/log/daemon.log" 2>/dev/null | while IFS= read -r line; do
        echo "  $line"
    done
else
    echo " 最近日志:（暂无）"
fi
echo "=============================================="
exit 0
