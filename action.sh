#!/system/bin/sh
# ============================================================
# 一个调度 · 管理器动作
# KernelSU / APatch 模块页点「动作」按钮时执行：只读体检。
# 完整管理请打开 WebUI。
# ============================================================

MODDIR="${0%/*}"
[ -f "$MODDIR/module.prop" ] || MODDIR="/data/adb/modules/WY925976742"

sh "$MODDIR/script/diag.sh"
exit 0
