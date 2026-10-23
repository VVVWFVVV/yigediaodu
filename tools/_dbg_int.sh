#!/usr/bin/env bash
cd /mnt/d/新调度 || exit 1
export XD_ROOT=/tmp/intdbg
export XDD_MODDIR="$PWD"
export XDD_HOME="$XD_ROOT/data/adb/WY925976742"
export XDD_BIN="$PWD/build-host/xindiaodud"
export PATH="$PWD/test/mock/bin:$PATH"
rm -rf "$XD_ROOT"
bash test/mock_tree.sh "$XD_ROOT" >/dev/null
rm -f "$XD_ROOT/data/adb/WY925976742/state/"* "$XD_ROOT/data/adb/WY925976742/baseline/"*
echo "=== 启动 daemon 6 秒..."
"$XDD_BIN" --daemon >/dev/null 2>&1 &
DPID=$!
sleep 6
echo "=== 进程还在？"
[ -d /proc/$DPID ] && echo "RUNNING" || echo "DEAD"
kill -TERM "$DPID" 2>/dev/null
sleep 1
echo "=== daemon.log:"
cat "$XD_ROOT/data/adb/WY925976742/log/daemon.log" 2>/dev/null | head -25
echo ""
echo "=== current:"
cat "$XD_ROOT/data/adb/WY925976742/state/current" 2>/dev/null || echo "(无)"
echo "=== capability:"
cat "$XD_ROOT/data/adb/WY925976742/state/capability" 2>/dev/null
