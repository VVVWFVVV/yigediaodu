#!/usr/bin/env bash
cd /tmp/fas/ex_fas-rs-ebpf || exit 1

echo "=== 1. 二进制中的关键节点/路径:"
strings fas-rs 2>/dev/null | grep -E "^/dev/|^/sys/|^/proc/|fas_rs|scaling_|mode" | sort -u | head -40

echo ""
echo "=== 2. 子命令:"
strings fas-rs 2>/dev/null | grep -E "^(run|merge|powersave|balance|performance|fast)$" | sort -u

echo ""
echo "=== 3. vtools/init_vtools.sh:"
cat vtools/init_vtools.sh

echo ""
echo "=== 4. vtools/powercfg.sh:"
cat vtools/powercfg.sh

echo ""
echo "=== 5. vtools/gen_json.sh:"
cat vtools/gen_json.sh
