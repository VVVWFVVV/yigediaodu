#!/usr/bin/env bash
# ============================================================
# 一个调度 · 打包脚本（POSIX 等价，Git Bash / WSL 可跑）
# 校验：必需文件齐全 / .sh 无 CRLF / 二进制为 arm64 ELF
# 产物：dist/<模块id>-v<版本>.zip
# ============================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() { echo "[pack] FAIL $1"; exit 1; }
ok()   { echo "[pack] OK $1"; }
info() { echo "[pack] $1"; }

VER="$(sed -n 's/^version=//p' module.prop | head -n1)"
[ -n "$VER" ] || VER="v1.0"
# zip 名前缀取模块 id（v1.0.16 起 id=WY925976742，不再硬编码）
MODID="$(sed -n 's/^id=//p' module.prop | head -n1 | tr -d '\r')"
[ -n "$MODID" ] || MODID="unknown"

# ---------------- 必需文件 ----------------
REQUIRED="
module.prop
customize.sh
post-fs-data.sh
service.sh
action.sh
uninstall.sh
system.prop
META-INF/com/google/android/update-binary
script/pathinfo.sh
script/setup.sh
script/fcctl.sh
script/diag.sh
config/settings.conf
config/profiles/daily.conf
config/profiles/daily-plus.conf
config/profiles/game.conf
config/profiles/bg.conf
config/profiles/off.conf
config/perapp/com.miHoYo.Yuanshen.conf
config/perapp/com.tencent.tmgp.sgame.conf
config/perapp/com.tencent.tmgp.pubgmhd.conf
webroot/index.html
webroot/assets/app.css
webroot/assets/bridge.js
webroot/assets/api.js
webroot/assets/app.js
webroot/assets/pages.js
webroot/assets/page-settings.js
webroot/assets/page-diag.js
libs/arm64-v8a/xindiaodud
"

MISSING=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    [ -f "$f" ] || { echo "  缺少: $f"; MISSING=1; }
done <<< "$REQUIRED"
[ "$MISSING" = "0" ] || fail "必需文件不齐"
ok "必需文件齐全"

# ---------------- CRLF ----------------
if grep -rlq $'\r' --include='*.sh' . 2>/dev/null; then
    grep -rl $'\r' --include='*.sh' . 2>/dev/null | sed 's/^/  CRLF: /'
    fail "脚本含 CRLF"
fi
ok "全部脚本 LF 行尾"

# ---------------- 二进制架构 ----------------
BIN="libs/arm64-v8a/xindiaodud"
[ -f "$BIN" ] || fail "缺少 $BIN"
MAGIC="$(head -c4 "$BIN" | od -An -tx1 | tr -d ' \n')"
[ "$MAGIC" = "7f454c46" ] || fail "不是 ELF"
EM="$(head -c20 "$BIN" | od -An -tx1 -j18 -N2 | tr -d ' \n')"
[ "$EM" = "b700" ] || fail "不是 AArch64（e_machine=$EM）"
ok "二进制为 AArch64 ELF（$(( $(stat -c%s "$BIN" 2>/dev/null || stat -f%z "$BIN") / 1024 )) KB）"

# ---------------- 打 zip（python3，条目在根级） ----------------
mkdir -p dist
ZIP="dist/$MODID-$VER.zip"
rm -f "$ZIP"

python3 - "$ROOT" "$ZIP" <<'PYEOF'
import os, sys, zipfile

root, zip_path = sys.argv[1], sys.argv[2]
top_files = ["module.prop", "customize.sh", "post-fs-data.sh", "service.sh",
             "action.sh", "uninstall.sh", "system.prop"]
top_dirs = ["script", "config", "webroot", "META-INF", "libs", "docs"]

count = 0
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    for f in top_files:
        p = os.path.join(root, f)
        if os.path.isfile(p):
            z.write(p, f)
            count += 1
    for d in top_dirs:
        dp = os.path.join(root, d)
        if not os.path.isdir(dp):
            continue
        for dirpath, _, files in os.walk(dp):
            for fn in files:
                if fn.startswith("_"):
                    continue
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, root).replace(os.sep, "/")
                z.write(p, rel)
                count += 1
print(f"  {count} files")
PYEOF

ok "打包完成: $ZIP（$(( $(stat -c%s "$ZIP" 2>/dev/null || stat -f%z "$ZIP") / 1024 )) KB）"
info "刷入：KernelSU -> 模块 -> 从本地安装zip；或 Magisk -> 模块 -> 安装"
