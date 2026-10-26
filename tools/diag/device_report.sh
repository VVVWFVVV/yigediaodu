#!/system/bin/sh
# ============================================================
# 一个调度 · 真机基线采集（只读）
# 用法（PC）：
#   adb push tools/diag/device_report.sh /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/device_report.sh"
# 输出可直接贴回开发记录；不做任何写入，不改节点。
# ============================================================

MOD=/data/adb/modules/WY925976742
HOME=/data/adb/WY925976742

echo "===== device ====="
getprop ro.product.model
getprop ro.build.version.release
getprop ro.build.version.sdk
uname -r
echo "uptime: $(cat /proc/uptime 2>/dev/null)"

echo "===== module ====="
sed -n '1,8p' "$MOD/module.prop" 2>/dev/null
echo "daemon pid: $(pidof xindiaodud 2>/dev/null)"
echo "supervisor pid: $(cat "$HOME/supervisor.pid" 2>/dev/null)"

echo "===== live ====="
cat "$HOME/state/live" 2>/dev/null
echo "===== capability ====="
cat "$HOME/state/capability" 2>/dev/null
echo "===== current ====="
cat "$HOME/state/current" 2>/dev/null

echo "===== battery ====="
for f in current_now temp status voltage_now capacity; do
    v=$(cat "/sys/class/power_supply/battery/$f" 2>/dev/null)
    [ -n "$v" ] && echo "battery.$f=$v"
done

echo "===== thermal (top 15 by temp) ====="
for z in /sys/class/thermal/thermal_zone*; do
    t=$(cat "$z/type" 2>/dev/null)
    v=$(cat "$z/temp" 2>/dev/null)
    [ -n "$t" ] && [ -n "$v" ] && echo "$v $t"
done | sort -nr | head -15

echo "===== cpu freq ====="
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    echo "$(basename "$p") cur=$(cat "$p/scaling_cur_freq" 2>/dev/null) min=$(cat "$p/scaling_min_freq" 2>/dev/null) max=$(cat "$p/scaling_max_freq" 2>/dev/null)"
done

echo "===== daemon cpu ====="
pid=$(pidof xindiaodud 2>/dev/null)
if [ -n "$pid" ]; then
    top -b -n 1 -H -p "$pid" 2>/dev/null | head -8
fi

echo "===== recent daemon log ====="
tail -20 "$HOME/log/daemon.log" 2>/dev/null

echo "===== recent supervisor log ====="
tail -10 "$HOME/log/supervisor.log" 2>/dev/null
