#!/usr/bin/env bash
# 调试 fas section A
cd /mnt/d/新调度 || exit 1
export XD_ROOT=/tmp/fasdbg
rm -rf "$XD_ROOT"
bash test/mock_tree.sh "$XD_ROOT" >/dev/null
export XDD_HOME="$XD_ROOT/data/adb/WY925976742"
export XDD_BIN="$PWD/build-host/xindiaodud"
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"*
mkdir -p "$XD_ROOT/dev/fas_rs"
echo balance > "$XD_ROOT/dev/fas_rs/mode"
echo com.miHoYo.Yuanshen > "$XD_ROOT/state/fg"

echo "=== 启动 daemon（5 秒）..."
"$XDD_BIN" --daemon >/dev/null 2>&1 &
DPID=$!
sleep 5
kill -TERM "$DPID" 2>/dev/null
sleep 1

echo "=== daemon.log（前 20 行）:"
head -20 "$XD_ROOT/data/adb/WY925976742/log/daemon.log" 2>/dev/null

echo ""
echo "=== config 目录:"
ls -la "$XD_ROOT/data/adb/WY925976742/config/" 2>/dev/null || echo "(config 目录不存在)"

echo ""
echo "=== toml 内容:"
cat "$XD_ROOT/data/adb/WY925976742/config/fas_games.toml" 2>/dev/null || echo "(toml 不存在)"

echo ""
echo "=== capability:"
cat "$XD_ROOT/data/adb/WY925976742/state/capability" 2>/dev/null

echo ""
echo "=== live:"
cat "$XD_ROOT/data/adb/WY925976742/state/live" 2>/dev/null

echo ""
echo "=== policy0 min_freq:"
cat "$XD_ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"

echo ""
echo "=== probe fas:"
"$XDD_BIN" --probe fas
