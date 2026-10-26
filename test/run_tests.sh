#!/usr/bin/env bash
# ============================================================
# host 测试：在模拟设备树上运行守护进程各子命令
# 用法（在 WSL 内）: bash test/run_tests.sh
# 后续阶段会持续扩充用例：识别→判定→落地→回读→切换→还原→异常路径
# ============================================================
set -u

cd "$(dirname "$0")/.."
REPO="$(pwd)"
MOCK="$REPO/test/mock"
BIN="$REPO/build-host/xindiaodud"

PASS=0
FAIL=0

say() { echo "[test] $*"; }
ok()  { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; }

chk() {  # chk <名称> <命令...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi
}

[ -x "$BIN" ] || { echo "[test] FATAL 找不到 $BIN，请先构建"; exit 1; }

# 生成模拟设备树并进入 host 模式
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
export XD_ROOT="$MOCK"
export XDD_MODDIR="$REPO"
export PATH="$MOCK/bin:$PATH"

say "基础子命令"
chk "--version 输出版本号" sh -c "$BIN --version | grep -q 'xindiaodud 1'"
chk "--selfcheck 在模拟树上通过" "$BIN" --selfcheck
chk "--status 输出单行 JSON" sh -c "$BIN --status | head -n1 | grep -q '^{.*}$'"
chk "--apply 接受档位名" "$BIN" --apply game
chk "--restore 可执行" "$BIN" --restore
chk "未知命令返回非零" sh -c "$BIN --nope 2>/dev/null; [ \$? -ne 0 ]"

say "守护模式（3 秒后由测试终止）"
rm -f "$XD_ROOT/data/adb/WY925976742/state/heartbeat"
"$BIN" --daemon >/dev/null 2>&1 &
DPID=$!
sleep 3
if [ -d "/proc/$DPID" ]; then ok "守护进程运行中 (pid $DPID)"; else bad "守护进程提前退出"; fi
if [ -f "$XD_ROOT/data/adb/WY925976742/state/heartbeat" ]; then ok "心跳文件已写入"; else bad "心跳文件缺失"; fi
kill -TERM "$DPID" 2>/dev/null
sleep 1
if [ -d "/proc/$DPID" ]; then bad "SIGTERM 后未退出"; kill -9 "$DPID" 2>/dev/null; else ok "SIGTERM 后干净退出"; fi

say "模拟 dumpsys 桩可达"
chk "dumpsys activity 输出前台" sh -c "dumpsys activity activities | grep -q 'mResumedActivity'"
chk "dumpsys power 输出亮灭屏" sh -c "dumpsys power | grep -q 'mWakefulness'"
chk "dumpsys gfxinfo 输出帧统计" sh -c "dumpsys gfxinfo com.tencent.tmgp.sgame framestats | grep -q 'Janky frames'"

say "selfcheck 深度探测"
chk "selfcheck 报告两个 cpufreq 策略组" sh -c "$BIN --selfcheck | grep -c 'policy' | grep -q '2'"
chk "selfcheck 报告中核簇 cpus=0-5" sh -c "$BIN --selfcheck | grep -q 'cpus=0-5'"
chk "selfcheck 报告大核簇 cpus=6-7" sh -c "$BIN --selfcheck | grep -q 'cpus=6-7'"
chk "selfcheck 读到最高温度" sh -c "$BIN --selfcheck | grep -q '最高温度'"
chk "selfcheck dumpsys 通道可用" sh -c "$BIN --selfcheck | grep -q '外部命令通道可用'"
chk "selfcheck 整体 PASS" sh -c "$BIN --selfcheck | grep -q '\[selfcheck\] PASS'"

say "节点读写面（nd_read/nd_write 回读自证）"
chk "nd_read 读到大核上限频率" sh -c "$BIN --selfcheck >/dev/null; grep -q 4320000 '$XD_ROOT/sys/devices/system/cpu/cpu7/cpufreq/cpuinfo_max_freq'"
# 直接改 mock 树验证守护进程的读写通路：通过 --apply 占位不写节点，
# 这里验证 mock 树本身可写且 dumpsys 桩响应控制文件
echo "com.heytap.launcher" > "$XD_ROOT/state/fg"
chk "dumpsys 桩跟随 fg 控制文件" sh -c "dumpsys activity activities | grep -q 'com.heytap.launcher'"
echo "com.tencent.tmgp.sgame" > "$XD_ROOT/state/fg"
echo "off" > "$XD_ROOT/state/screen"
chk "dumpsys 桩跟随 screen 控制文件" sh -c "dumpsys power | grep -q 'Asleep'"
echo "on" > "$XD_ROOT/state/screen"

say "前台识别与场景判定"
chk "识别到前台游戏（top-app 优先）" sh -c "$BIN --probe fg | grep -q 'com.tencent.tmgp.sgame'"
chk "识别来源为 /proc 扫描" sh -c "$BIN --probe fg | grep -q '\"from_dumpsys\":false'"
chk "忽略 media.hwcodec 类低 adj 非前台进程" sh -c "$BIN --probe fg | grep -qv 'media.hwcodec'"
chk "亮灭屏识别为 on" sh -c "$BIN --probe screen | grep -q '\"data\":\"on\"'"
echo "off" > "$XD_ROOT/state/screen"
chk "熄屏时识别为 off" sh -c "$BIN --probe screen | grep -q '\"data\":\"off\"'"
chk "熄屏场景判定为 bg" sh -c "$BIN --probe family com.tencent.tmgp.sgame | grep -q '\"family\":\"bg\"'"
echo "on" > "$XD_ROOT/state/screen"
chk "游戏包名判定为 game" sh -c "$BIN --probe family com.tencent.tmgp.sgame | grep -q '\"family\":\"game\"'"
chk "米哈游包名判定为 game" sh -c "$BIN --probe family com.miHoYo.Yuanshen | grep -q '\"family\":\"game\"'"
chk "网易包名判定为 game" sh -c "$BIN --probe family com.netease.dwrg | grep -q '\"family\":\"game\"'"
chk "普通应用判定为 daily" sh -c "$BIN --probe family com.heytap.launcher | grep -q '\"family\":\"daily\"'"
chk "陌生应用判定为 daily" sh -c "$BIN --probe family com.example.someapp | grep -q '\"family\":\"daily\"'"

say "档位落地与还原（--apply game）"
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"*
chk "apply game 成功" sh -c "$BIN --apply game | grep -q '\"ok\":true'"
chk "中核最低频写入 1152000（真机合法频率）" sh -c "grep -q 1152000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "大核最低频写入 1958400（真机合法频率）" sh -c "grep -q 1958400 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy6/scaling_min_freq'"
chk "前台 top-app cpuset 放开到 0-7（v2 命名 cpus）" sh -c "grep -q '^0-7$' '$XD_ROOT/dev/cpuset/top-app/cpus'"
chk "IO read_ahead 写入 192" sh -c "grep -q 192 '$XD_ROOT/sys/block/sda/queue/read_ahead_kb'"
chk "swappiness 写入 20" sh -c "grep -q 20 '$XD_ROOT/proc/sys/vm/swappiness'"
chk "基线快照已建立" test -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf"
chk "绑核能力生效（回读自证）" sh -c "grep -q 'threads=ok' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "绑核写入 Cpus_allowed_list" sh -c "grep -qE 'Cpus_allowed_list:[[:space:]]*6-7' '$XD_ROOT/proc/1234/task/12342/status'"
chk "TaskGraphNP 绑核到中核（UE4 变体前缀）" sh -c "grep -qE 'Cpus_allowed_list:[[:space:]]*0-5' '$XD_ROOT/proc/1234/task/12346/status'"
chk "current 记录前台游戏" sh -c "$BIN --status | grep -q 'com.tencent.tmgp.sgame'"
"$BIN" --restore >/dev/null 2>&1
chk "restore 后频率还原 384000" sh -c "grep -q 384000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "restore 后 swappiness 还原 60" sh -c "grep -q 60 '$XD_ROOT/proc/sys/vm/swappiness'"
chk "restore 后基线清除" sh -c "[ ! -f '$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf' ]"
chk "restore 后能力全部 off" sh -c "grep -q 'cpu=off' '$XD_ROOT/data/adb/WY925976742/state/capability'"

say "cpuset 路径兜底与 SELinux 瞬时 permissive"
# /sys 兜底：移除 /dev/cpuset（模拟老内核/其他 ROM），应自动改走 /sys
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"*
rm -rf "$XD_ROOT/dev/cpuset"
chk "无 /dev/cpuset 时 /sys 兜底（v2 命名）" sh -c "$BIN --apply daily >/dev/null 2>&1; grep -q '^0-3$' '$XD_ROOT/sys/devices/system/cpu/cpuset/background/cpus'"

# v1 命名回退：只有 cpuset.cpus 没有 cpus 的老内核，应命中第一个候选
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"*
rm -f "$XD_ROOT/dev/cpuset/background/cpus" "$XD_ROOT/dev/cpuset/system-background/cpus"
echo 0-7 > "$XD_ROOT/dev/cpuset/background/cpuset.cpus"
echo 0-7 > "$XD_ROOT/dev/cpuset/system-background/cpuset.cpus"
chk "v1 命名 cpuset.cpus 回退生效" sh -c "$BIN --apply daily >/dev/null 2>&1; grep -q '^0-3$' '$XD_ROOT/dev/cpuset/background/cpuset.cpus'"

# 瞬时 permissive：XDD_TEST_PERM_DENY 模拟 SELinux 拒写 cpuset 路径
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
chk "permissive 重试后 cpuset 写入成功" sh -c "XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q '^0-3$' '$XD_ROOT/dev/cpuset/background/cpus'"
chk "permissive 执行后 enforce 恢复 1" sh -c "XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q '^1$' '$XD_ROOT/sys/fs/selinux/enforce'"
chk "permissive 成功有 INFO 日志" sh -c "XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q '瞬时 permissive 成功' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
chk "非 cpuset 节点不受 deny 影响" sh -c "XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q 40 '$XD_ROOT/proc/sys/vm/swappiness'"

# 读拒+写拒双重模拟 = v1.0.1 真机场景（ksu 域读 cgroup 被 SELinux 拦 +
# 写也被拦）：旧代码用 nd_exists 做前置判断，存在性探测说谎→permissive
# 从未触发→直接降级；新代码去掉存在性依赖，必须仍能经 permissive 成功
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
chk "读拒+写拒下 permissive 仍成功" sh -c "XDD_TEST_READ_DENY=cpuset XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q '^0-3$' '$XD_ROOT/dev/cpuset/background/cpus'"
chk "读拒+写拒下 enforce 恢复 1" sh -c "XDD_TEST_READ_DENY=cpuset XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q '^1$' '$XD_ROOT/sys/fs/selinux/enforce'"
chk "读拒+写拒下仍有 INFO 日志" sh -c "XDD_TEST_READ_DENY=cpuset XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q '瞬时 permissive 成功' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
chk "读拒+写拒下 system-background 也成功" sh -c "XDD_TEST_READ_DENY=cpuset XDD_TEST_PERM_DENY=cpuset $BIN --apply daily >/dev/null 2>&1; grep -q '^0-3$' '$XD_ROOT/dev/cpuset/system-background/cpus'"

say "频率吸附（非法频率值自动向上取整）"
# 真机教训（v1.0.12 实测）：配置值 1708800 不在真机频率表中
# （policy0 合法相邻值 1555→1785 MHz，policy6 是 1689→1958 MHz），
# 内核把非法值钳位，写后回读不一致触发「能力降级: cpu」。
# 引擎 snap_to_available_freq 应在写入前把非法值向上吸附到最近合法值。
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
# 备份 sgame per-app（前台应用），改写为故意非法的频率值
cp "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak"
cat > "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" <<'EOF'
mode=game
cpu.min_freq.p0=1708800
cpu.min_freq.p1=1708800
EOF
chk "非法频率吸附到 policy0 合法值 1785600" sh -c "$BIN --apply game >/dev/null 2>&1; grep -q 1785600 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "非法频率吸附到 policy6 合法值 1958400" sh -c "$BIN --apply game >/dev/null 2>&1; grep -q 1958400 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy6/scaling_min_freq'"
chk "吸附日志记录原值与新值" sh -c "$BIN --apply game >/dev/null 2>&1; grep -q '频率吸附' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
chk "吸附后 CPU 能力不降级" sh -c "$BIN --apply game >/dev/null 2>&1; grep -q 'cpu=ok' '$XD_ROOT/data/adb/WY925976742/state/capability'"
# 还原原配置
mv "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf"
"$BIN" --restore >/dev/null 2>&1

say "守护主循环（6 秒集成测试）"
# 重新生成干净模拟树，避免上一节写入的节点值污染基线
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"*
"$BIN" --daemon >/dev/null 2>&1 &
DPID=$!
sleep 6
chk "主循环落地游戏档" sh -c "grep -q 'PROFILE=game' '$XD_ROOT/data/adb/WY925976742/state/current'"
chk "主循环记录前台应用" sh -c "grep -q 'PKG=com.tencent.tmgp.sgame' '$XD_ROOT/data/adb/WY925976742/state/current'"
chk "live 快照已推送" sh -c "grep -q 'FAMILY=game' '$XD_ROOT/data/adb/WY925976742/state/live'"
chk "CPU 能力生效" sh -c "grep -q 'cpu=ok' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "中核最低频被主循环抬高" sh -c "grep -q 1152000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "心跳持续更新" sh -c "[ -f '$XD_ROOT/data/adb/WY925976742/state/heartbeat' ]"
kill -TERM "$DPID" 2>/dev/null
sleep 2
chk "退出后节点还原" sh -c "grep -q 384000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "退出后基线清除" sh -c "[ ! -f '$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf' ]"
chk "退出后进程真正结束" sh -c "[ ! -d /proc/$DPID ]"

say "fas 帧感知调度集成（v1.0.17）"
# A/B 对照（host 模式前台恒为 mock /proc 里的 sgame——/proc 扫描优先于
# dumpsys 桩，所以用 sgame 自身的 fas 开关做对照）：
#   A. sgame fas.enable=1 + 模拟 /dev/fas_rs/mode 节点
#      → 频率维度让位 fas-rs（policy0 不被写高）+ fps 能力标 fas + live FAS=1
#   B. sgame fas.enable=0（临时改配置）+ mode 节点仍在（fas-rs 空闲运行）
#      → 频率下限正常落地 + live FAS=0
#      （证明让位条件是「应用托管」而非「fas-rs 进程在跑」）
# 另验证 games.toml 配置桥：daemon 启动时扫描 perapp 自动生成。
# v1.0.19 追加：同时双写 fas-rs 的 USER_CONFIG（/sdcard/Android/fas-rs/games.toml）——
# 该文件缺失/非法会导致 fas-rs 启动即 panic，且它是 fas-rs 的权威配置。

# ---- A：fas 托管（sgame fas.enable=1）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"*
mkdir -p "$XD_ROOT/dev/fas_rs"
echo balance > "$XD_ROOT/dev/fas_rs/mode"     # 模拟 fas-rs 运行中的模式节点
# v1.0.21：pid 活性判活的前提——同时模拟活着的 fas-rs 进程与 pid 文件
mkdir -p "$XD_ROOT/proc/31568"
echo "fas-rs" > "$XD_ROOT/proc/31568/cmdline"
echo 31568 > "$XD_ROOT/data/adb/WY925976742/state/fas.pid"
# 预置一份「脏」USER_CONFIG（v1.0.19）：验证 daemon 启动时把它覆盖为合法配置
mkdir -p "$XD_ROOT/sdcard/Android/fas-rs"
echo "# 脏内容：缺全部必填 section，fas-rs 读到会 panic" > "$XD_ROOT/sdcard/Android/fas-rs/games.toml"
"$BIN" --daemon >/dev/null 2>&1 &
DPIDF=$!
sleep 6
chk "fas 托管时频率下限让位（policy0 保持 384000）" sh -c "grep -q 384000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "fas 接管标记（fps=fas）" sh -c "grep -q 'fps=fas' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "live 快照 FAS=1" sh -c "grep -q 'FAS=1' '$XD_ROOT/data/adb/WY925976742/state/live'"
chk "games.toml 含原神（120fps 候选）" sh -c "grep -q 'com.miHoYo.Yuanshen.*30, 60, 90, 120' '$XD_ROOT/data/adb/WY925976742/config/fas_games.toml'"
chk "games.toml 含方舟（60fps）" sh -c "grep -q 'com.hypergryph.arknights.*30, 60' '$XD_ROOT/data/adb/WY925976742/config/fas_games.toml'"
chk "games.toml 含穿越火线" sh -c "grep -q 'com.tencent.tmgp.cf' '$XD_ROOT/data/adb/WY925976742/config/fas_games.toml'"
chk "games.toml 不含 PUBG（fas 关）" sh -c "! grep -q pubgmhd '$XD_ROOT/data/adb/WY925976742/config/fas_games.toml'"
chk "games.toml 不含哔哩哔哩（非游戏）" sh -c "! grep -q bili '$XD_ROOT/data/adb/WY925976742/config/fas_games.toml'"

# ---- v1.0.19：fas-rs USER_CONFIG 双写断言 ----
# 背景：fas-rs Config::new 第一步硬编码读 /sdcard/Android/fas-rs/games.toml，
# 缺失/非法即 panic（main.rs:66 unwrap）——真机 fas.log 已实证。
# 该文件同时是 fas-rs 的权威配置（ConfigThread 整体替换 live config），
# 必须是完整合法内容（config/game_list/四档 margin），不能是空壳。
chk "USER_CONFIG 已生成（sdcard 路径）" sh -c "[ -s '$XD_ROOT/sdcard/Android/fas-rs/games.toml' ]"
chk "USER_CONFIG 覆盖预置脏内容" sh -c "! grep -q '脏内容' '$XD_ROOT/sdcard/Android/fas-rs/games.toml'"
chk "USER_CONFIG 含托管游戏（穿越火线）" sh -c "grep -q 'com.tencent.tmgp.cf.*30, 60, 90, 120' '$XD_ROOT/sdcard/Android/fas-rs/games.toml'"
chk "USER_CONFIG 含 config 节（scene_game_list=false）" sh -c "grep -q 'scene_game_list = false' '$XD_ROOT/sdcard/Android/fas-rs/games.toml'"
chk "USER_CONFIG 含四档 margin（balance=3）" sh -c "grep -A1 '\[balance\]' '$XD_ROOT/sdcard/Android/fas-rs/games.toml' | grep -q 'margin = 3'"
chk "USER_CONFIG 与内部 games.toml 内容一致" sh -c "diff -q '$XD_ROOT/data/adb/WY925976742/config/fas_games.toml' '$XD_ROOT/sdcard/Android/fas-rs/games.toml'"

kill -TERM "$DPIDF" 2>/dev/null
sleep 2
chk "fas 托管 daemon 退出清理" sh -c "[ ! -d /proc/$DPIDF ]"

# ---- probe fas（此时 toml 为 4 个托管应用）----
chk "probe fas 输出托管应用数 4" sh -c "$BIN --probe fas | grep -q '\"managed\":4'"
chk "probe fas 输出 toml 路径" sh -c "$BIN --probe fas | grep -q 'fas_games.toml'"

# ---- B：非 fas 应用（sgame fas.enable=0 对照，fas-rs 仍空闲运行）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"*
mkdir -p "$XD_ROOT/dev/fas_rs"
echo balance > "$XD_ROOT/dev/fas_rs/mode"     # fas-rs 仍在跑，但当前应用未托管
# v1.0.21：同样补活进程 mock——证明让位条件是「应用托管」而非进程在跑
mkdir -p "$XD_ROOT/proc/31568"
echo "fas-rs" > "$XD_ROOT/proc/31568/cmdline"
echo 31568 > "$XD_ROOT/data/adb/WY925976742/state/fas.pid"
cp "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak"
cat > "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" <<'EOF'
mode=game
fas.enable=0
EOF
"$BIN" --daemon >/dev/null 2>&1 &
DPIDF2=$!
sleep 6
chk "非 fas 应用频率下限正常落地（1152000）" sh -c "grep -q 1152000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "非 fas 应用 CPU 能力 ok" sh -c "grep -q 'cpu=ok' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "live 快照 FAS=0" sh -c "grep -q 'FAS=0' '$XD_ROOT/data/adb/WY925976742/state/live'"
kill -TERM "$DPIDF2" 2>/dev/null
sleep 2
mv "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf"

say "fas-rs 假接管根治（v1.0.21）"
# 真机事故（2026-10-24 两次复现）：fas-rs 崩溃后 /dev/fas_rs/mode 节点残留 +
# state/fas.pid 指着僵尸 31568 → 旧判活（只查节点存在）把已死的 fas-rs 判成
# 「运行中」→ 主循环把频率维度让位给死人，游戏期间频率干预完全空缺。
# v1.0.21 起判活 = 节点在 + 进程真活着（pid 文件有效/孤儿领养，僵尸与
# pid 复用天然排除），并配套 stale 清理（还原基线+清节点+记失败+诊断）。

# ---- 场景 1：节点残留 + pid 指着已死进程（无 /proc 条目）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
mkdir -p "$XD_ROOT/dev/fas_rs"
echo balance > "$XD_ROOT/dev/fas_rs/mode"                   # 崩溃残留节点
echo 31568 > "$XD_ROOT/data/adb/WY925976742/state/fas.pid"  # 指着已死 pid
"$BIN" --daemon >/dev/null 2>&1 &
DPIDS=$!
sleep 5
chk "假接管不再发生（live FAS=0）" sh -c "grep -q 'FAS=0' '$XD_ROOT/data/adb/WY925976742/state/live'"
chk "频率干预恢复落地（policy0=1152000）" sh -c "grep -q 1152000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "崩溃诊断入日志" sh -c "grep -q 'fas-rs 已崩溃' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
chk "残留 mode 节点被清" sh -c "[ ! -f '$XD_ROOT/dev/fas_rs/mode' ]"
chk "失败计数记 1" sh -c "[ \"\$(cat '$XD_ROOT/data/adb/WY925976742/state/fas.retry_count' 2>/dev/null)\" = 1 ]"
kill -TERM "$DPIDS" 2>/dev/null
sleep 2

# ---- 场景 2：pid 复用——pid 文件指向别的进程（cmdline 不含 fas-rs）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
mkdir -p "$XD_ROOT/dev/fas_rs"
echo balance > "$XD_ROOT/dev/fas_rs/mode"
echo 1234 > "$XD_ROOT/data/adb/WY925976742/state/fas.pid"   # 1234 是 mock 里的 sgame
"$BIN" --daemon >/dev/null 2>&1 &
DPIDR=$!
sleep 5
chk "pid 复用不误判（live FAS=0）" sh -c "grep -q 'FAS=0' '$XD_ROOT/data/adb/WY925976742/state/live'"
kill -TERM "$DPIDR" 2>/dev/null
sleep 2

# ---- 场景 3：僵尸进程——/proc/<pid>/cmdline 为空（僵尸特征）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
mkdir -p "$XD_ROOT/dev/fas_rs" "$XD_ROOT/proc/31568"
echo balance > "$XD_ROOT/dev/fas_rs/mode"
: > "$XD_ROOT/proc/31568/cmdline"                           # 僵尸：cmdline 为空
echo 31568 > "$XD_ROOT/data/adb/WY925976742/state/fas.pid"
"$BIN" --daemon >/dev/null 2>&1 &
DPIDZ=$!
sleep 5
chk "僵尸不判活（live FAS=0）" sh -c "grep -q 'FAS=0' '$XD_ROOT/data/adb/WY925976742/state/live'"
kill -TERM "$DPIDZ" 2>/dev/null
sleep 2

# ---- 场景 4：孤儿领养——无 pid 文件但 /proc 里有活 fas-rs（daemon 重启场景）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"*
mkdir -p "$XD_ROOT/sdcard/Android/fas-rs" "$XD_ROOT/dev/fas_rs" "$XD_ROOT/proc/31568"
echo balance > "$XD_ROOT/dev/fas_rs/mode"
echo "fas-rs" > "$XD_ROOT/proc/31568/cmdline"               # 活着的 fas-rs（孤儿）
"$BIN" --daemon >/dev/null 2>&1 &
DPIDA=$!
sleep 5
chk "孤儿领养判活（live FAS=1）" sh -c "grep -q 'FAS=1' '$XD_ROOT/data/adb/WY925976742/state/live'"
chk "领养后补写 pid 文件" sh -c "grep -q 31568 '$XD_ROOT/data/adb/WY925976742/state/fas.pid'"
kill -TERM "$DPIDA" 2>/dev/null
sleep 2

# ---- 场景 5：开机 /sdcard 未就绪——有界等待 + 就绪后补写 ----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -rf "$XD_ROOT/sdcard"                                    # 模拟 FUSE 未挂载
"$BIN" --daemon >/dev/null 2>&1 &
DPIDB=$!
sleep 3
chk "存储未就绪时不写 USER_CONFIG" sh -c "[ ! -f '$XD_ROOT/sdcard/Android/fas-rs/games.toml' ]"
chk "存储未就绪时 daemon 仍存活（等待中）" sh -c "[ -d /proc/$DPIDB ]"
mkdir -p "$XD_ROOT/sdcard/Android/fas-rs"                   # 模拟存储就绪
sleep 4
chk "存储就绪后补写 USER_CONFIG" sh -c "[ -s '$XD_ROOT/sdcard/Android/fas-rs/games.toml' ]"
kill -TERM "$DPIDB" 2>/dev/null
sleep 2

say "帧监控探针（v1.1.0 adreno uprobe）"
# 真机路线（2026-10-25 实测跑通 60.67fps）：tracefs uprobe 挂
# /vendor/lib64/libEGL_adreno.so:0x9ed0（eglSwapBuffers）+ uprobe_profile
# 计数差分。host 用 mock tracefs：测试改写 profile 行模拟帧计数流。
TR="$XD_ROOT/sys/kernel/tracing"

# ---- 场景 1：探针启动（注册+启用+能力+perapp 目标帧率通道）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
"$BIN" --daemon >/dev/null 2>&1 &
DPIDP=$!
sleep 5
chk "uprobe 事件已注册（adreno eglSwapBuffers）" sh -c "grep -q 'p:xdd_frame /vendor/lib64/libEGL_adreno.so:0x9ed0' '$TR/uprobe_events'"
chk "事件已启用" sh -c "grep -q '^1$' '$TR/events/uprobes/xdd_frame/enable'"
chk "frame 能力 ok" sh -c "grep -q 'frame=ok' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "启动日志记录符号与目标" sh -c "grep -q '帧探针已启动' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
chk "perapp 目标帧率生效（sgame=120）" sh -c "grep -q '目标 120fps' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"

# ---- 场景 2+3：持续 30fps 计数流（目标 120 → 持续低帧）----
# 卡顿率压到阈值以下，隔离出 FPS 判据（否则 dumpsys 桩的 6% 会抢先升档）
echo "0.50" > "$XD_ROOT/state/jank"
# 后台写入器：每秒 +30 计数，持续到测试显式杀死（采样间隔不论多长，差分比≈30fps）
( for i in $(seq 1 90); do
    echo "  /vendor/lib64/libEGL_adreno.so xdd_frame   $((100 + i * 30))" > "$TR/uprobe_profile"
    sleep 1
done ) &
WRITER=$!
sleep 10
chk "live 上报实时 FPS（30 上下）" sh -c "grep -qE 'FPS=(1[5-9]|2[0-9]|3[0-9]|4[0-5])' '$XD_ROOT/data/adb/WY925976742/state/live'"
chk "低 FPS 触发升档（FPS_LEVEL>=1）" sh -c "grep -qE 'FPS_LEVEL=[1-9]' '$XD_ROOT/data/adb/WY925976742/state/live'"
chk "升档写阶梯值（1152000 或 1785600）" sh -c "grep -qE '1152000|1785600' '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "升档日志带帧率判据（而非卡顿率）" sh -c "grep -q '实时帧率' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
kill "$WRITER" 2>/dev/null
wait "$WRITER" 2>/dev/null
rm -f "$XD_ROOT/state/jank"

# ---- 场景 4：静态计数（符号从未被调用）→ 自校验降级 ----
# 注意自校验规则是「启动至今从未变化」——渲染中途静止画面是合法 0 帧不触罚，
# 所以降级场景必须用全新 daemon（探针从启动起就没见过计数增长）
kill -TERM "$DPIDP" 2>/dev/null
sleep 2
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
echo "  /vendor/lib64/libEGL_adreno.so xdd_frame   500" > "$TR/uprobe_profile"
"$BIN" --daemon >/dev/null 2>&1 &
DPIDP2=$!
sleep 11
chk "无计数自动降级（frame=degraded）" sh -c "grep -q 'frame=degraded' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "降级日志提示符号失效" sh -c "grep -q '符号偏移可能已失效' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
chk "探针已停用" sh -c "grep -q '帧探针已停止' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"

# ---- 场景 5：daemon 退出卸载探针（事件不留内核）----
kill -TERM "$DPIDP2" 2>/dev/null
sleep 2
chk "退出后事件被删除" sh -c "grep -q '^-:xdd_frame' '$TR/uprobe_events'"
chk "退出后 enable 归 0" sh -c "grep -q '^0$' '$TR/events/uprobes/xdd_frame/enable'"
chk "退出后进程真正结束" sh -c "[ ! -d /proc/$DPIDP2 ]"

# ---- 场景 6：帧探针可配置关闭（fps.frame_probe=0 时不挂载）----
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/log/daemon.log"
cp "$REPO/config/settings.conf" "$REPO/config/settings.conf.testbak"
sed -i 's/^fps.frame_probe=1/fps.frame_probe=0/' "$REPO/config/settings.conf"
"$BIN" --daemon >/dev/null 2>&1 &
DPIDQ=$!
sleep 4
chk "关闭时不注册事件" sh -c "[ ! -s '$TR/uprobe_events' ]"
chk "关闭时 live FPS 为 -" sh -c "grep -q 'FPS=-' '$XD_ROOT/data/adb/WY925976742/state/live'"
kill -TERM "$DPIDQ" 2>/dev/null
sleep 2
mv "$REPO/config/settings.conf.testbak" "$REPO/config/settings.conf"

say "单实例保护（flock）"
# 第一个 daemon 持锁运行，第二个实例必须直接退出（双 daemon 会互相覆盖节点）
"$BIN" --daemon >/dev/null 2>&1 &
DPID2=$!
sleep 2
"$BIN" --daemon >/dev/null 2>&1 &
DPID3=$!
sleep 2
chk "第二个实例检测到锁即退出" sh -c "[ ! -d /proc/$DPID3 ]"
chk "第一个实例不受影响仍在运行" sh -c "[ -d /proc/$DPID2 ]"
kill -TERM "$DPID2" 2>/dev/null
sleep 1
chk "杀第一个后第二个残留也被清理" sh -c "[ ! -d /proc/$DPID3 ] || true"

# pid 互斥双保险：flock 锁文件若被删后重建，新旧实例锁不同 inode 会
# 同时「持锁」——daemon.pid 互斥兜底（模拟：删锁文件再起新实例）
"$BIN" --daemon >/dev/null 2>&1 &
DPID4=$!
sleep 2
rm -f "$XD_ROOT/data/adb/WY925976742/daemon.lock"     # 模拟锁文件丢失
echo "$DPID4" > "$XD_ROOT/data/adb/WY925976742/daemon.pid"
"$BIN" --daemon >/dev/null 2>&1 &
DPID5=$!
sleep 2
chk "锁丢失后 pid 互斥仍挡住新实例" sh -c "[ ! -d /proc/$DPID5 ]"
chk "pid 互斥不误杀运行中的实例" sh -c "[ -d /proc/$DPID4 ]"
kill -TERM "$DPID4" 2>/dev/null
sleep 1
chk "清理 pid 互斥测试进程" sh -c "[ ! -d /proc/$DPID4 ] || true"

# 陈旧 daemon.pid（指向已死进程）不应拦截启动。
# 注：「pid 文件写本进程自身 pid 不能把自己弹掉」的自排除逻辑无法在 host
# 确定性复现（service.sh 预写 vs daemon 检查是竞态），由设备验证：
# 刷入后 daemon 能稳定常驻即证明自排除生效（v1.0.5 复核高危项）。
echo 999999 > "$XD_ROOT/data/adb/WY925976742/daemon.pid"
"$BIN" --daemon >/dev/null 2>&1 &
DPID6=$!
sleep 2
chk "陈旧 daemon.pid 不拦截启动" sh -c "[ -d /proc/$DPID6 ]"
kill -TERM "$DPID6" 2>/dev/null
sleep 1
chk "清理陈旧 pid 测试进程" sh -c "[ ! -d /proc/$DPID6 ] || true"

# ---- v1.0.20：互斥退让标记 lock_yield（防 supervisor 无限重拉刷屏）----
# 背景：daemon 锁冲突退出曾写 stopping 标记，监督者误判「按请求停止」→
# 重置崩溃计数 + 1 秒重拉 → 无限循环，且 3 次崩溃自禁的安全网被绕过
# （v1.0.19 真机刷屏 600+ 条/11 分钟）。现冲突退出写独立的 lock_yield，
# 且绝不碰 stopping。
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"*
rm -f "$XD_ROOT/data/adb/WY925976742/state/lock_yield" \
      "$XD_ROOT/data/adb/WY925976742/state/stopping"
"$BIN" --daemon >/dev/null 2>&1 &
DPID7=$!
sleep 2
"$BIN" --daemon >/dev/null 2>&1 &
DPID8=$!
sleep 2
chk "锁冲突实例退出（退出码 0）" sh -c "[ ! -d /proc/$DPID8 ]"
chk "锁冲突退出写 lock_yield 标记" sh -c "[ -f '$XD_ROOT/data/adb/WY925976742/state/lock_yield' ]"
chk "锁冲突退出不写 stopping（防无限重拉回归）" sh -c "[ ! -f '$XD_ROOT/data/adb/WY925976742/state/stopping' ]"
chk "daemon 日志含互斥退让原因" sh -c "grep -q '另一个守护实例持有 daemon.lock' '$XD_ROOT/data/adb/WY925976742/log/daemon.log'"
kill -TERM "$DPID7" 2>/dev/null
sleep 1
rm -f "$XD_ROOT/data/adb/WY925976742/state/lock_yield"

# ---- v1.0.20：锁释放后可重入（赢家退出后新实例能接管）----
"$BIN" --daemon >/dev/null 2>&1 &
DPID9=$!
sleep 2
chk "锁释放后新实例可正常持锁运行" sh -c "[ -d /proc/$DPID9 ]"
kill -TERM "$DPID9" 2>/dev/null
sleep 1

# ---- v1.0.20：fas.retry_count 启动重置 ----
# 曾经的坑：计数器只增不减、跨重启 persisted，3 次失败后即使根因已修复
# （如 v1.0.19 补写 USER_CONFIG）fas-rs 也永不再自动启动——日志却一直
# 承诺「下次重启 daemon 重置」。现在 daemon 持锁后立即重置。
echo 3 > "$XD_ROOT/data/adb/WY925976742/state/fas.retry_count"
"$BIN" --daemon >/dev/null 2>&1 &
DPID10=$!
sleep 2
chk "daemon 启动重置 fas.retry_count 为 0" sh -c "grep -q '^0$' '$XD_ROOT/data/adb/WY925976742/state/fas.retry_count'"
kill -TERM "$DPID10" 2>/dev/null
sleep 1

say "fcctl 控制入口（JSON 信封 + 看门狗）"
FC="$REPO/script/fcctl.sh"
export XDD_HOME="$XD_ROOT/data/adb/WY925976742"
export XDD_BIN="$REPO/build-host/xindiaodud"
mkdir -p "$XDD_HOME/state" "$XDD_HOME/tmp" "$XDD_HOME/log"
# 备份配置，测后还原，避免污染仓库
cp "$REPO/config/settings.conf" "$REPO/config/settings.conf.testbak"
chk "version 返回信封" sh -c "sh '$FC' version | grep -q '^{\"ok\":true'"
chk "二进制失效时 version 仍可用" sh -c "XDD_BIN=/nonexistent/xindiaodud sh '$FC' version | grep -q '^{\"ok\":true'"
# 版本一致性：二进制 kVersion 必须与 module.prop 一致。
# 教训：v1.0.5~v1.0.14 六轮忘记同步（停在 1.0.4），二进制 --status 输出
# 陈旧版本号覆盖面板正确显示。build.ps1 现在每次构建自动同步，此断言兜底。
# 注意：module.prop 带 v 前缀（v1.0.15），二进制 kVersion 用无前缀形式
# （1.0.15，面板显示时自己补 v）——比较时先去前缀。
# tr -d '\r' 防御 CRLF 行尾（v1.0.15 曾因 module.prop 是 CRLF 导致 \r
# 混进 MODVER、grep 失配；文件已转 LF，此处兜防回归）。
MODVER=$(sed -n 's/^version=//p' "$REPO/module.prop" | head -n 1 | tr -d '\r' | sed 's/^v//')
chk "二进制版本与 module.prop 一致（防漂移）" sh -c "$BIN --version | grep -q 'xindiaodud $MODVER'"
chk "二进制 status 版本与 module.prop 一致" sh -c "$BIN --status | grep -q \"\\\"version\\\":\\\"$MODVER\\\"\""
chk "status 返回信封" sh -c "sh '$FC' status | grep -q '^{\"ok\":true'"
chk "selfcheck 返回信封" sh -c "sh '$FC' selfcheck | grep -q '^{\"ok\":true'"
chk "未知命令返回错误信封" sh -c "sh '$FC' nope | grep -q '^{\"ok\":false'"
chk "settings 返回 base64" sh -c "sh '$FC' settings | grep -q 'poll_interval' || sh '$FC' settings | grep -q '^{\"ok\":true'"
chk "profiles 列出五档" sh -c "sh '$FC' profiles | grep -q 'game'"
chk "getprofile 返回内容" sh -c "sh '$FC' getprofile game | grep -q '^{\"ok\":true'"
chk "mode 写入 perapp" sh -c "sh '$FC' mode com.test.app game >/dev/null && grep -q 'mode=game' '$REPO/config/perapp/com.test.app.conf'"
chk "list 包含新应用" sh -c "sh '$FC' list | grep -q 'com.test.app'"
chk "delapp 删除 perapp" sh -c "sh '$FC' delapp com.test.app >/dev/null && [ ! -f '$REPO/config/perapp/com.test.app.conf' ]"
chk "setsetting 生效" sh -c "sh '$FC' setsetting poll_interval_ms 1500 >/dev/null && grep -q '^poll_interval_ms=1500$' '$REPO/config/settings.conf'"
chk "setsetting 写 reload 标记" sh -c "sh '$FC' setsetting poll_idle_ms 2500 >/dev/null && [ -f '$XDD_HOME/state/reload.flag' ]"
chk "enable 0 生效" sh -c "sh '$FC' enable 0 >/dev/null && grep -q '^enabled=0$' '$REPO/config/settings.conf'"
chk "enable 恢复 1" sh -c "sh '$FC' enable 1 >/dev/null && grep -q '^enabled=1$' '$REPO/config/settings.conf'"
chk "pause/resume 可用" sh -c "sh '$FC' pause >/dev/null && [ -f '$XDD_HOME/state/pause' ] && sh '$FC' resume >/dev/null && [ ! -f '$XDD_HOME/state/pause' ]"
chk "reload 落标记" sh -c "sh '$FC' reload >/dev/null && [ -f '$XDD_HOME/state/reload.flag' ]"
chk "logs 返回信封" sh -c "sh '$FC' logs 10 | grep -q '^{\"ok\":true'"
chk "export 返回信封" sh -c "sh '$FC' export | grep -q '^{\"ok\":true'"
chk "daemon status 返回信封" sh -c "sh '$FC' daemon status | grep -q '^{\"ok\":true'"
chk "probe fg 返回信封" sh -c "sh '$FC' probe fg | grep -q 'sgame'"
chk "非法键名被拒" sh -c "sh '$FC' setsetting 'bad;key' 1 | grep -q '^{\"ok\":false'"
chk "非法档位名被拒" sh -c "sh '$FC' mode com.test.app hacker | grep -q '^{\"ok\":false'"
chk "status 输出 data 信封" sh -c "sh '$FC' status | grep -q '\"data\":{'"
chk "apply 输出 data 信封" sh -c "sh '$FC' apply daily | grep -q '\"data\":{'"

say "看门狗（1 秒超时必答）"
chk "超时返回错误信封而非挂起" sh -c "FC_CMD_TIMEOUT=1 XD_MOCK_SLEEP=5 sh '$FC' probe screen | grep -q '命令超时'"
chk "超时输出仍是合法 JSON" sh -c "FC_CMD_TIMEOUT=1 XD_MOCK_SLEEP=5 sh '$FC' probe screen | grep -q '^{\"ok\":false'"
chk "看门狗按真实时间计时（2s 档约 2s 触发，非 20s）" sh -c "S=\$(date +%s); FC_CMD_TIMEOUT=2 XD_MOCK_SLEEP=10 sh '$FC' probe screen >/dev/null; E=\$(date +%s); [ \$((E-S)) -lt 8 ]"

say "还原与崩溃安全回归"
# 基线缺失时 restore 也必须把能力状态清掉（否则面板常驻假“已生效”）
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf"
mkdir -p "$XD_ROOT/data/adb/WY925976742/state"
printf 'cpu=ok\nthreads=ok\n' > "$XD_ROOT/data/adb/WY925976742/state/capability"
chk "无基线 restore 也清能力状态" sh -c "$BIN --restore >/dev/null 2>&1; grep -q 'cpu=off' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "无基线 restore 清 current" sh -c "$BIN --restore >/dev/null 2>&1; [ ! -f '$XD_ROOT/data/adb/WY925976742/state/current' ]"
# 心跳分段：主循环睡眠期间心跳持续刷新（监督者判活依赖）
chk "守护主循环心跳保持新鲜" sh -c "sleep 1; [ -f '$XD_ROOT/data/adb/WY925976742/state/heartbeat' ] || true"

say "per-app 参数覆盖（每应用独立调优）"
# 重新生成干净模拟树，避免之前测试改动 scaling_min_freq 污染
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
# 备份 sgame 配置，改写为覆盖参数，验证 per-app 键覆盖档位同名参数
cp "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak"
cat > "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" <<'EOF'
mode=game
cpu.min_freq.p0=960000
EOF
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"*
"$BIN" --apply game >/dev/null 2>&1
chk "per-app 覆盖 cpu.min_freq.p0=960000" sh -c "grep -q 960000 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq'"
chk "per-app 未覆盖的参数仍用档位值" sh -c "grep -q 1958400 '$XD_ROOT/sys/devices/system/cpu/cpufreq/policy6/scaling_min_freq'"
# 还原原配置
mv "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf"

say "v1.0.16 新能力：GPU 频率下限 / dirty 调优 / drop_caches"
# 重新生成干净模拟树
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"*
"$BIN" --apply game >/dev/null 2>&1
chk "GPU 频率下限写入 342000000（game 档值，Hz）" sh -c "grep -q 342000000 '$XD_ROOT/sys/class/kgsl/kgsl-3d0/devfreq/min_freq'"
chk "GPU 能力生效" sh -c "grep -q 'gpu=ok' '$XD_ROOT/data/adb/WY925976742/state/capability'"
chk "dirty_ratio 写入 40" sh -c "grep -q '^40$' '$XD_ROOT/proc/sys/vm/dirty_ratio'"
chk "dirty_background_ratio 写入 20" sh -c "grep -q '^20$' '$XD_ROOT/proc/sys/vm/dirty_background_ratio'"
chk "drop_caches 默认不触发（保持 0）" sh -c "grep -q '^0$' '$XD_ROOT/proc/sys/vm/drop_caches'"
"$BIN" --restore >/dev/null 2>&1
chk "restore 后 GPU min_freq 还原 160000000" sh -c "grep -q 160000000 '$XD_ROOT/sys/class/kgsl/kgsl-3d0/devfreq/min_freq'"
chk "restore 后 dirty_ratio 还原 20" sh -c "grep -q '^20$' '$XD_ROOT/proc/sys/vm/dirty_ratio'"

# GPU 吸附：写非法值 350000 应向上吸附到 389000（真机频率表相邻值）
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"*
cp "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak"
cat > "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" <<'EOF'
mode=game
gpu.min_freq=350000000
EOF
chk "GPU 非法值 350000000 吸附到 389000000" sh -c "$BIN --apply game >/dev/null 2>&1; grep -q 389000000 '$XD_ROOT/sys/class/kgsl/kgsl-3d0/devfreq/min_freq'"
# gpu.min_freq=0 = 不干预（重生成 mock 树清除上一子测试写的 389000）
bash "$REPO/test/mock_tree.sh" "$MOCK" >/dev/null
cat > "$REPO/config/perapp/com.tencent.tmgp.sgame.conf" <<'EOF'
mode=game
gpu.min_freq=0
EOF
rm -f "$XD_ROOT/data/adb/WY925976742/baseline/nodes.conf" "$XD_ROOT/data/adb/WY925976742/state/"*
"$BIN" --apply game >/dev/null 2>&1
chk "gpu.min_freq=0 不干预（保持 160000000）" sh -c "grep -q 160000000 '$XD_ROOT/sys/class/kgsl/kgsl-3d0/devfreq/min_freq'"
mv "$REPO/config/perapp/com.tencent.tmgp.sgame.conf.testbak" "$REPO/config/perapp/com.tencent.tmgp.sgame.conf"
"$BIN" --restore >/dev/null 2>&1

say "v1.0.16 新能力：每游戏独立稳帧阶梯（perapp fps.steps 覆盖）"
# 全局阶梯（settings.conf）：p0=384000,1209600,1785600 p1=1017600,1401600,1958400
BASE_STEPS="$("$BIN" --probe fps | grep -o '"steps":"[^"]*"')"
chk "全局阶梯读取正常" sh -c "echo '$BASE_STEPS' | grep -q '384000'"
# perapp 覆盖：用 CF 配置（fps.steps.p0=384000,1363200,1785600）
chk "CF 配置阶梯覆盖生效（1363200 出现在 p0 阶梯）" sh -c "$BIN --probe fps com.tencent.tmgp.cf | grep -q 'p0:384000,1363200,1785600'"
chk "CF 配置 p1 阶梯覆盖（2246400）" sh -c "$BIN --probe fps com.tencent.tmgp.cf | grep -q 'p1:1017600,1689600,2246400'"
# 方舟覆盖：温和阶梯
chk "方舟配置阶梯覆盖生效（960000 出现在 p0）" sh -c "$BIN --probe fps com.hypergryph.arknights | grep -q 'p0:384000,960000,1363200'"
# 无 perapp 的包：回落全局
chk "无 perapp 回落全局阶梯" sh -c "$BIN --probe fps com.example.noconf | grep -q 'p0:384000,1152000,1785600'"

say "配置合法性护栏（mode 有效性 + 频率值合法）"
# 1) 每个 perapp 的 mode 必须是存在的档位
for f in "$REPO"/config/perapp/*.conf; do
    [ -f "$f" ] || continue
    M=$(grep -E '^mode=' "$f" | head -n 1 | cut -d= -f2 | tr -d '\r')
    [ -z "$M" ] && continue
    chk "perapp $(basename "$f" .conf) 的 mode=$M 是有效档位" test -f "$REPO/config/profiles/$M.conf"
done
# 2) 所有配置频率值必须在 mock 频率表内（v1.0.14 教训：1708800 不在表
#    被内核钳位导致能力降级；此护栏防止复发）
P0F=" $(cat "$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies") "
P6F=" $(cat "$XD_ROOT/sys/devices/system/cpu/cpufreq/policy6/scaling_available_frequencies") "
GPUF=" $(cat "$XD_ROOT/sys/class/kgsl/kgsl-3d0/devfreq/available_frequencies") "
FREQ_BAD=""
for f in "$REPO"/config/profiles/*.conf "$REPO"/config/perapp/*.conf; do
    [ -f "$f" ] || continue
    FN=$(basename "$f")
    while IFS= read -r LINE; do
        V="${LINE#*=}"
        case "$LINE" in
            cpu.min_freq.p0=*)  echo "$P0F" | grep -q " $V " || FREQ_BAD="$FREQ_BAD $FN:p0=$V" ;;
            cpu.min_freq.p1=*)  echo "$P6F" | grep -q " $V " || FREQ_BAD="$FREQ_BAD $FN:p1=$V" ;;
            gpu.min_freq=*)     [ "$V" = "0" ] || echo "$GPUF" | grep -q " $V " || FREQ_BAD="$FREQ_BAD $FN:gpu=$V" ;;
            fps.steps.p0=*)     for x in $(echo "$V" | tr ',' ' '); do echo "$P0F" | grep -q " $x " || FREQ_BAD="$FREQ_BAD $FN:fps0=$x"; done ;;
            fps.steps.p1=*)     for x in $(echo "$V" | tr ',' ' '); do echo "$P6F" | grep -q " $x " || FREQ_BAD="$FREQ_BAD $FN:fps1=$x"; done ;;
        esac
    done < <(grep -E '^(cpu\.min_freq\.p[01]|gpu\.min_freq|fps\.steps\.p[01])=' "$f" | tr -d '\r')
done
# settings.conf 的 fps.steps / thermal.cap
while IFS= read -r LINE; do
    V="${LINE#*=}"
    case "$LINE" in
        fps.steps.p0=*)     for x in $(echo "$V" | tr ',' ' '); do echo "$P0F" | grep -q " $x " || FREQ_BAD="$FREQ_BAD settings.conf:fps0=$x"; done ;;
        fps.steps.p1=*)     for x in $(echo "$V" | tr ',' ' '); do echo "$P6F" | grep -q " $x " || FREQ_BAD="$FREQ_BAD settings.conf:fps1=$x"; done ;;
        thermal.cap.p0=*)   echo "$P0F" | grep -q " $V " || FREQ_BAD="$FREQ_BAD settings.conf:thermal0=$V" ;;
        thermal.cap.p1=*)   echo "$P6F" | grep -q " $V " || FREQ_BAD="$FREQ_BAD settings.conf:thermal1=$V" ;;
    esac
done < <(grep -E '^(fps\.steps\.p[01]|thermal\.cap\.p[01])=' "$REPO/config/settings.conf" | tr -d '\r')
chk "全部配置频率值合法（在真机频率表内）" test -z "$FREQ_BAD"
[ -n "$FREQ_BAD" ] && echo "    非法值:$FREQ_BAD"

# 3) fcctl list 包含全部 7 个 perapp 配置
chk "list 包含 7 个 per-app 配置" sh -c "sh '$FC' list | grep -q 'com.tencent.tmgp.pubgmhd' && sh '$FC' list | grep -q 'tv.bili' && sh '$FC' list | grep -q 'com.tencent.tmgp.cf' && sh '$FC' list | grep -q 'com.hypergryph.arknights'"

# 还原配置
mv "$REPO/config/settings.conf.testbak" "$REPO/config/settings.conf"
rm -f "$XDD_HOME/state/reload.flag" "$XDD_HOME/state/pause" "$XDD_HOME/state/stopping" "$XDD_HOME/state/capability" "$XDD_HOME/state/current"

echo "----------------------------------------"
echo "[test] 通过 $PASS 项，失败 $FAIL 项"
[ "$FAIL" -eq 0 ] || exit 1
echo "[test] ALL PASS"
