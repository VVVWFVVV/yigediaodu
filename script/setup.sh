#!/system/bin/sh
# ============================================================
# 一个调度 · 初始化（幂等）
# 安装时与守护启动时都可能被调用：确保运行时目录存在。
# 配置播种在后续阶段补充。
# ============================================================

MODDIR="${1:-${0%/*}}"
case "$MODDIR" in
    */script) MODDIR="${MODDIR%/script}" ;;
esac
[ -f "$MODDIR/module.prop" ] || MODDIR="/data/adb/modules/WY925976742"

XDD_HOME=/data/adb/WY925976742
mkdir -p "$XDD_HOME/log" "$XDD_HOME/state" "$XDD_HOME/baseline" "$XDD_HOME/tmp" 2>/dev/null
chmod 0700 "$XDD_HOME" 2>/dev/null
echo " 运行时目录就绪：$XDD_HOME"
exit 0
