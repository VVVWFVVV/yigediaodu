#!/usr/bin/env bash
# ============================================================
# 生成模拟设备树（sm8750 风格：2 大核 + 6 中核，两个 cpufreq policy）
# 用法: mock_tree.sh [目标目录]   默认 test/mock
# 仅用于 WSL host 测试，不进入模块打包产物
# ============================================================
set -eu

ROOT="${1:-$(cd "$(dirname "$0")" && pwd)/mock}"
rm -rf "$ROOT"
mkdir -p "$ROOT"

SYS="$ROOT/sys"
BIN="$ROOT/bin"
mkdir -p "$SYS/devices/system/cpu" "$SYS/class/thermal" "$BIN"

# ---------------- cpufreq ----------------
# policy0 = cpu0-5（中核簇 3.53GHz），policy6 = cpu6-7（大核簇 4.32GHz）
# 注：真机拓扑是 policy0 + policy6（非 policy1），mock 树与真机保持一致
for i in 0 1 2 3 4 5; do
    d="$SYS/devices/system/cpu/cpu$i/cpufreq"
    mkdir -p "$d"
    echo 1 > "$SYS/devices/system/cpu/cpu$i/online"
    echo "0-5"  > "$d/related_cpus"
    echo 384000 > "$d/cpuinfo_min_freq"
    echo 3532800 > "$d/cpuinfo_max_freq"
    echo 384000 > "$d/scaling_min_freq"
    echo 3532800 > "$d/scaling_max_freq"
    echo schedutil > "$d/scaling_governor"
    echo 1152000 > "$d/scaling_cur_freq"
    # 真机实测频率表（policy0, cpu0-5，单位 kHz）：384/556/748/960/1152/1363/1555/1785/1996/2227/2400/2745/2918/3072/3321/3532 MHz
    echo "384000 556800 748000 960000 1152000 1363200 1555200 1785600 1996800 2227200 2400000 2745600 2918400 3072000 3321600 3532800" > "$d/scaling_available_frequencies"
done
for i in 6 7; do
    d="$SYS/devices/system/cpu/cpu$i/cpufreq"
    mkdir -p "$d"
    echo 1 > "$SYS/devices/system/cpu/cpu$i/online"
    echo "6-7"  > "$d/related_cpus"
    echo 1017600 > "$d/cpuinfo_min_freq"
    echo 4320000 > "$d/cpuinfo_max_freq"
    echo 1017600 > "$d/scaling_min_freq"
    echo 4320000 > "$d/scaling_max_freq"
    echo schedutil > "$d/scaling_governor"
    echo 1958400 > "$d/scaling_cur_freq"
    # 真机实测频率表（policy6, cpu6-7，单位 kHz）：1017/1209/1401/1689/1958/2246/2438/2649/2841/3072/3283/3513/3801/4089/4204/4320 MHz
    echo "1017600 1209600 1401600 1689600 1958400 2246400 2438400 2649600 2841600 3072000 3283200 3513600 3801600 4089600 4204800 4320000" > "$d/scaling_available_frequencies"
done

# policy 目录（部分内核读取入口）
for p in 0 6; do
    pd="$SYS/devices/system/cpu/cpufreq/policy$p"
    mkdir -p "$pd"
    if [ "$p" = "0" ]; then
        echo "0-5" > "$pd/related_cpus"
        cp -r "$SYS/devices/system/cpu/cpu0/cpufreq/." "$pd/"
    else
        echo "6-7" > "$pd/related_cpus"
        cp -r "$SYS/devices/system/cpu/cpu6/cpufreq/." "$pd/"
    fi
done

# ---------------- 温区 ----------------
# temp 为毫度
mk_zone() { mkdir -p "$SYS/class/thermal/thermal_zone$1"; echo "$2" > "$SYS/class/thermal/thermal_zone$1/type"; echo "$3" > "$SYS/class/thermal/thermal_zone$1/temp"; }
mk_zone 0 cpu-0-0 42000
mk_zone 1 cpu-0-1 43000
mk_zone 2 cpu-1-0 41000
mk_zone 3 cpu-1-1 41500
mk_zone 4 gpu 45000
mk_zone 5 motherboard 38000
mk_zone 6 battery 35000

# ---------------- cpuset（Android 实际挂载点在 /dev/cpuset） ----------------
# 真机实测：该内核 cpuset 挂载带 cpuset_v2_mode，文件名是 v2 命名 cpus/mems
# （不是 v1 的 cpuset.cpus）。mock 与真机一致用 cpus；
# /dev 与 /sys 两处并存，验证 cpuset_candidates() 的「/dev 优先」选择；
# /sys 兜底与 v1 命名回退测试在 run_tests.sh 里进行
for grp in background system-background foreground top-app restricted; do
    mkdir -p "$ROOT/dev/cpuset/$grp" "$SYS/devices/system/cpu/cpuset/$grp"
    echo 0-7 > "$ROOT/dev/cpuset/$grp/cpus"
    echo 0-7 > "$SYS/devices/system/cpu/cpuset/$grp/cpus"
done

# ---------------- SELinux ----------------
mkdir -p "$SYS/fs/selinux"
echo 1 > "$SYS/fs/selinux/enforce"

# ---------------- /proc 模拟 ----------------
PROC="$ROOT/proc"
mkdir -p "$PROC"
echo "123456.78 987654.32" > "$PROC/uptime"
# /proc/stat：汇总行 + 8 个 CPU 行
echo "cpu  1000 200 3000 900000 500 0 100 0 0 0" > "$PROC/stat"
for i in 0 1 2 3 4 5 6 7; do
    echo "cpu$i 120 20 380 112500 60 0 12 0 0 0" >> "$PROC/stat"
done
echo "ctxt 987654321" >> "$PROC/stat"
echo "btime 1700000000" >> "$PROC/stat"
echo "processes 12345" >> "$PROC/stat"

# 模拟进程：前台游戏（oom_score_adj=0，top-app cgroup）、桌面（200，top-app）、
# 系统进程、媒体服务（adj 更低但不在 top-app——真实设备上 media.hwcodec 的误报回归）
mk_proc() {  # mk_proc <pid> <comm> <cmdline> <oom_score_adj> <cgroup组> [线程名...]
    local pid="$1" comm="$2" cmdline="$3" adj="$4" cg="${5:-top-app}"; shift 5 2>/dev/null || shift $#
    local tid="$((pid * 10))"
    mkdir -p "$PROC/$pid/task"
    echo "$comm" > "$PROC/$pid/comm"
    echo "$cmdline" > "$PROC/$pid/cmdline"
    echo "$adj" > "$PROC/$pid/oom_score_adj"
    echo "Name:	$comm" > "$PROC/$pid/status"
    echo "Uid:	10234	10234	10234	10234" >> "$PROC/$pid/status"
    # cgroup：cpuset 行决定 top-app 判定（真机上前台应用必在 top-app）
    {
        echo "4: cpuset:/$cg"
        echo "3: cpu:/$cg"
        echo "0::/acct uid 99995/pid $pid"
    } > "$PROC/$pid/cgroup"
    if [ "$#" -gt 0 ]; then
        for t in "$@"; do
            tid=$((tid + 1))
            mkdir -p "$PROC/$pid/task/$tid"
            echo "$t" > "$PROC/$pid/task/$tid/comm"
            # host 模拟绑核回路会写 task/<tid>/status（nd_write 不创建文件）
            echo "Name:	$t" > "$PROC/$pid/task/$tid/status"
        done
    fi
}
mk_proc 1234 sgame com.tencent.tmgp.sgame 0 top-app main RenderThread "UnityChoreograp" "Worker Thread" AsyncCompilePSO "TaskGraphNP 0" "RHIThread"
mk_proc 2345 launcher3 com.heytap.launcher 200 top-app main
mk_proc 999 system_server system_server 0 system-background main
mk_proc 1500 surfaceflinger surfaceflinger 0 system-background main "RenderEngine"
# 误报回归进程：adj 比前台游戏还低，但不在 top-app——旧逻辑会选它，新逻辑必须忽略
mk_proc 1600 media.hwcodec media.hwcodec -100 background

# ---------------- 块设备（IO 调优入口） ----------------
for dev in sda sdb; do
    mkdir -p "$SYS/block/$dev/queue"
    echo 128 > "$SYS/block/$dev/queue/read_ahead_kb"
    echo 128 > "$SYS/block/$dev/queue/nr_requests"
done

# ---------------- /proc/sys/vm ----------------
mkdir -p "$PROC/sys/vm"
echo 60 > "$PROC/sys/vm/swappiness"
# v1.0.16 新增节点（游戏档 dirty 调优 + 启动缓存释放）
echo 20 > "$PROC/sys/vm/dirty_ratio"
echo 10 > "$PROC/sys/vm/dirty_background_ratio"
echo 0 > "$PROC/sys/vm/drop_caches"   # 读回恒 0 的一次性动作节点

# ---------------- GPU devfreq（v1.0.16 A1：游戏 GPU 频率下限） ----------------
# 真机 KGSL 节点用 **Hz**（10 位数字）：1100000000=1100MHz ... 160000000=160MHz。
# v1.0.16 ADB 实测抓到：配置若按 kHz 写会被 snap 吸附到最低档（方向反）！
# 配置与 mock 一律用 Hz（与节点原生单位一致）。KGSL 节点无 scaling_ 前缀。
KGSL="$SYS/class/kgsl/kgsl-3d0/devfreq"
mkdir -p "$KGSL"
echo "1100000000 1050000000 967000000 900000000 832000000 734000000 660000000 607000000 525000000 443000000 389000000 342000000 222000000 160000000" > "$KGSL/available_frequencies"
echo 160000000 > "$KGSL/min_freq"
echo 1100000000 > "$KGSL/max_freq"
echo "msm-adreno-tz" > "$KGSL/governor"

# ---------------- dumpsys 桩 ----------------
# 测试通过 $XD_ROOT/state/fg 控制"前台应用"，$XD_ROOT/state/screen 控制亮灭屏
cat > "$BIN/dumpsys" <<'STUB'
#!/usr/bin/env bash
# 模拟 dumpsys：依据 $XD_ROOT/state 下的控制文件输出
# XD_MOCK_SLEEP 非空时先睡眠对应秒数（用于看门狗超时测试）
[ -n "${XD_MOCK_SLEEP:-}" ] && sleep "$XD_MOCK_SLEEP"
STATE="${XD_ROOT:-.}/state"
fg=$(cat "$STATE/fg" 2>/dev/null || echo com.heytap.launcher)
screen=$(cat "$STATE/screen" 2>/dev/null || echo on)
case "$1" in
    activity)
        case "$*" in
            *activities*)
                echo "  mResumedActivity: ActivityRecord{8f2e1a3 u0 $fg/.MainActivity t42}"
                ;;
        esac
        ;;
    power)
        if [ "$screen" = "off" ]; then echo "  mWakefulness=Asleep"; else echo "  mWakefulness=Awake"; fi
        ;;
    gfxinfo)
        cat <<'FRAMES'
** Graphics info for package 'com.tencent.tmgp.sgame' (pid 1234) **
Stats since: 1234567890ns
Total frames rendered: 600
Janky frames: 36 (6.00%)
50th percentile: 16667ms
90th percentile: 17000ms
95th percentile: 18000ms
99th percentile: 25000ms
Number Missed Vsync: 2
Number High input latency: 0
Number Slow UI thread: 3
Number Slow bitmap uploads: 1
Number Slow issue draw commands: 0
FRAMES
        ;;
esac
exit 0
STUB
chmod +x "$BIN/dumpsys"

# 测试控制文件与运行时目录骨架
mkdir -p "$ROOT/state" "$ROOT/data/adb/WY925976742/log" "$ROOT/data/adb/WY925976742/state" \
         "$ROOT/data/adb/WY925976742/baseline" "$ROOT/data/adb/WY925976742/tmp"
# /sdcard 存储（v1.0.21：daemon 启动时有界等待其就绪——mock 默认就绪；
# 「未就绪时等待、就绪后补写」的行为由 run_tests.sh 场景 5 显式移除后验证）
mkdir -p "$ROOT/sdcard/Android/fas-rs"
echo "com.tencent.tmgp.sgame" > "$ROOT/state/fg"
echo "on" > "$ROOT/state/screen"

echo "$ROOT"
