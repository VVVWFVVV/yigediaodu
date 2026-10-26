#!/system/bin/sh
# ============================================================
# 一个调度 · 安装脚本
# 同时兼容 Magisk / KernelSU / KernelSU-Next / SukiSU / APatch
# Magisk 由 META-INF 的 update-binary 调用，KernelSU 系直接执行本脚本
# ============================================================

SKIPUNZIP=0

# ---------------- 安装环境兼容层 ----------------
command -v ui_print >/dev/null 2>&1 || ui_print() { echo "$1"; }
command -v abort >/dev/null 2>&1 || abort() { ui_print "$1"; exit 1; }

command -v set_perm >/dev/null 2>&1 || set_perm() {
    chown "$1:$2" "$4" 2>/dev/null
    chmod "$3" "$4" 2>/dev/null
}

command -v set_perm_recursive >/dev/null 2>&1 || set_perm_recursive() {
    chown -R "$1:$2" "$5" 2>/dev/null
    chmod -R "$3" "$5" 2>/dev/null
}

if [ -z "$MODPATH" ]; then
    MODPATH="$(dirname "$0")"
fi
[ -f "$MODPATH/module.prop" ] || abort "! 模块文件不完整（缺少 module.prop）"

# ---------------- 架构校验 ----------------
ABI="$(getprop ro.product.cpu.abi)"
if [ "$ARCH" != "arm64" ] && [ "$ABI" != "arm64-v8a" ]; then
    abort "! 一个调度仅支持 arm64 设备，当前架构：$ARCH / $ABI"
fi

# ---------------- 净化换行符 ----------------
# Windows 环境打包极易带上 CRLF，会让所有 shell 脚本无法执行
find "$MODPATH" -type f -name "*.sh" 2>/dev/null | while read -r f; do
    sed -i 's/\r$//' "$f" 2>/dev/null
done

ui_print " "
ui_print "*******************************"
ui_print "       一个调度 · Xindiaodu"
ui_print "*******************************"
ui_print " 设备信息：$(getprop ro.product.marketname 2>/dev/null)$(getprop ro.product.model)"
ui_print " SoC     ：$(getprop ro.soc.model)$(getprop ro.board.platform)"
ui_print " 内核    ：$(uname -r)"
ui_print " 架构    ：$ABI"
ui_print " "

# ---------------- 根方案识别 ----------------
if [ -d /data/adb/magisk ]; then
    ROOT_SOLUTION="Magisk"
elif [ -d /data/adb/ksu ]; then
    ROOT_SOLUTION="KernelSU"
elif [ -d /data/adb/ap ]; then
    ROOT_SOLUTION="APatch"
else
    ROOT_SOLUTION="Unknown"
fi
ui_print " 根方案  ：$ROOT_SOLUTION"

# ---------------- 旧版模块检测（id 迁移，v1.0.16） ----------------
# 模块 id 由 xindiaodu 改为 WY925976742：新旧为两个独立模块，同时安装会
# 在开机阶段互相杀进程（运行时虽能收敛，但有 boot 互杀窗口与日志噪音）。
# 检测到旧版直接 abort，提示先卸载。
if [ -d /data/adb/modules/xindiaodu ]; then
    abort "! 检测到旧版模块（id=xindiaodu，名字「一个调度」）仍在安装列表。
请先在管理器卸载旧版，再刷入本版本（id=WY925976742，名字「一个调度」）。"
fi

# ---------------- 同类调度模块共存检测 ----------------
# 多个调度模块同时写 CPU/GPU 节点会互相覆盖：只提示，不改动对方
CONFLICT=""
for m in /data/adb/modules/*; do
    [ -d "$m" ] || continue
    b="$(basename "$m")"
    [ "$b" = "WY925976742" ] && continue
    case "$b" in
        *uperf*|*scene*|*fde*|*ktweak*|*yc_df*|*Littleyouran*|*perf_boost*|*sched*)
            CONFLICT="$CONFLICT $b" ;;
    esac
done
if [ -n "$CONFLICT" ]; then
    ui_print " "
    ui_print " ! 检测到其他调度模块：$CONFLICT"
    ui_print "   多个调度模块同时写入 CPU/GPU 节点会互相覆盖，"
    ui_print "   建议只保留一个，或在 WebUI 中关闭一个调度的对应能力。"
fi

# ---------------- 运行时目录 ----------------
XDD_HOME=/data/adb/WYWY925976742
mkdir -p "$XDD_HOME/log" "$XDD_HOME/state" "$XDD_HOME/baseline" "$XDD_HOME/tmp" 2>/dev/null
chmod 0700 "$XDD_HOME" 2>/dev/null

# ---------------- 守护二进制硬校验 ----------------
# v1.0 教训：安装器的权限规则把二进制留成 0644（无执行位），
# 运行时才暴露「守护二进制缺失」，用户毫无线索。这里安装期就硬校验：
# 缺失直接 abort；设权后仍不可执行则 chmod 重试，再失败 abort。
XDD_BIN="$MODPATH/libs/arm64-v8a/xindiaodud"
if [ ! -f "$XDD_BIN" ]; then
    abort "! 守护二进制缺失（libs/arm64-v8a/xindiaodud），请重新打包"
fi
set_perm 0 0 0755 "$XDD_BIN" 2>/dev/null
if [ ! -x "$XDD_BIN" ]; then
    chmod 0755 "$XDD_BIN" 2>/dev/null
fi
if [ ! -x "$XDD_BIN" ]; then
    abort "! 守护二进制不可执行（权限设置失败），请重新打包或反馈"
fi
ui_print " 守护进程：已就绪（arm64-v8a，可执行）"
ui_print " "

# ---------------- 初始化配置（幂等） ----------------
if [ -f "$MODPATH/script/setup.sh" ]; then
    sh "$MODPATH/script/setup.sh" "$MODPATH" 2>&1 | while IFS= read -r line; do
        ui_print "$line"
    done
fi

# ---------------- 权限 ----------------
set_perm_recursive 0 0 0755 0644 "$MODPATH"
set_perm_recursive 0 0 0755 0755 "$MODPATH/script"
for f in service.sh post-fs-data.sh action.sh uninstall.sh customize.sh; do
    [ -f "$MODPATH/$f" ] && set_perm 0 0 0755 "$MODPATH/$f"
done
# 真机教训（一加平板/KSU）：set_perm 在该环境可能不生效，安装后文件全是
# 0644。这里在 set_perm 之后再用 chmod 直设一遍做双保险，并硬校验二进制。
for f in "$MODPATH"/*.sh "$MODPATH"/script/*.sh "$XDD_BIN"; do
    [ -f "$f" ] && chmod 0755 "$f" 2>/dev/null
done
[ -x "$XDD_BIN" ] || abort "! 守护二进制执行位丢失（chmod 后仍不可执行）"

# WebUI：KernelSU / APatch 由内核侧静态服务器读取本目录
if [ -d "$MODPATH/webroot" ]; then
    set_perm_recursive 0 0 0755 0755 "$MODPATH/webroot"
    find "$MODPATH/webroot" -type f 2>/dev/null | while read -r f; do
        set_perm 0 0 0644 "$f"
    done
    if [ -f "$MODPATH/webroot/index.html" ]; then
        ui_print " WebUI   ：已就绪（模块页面点「打开」即可管理）"
    else
        ui_print " ! WebUI ：缺少 webroot/index.html，面板无法打开"
    fi
else
    ui_print " ! WebUI ：未找到 webroot 目录，仅命令行可用"
fi

[ -f "$MODPATH/META-INF/com/google/android/update-binary" ] && \
    set_perm 0 0 0755 "$MODPATH/META-INF/com/google/android/update-binary"

ui_print " 安装完成，重启后生效。"
ui_print " 重启后可在 KernelSU / APatch 的模块页面打开 WebUI 管理面板。"
ui_print " "
