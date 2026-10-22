#!/system/bin/sh
# ============================================================
# 一个调度 · 路径与运行时目录规范
# 被顶层 *.sh 与 script/*.sh source；约定：
#   XDD_MODDIR  模块目录
#   XDD_HOME    运行时根目录 /data/adb/WY925976742
#   XDD_LOG/STATE/BASELINE/TMP  子目录
#   XDD_BIN     守护二进制路径
#   XDD_CONFIG  配置目录
# ============================================================

if [ -z "$XDD_MODDIR" ]; then
    _pi="${0%/*}"
    case "$_pi" in
        */script) XDD_MODDIR="${_pi%/script}" ;;
        *) XDD_MODDIR="$_pi" ;;
    esac
    [ -f "$XDD_MODDIR/module.prop" ] || XDD_MODDIR="/data/adb/modules/WY925976742"
fi

XDD_HOME="${XDD_HOME:-/data/adb/WY925976742}"
XDD_LOG="$XDD_HOME/log"
XDD_STATE="$XDD_HOME/state"
XDD_BASELINE="$XDD_HOME/baseline"
XDD_TMP="$XDD_HOME/tmp"
XDD_BIN="$XDD_MODDIR/libs/arm64-v8a/xindiaodud"
XDD_CONFIG="$XDD_MODDIR/config"
