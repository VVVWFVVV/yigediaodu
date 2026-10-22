#!/system/bin/sh
# ============================================================
# 一个调度 · 控制入口（WebUI 唯一调用面）
#
# 设计：前端不含任何 root 逻辑，所有读写都通过 ksu.exec 调用本脚本；
#       本脚本是唯一被 WebUI 触碰的执行文件，权限面最小。
#
# 输出：统一单行 JSON  {"ok":true,"data":...} / {"ok":false,"msg":"..."}
# 传参：标识类参数（包名/键名/档位）直接传；多行内容一律 base64 传入。
#
# 可靠性：每条命令都有硬看门狗——父子关系反转，父壳计时、子壳分发
#       （stdio 全落盘不持有 stdout 管道）；超时先输出单行错误 JSON
#       再杀子进程树，前端永远拿得到可读结果，不会永久转圈。
# ============================================================

MODDIR="${0%/*}"
case "$MODDIR" in
    */script) MODDIR="${MODDIR%/script}" ;;
esac
[ -f "$MODDIR/module.prop" ] || MODDIR="/data/adb/modules/WY925976742"

XDD_HOME="${XDD_HOME:-/data/adb/WY925976742}"
# XDD_BIN 可被环境变量覆盖（host 测试指向本机编译的二进制）；设备上默认模块内二进制
XDD_BIN="${XDD_BIN:-$MODDIR/libs/arm64-v8a/xindiaodud}"
XDD_CONFIG="$MODDIR/config"
XDD_TMP="$XDD_HOME/tmp"

CMD="${1:-}"
[ -n "$CMD" ] || CMD="status"
shift 2>/dev/null || true

mkdir -p "$XDD_TMP" 2>/dev/null

# ---------------- 超时分档（秒） ----------------
# 约束：前端 bridge 默认 12s 超时，必须大于此处任何一档，
# 否则前端先超时，永远拿不到后端给出的可读原因。
case "$CMD" in
    apply|export) TO=8 ;;
    diag) TO=10 ;;
    *) TO=6 ;;
esac
[ -n "$FC_CMD_TIMEOUT" ] && TO="$FC_CMD_TIMEOUT"

# ---------------- 工具函数 ----------------
jesc() {  # 转义 JSON 字符串：引号、反斜杠、制表符、换行（awk 逐行处理，单行/多行都正确）
    printf '%s' "$1" | awk '
    {
        gsub(/\\/, "\\\\")
        gsub(/"/, "\\\"")
        gsub(/\t/, "\\t")
        if (NR > 1) printf "\\n"
        printf "%s", $0
    }'
}

ok() {  # ok <data-json>  —— data-json 为合法 JSON 片段
    printf '{"ok":true,"data":%s}\n' "${1:-\{\}}"
}

err() {  # err <消息>
    printf '{"ok":false,"msg":"%s"}\n' "$(jesc "$1")"
}

cfg_get() {  # cfg_get <文件> <键> -> 值
    [ -f "$1" ] || return 1
    awk -v k="$2" '{ eq=index($0,"="); if (eq>0) { key=substr($0,1,eq-1); if (key==k) { print substr($0,eq+1); exit } } }' "$1"
}

cfg_set() {  # cfg_set <文件> <键> <值>（原子写）
    f="$1"; k="$2"; v="$3"
    mkdir -p "$(dirname "$f")" 2>/dev/null
    tmp="$f.fcctl.tmp"
    if [ -f "$f" ]; then
        awk -v k="$k" -v v="$v" 'BEGIN{done=0}
            { eq=index($0,"="); key=(eq>0)?substr($0,1,eq-1):$0
              if (!done && key==k) { print k "=" v; done=1 } else print $0 }
            END{ if (!done) print k "=" v }' "$f" > "$tmp" 2>/dev/null || { echo "$k=$v" > "$tmp"; }
    else
        echo "$k=$v" > "$tmp"
    fi
    mv "$tmp" "$f"
}

cfg_del() {  # cfg_del <文件> <键>
    f="$1"; k="$2"
    [ -f "$f" ] || return 0
    tmp="$f.fcctl.tmp"
    awk -v k="$k" '{ eq=index($0,"="); key=(eq>0)?substr($0,1,eq-1):$0; if (key!=k) print $0 }' "$f" > "$tmp" 2>/dev/null && mv "$tmp" "$f"
    return 0
}

touch_reload() {
    : > "$XDD_HOME/state/reload.flag" 2>/dev/null
    return 0
}

bin_ok() {
    [ -x "$XDD_BIN" ]
}

# 单行 base64：-w0 不可用时退 tr -d 换行（换行会撕裂 JSON 字符串）
b64file() {
    B=$(base64 -w0 "$1" 2>/dev/null)
    [ -n "$B" ] || B=$(base64 "$1" 2>/dev/null | tr -d '\n')
    printf '%s' "$B"
}

# ---------------- 命令分发（子壳内执行） ----------------
fc_dispatch() {
    case "$1" in
        version)
            # 只读 module.prop，不依赖守护二进制：
            # 二进制缺失/无执行位时版本号与面板顶栏仍可用（v1.0 首测教训）
            V=$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null | head -n 1)
            ok "\"$(jesc "${V:-v1.0}")\""
            ;;
        status)
            # 二进制输出本身已是完整信封（{"ok":true,...}），直接透传
            if bin_ok; then
                "$XDD_BIN" --status || err "守护二进制返回非零"
            else
                err "守护二进制缺失"
            fi
            ;;
        live)
            # state/live（key=value）-> JSON 对象
            if [ -f "$XDD_HOME/state/live" ]; then
                J=$(awk -F= 'NF>=2 { gsub(/"/,"\\\"",$2); printf "%s\"%s\":\"%s\"", sep, $1, $2; sep="," }' "$XDD_HOME/state/live" 2>/dev/null)
                ok "{$J}"
            else
                ok "{}"
            fi
            ;;
        probe)
            # 二进制输出已是完整信封（{"ok":true,"data":...}），直接透传
            if bin_ok; then
                "$XDD_BIN" --probe "$2" "$3" || err "probe 失败"
            else
                err "守护二进制缺失"
            fi
            ;;
        selfcheck)
            if bin_ok; then
                OUT=$("$XDD_BIN" --selfcheck 2>&1)
                RC=$?
                printf '{"ok":%s,"data":"%s"}\n' "$([ $RC -eq 0 ] && echo true || echo false)" "$(jesc "$OUT")"
            else
                err "守护二进制缺失"
            fi
            ;;
        settings)
            if [ -f "$XDD_CONFIG/settings.conf" ]; then
                ok "\"$(b64file "$XDD_CONFIG/settings.conf")\""
            else
                err "settings.conf 不存在"
            fi
            ;;
        setsetting)
            k="$2"; v="$3"
            case "$k" in
                ''|*[!A-Za-z0-9_.]*) err "非法设置键名"; return 0 ;;
            esac
            case "$v" in
                ''|*[!A-Za-z0-9_.,:%/-]*) err "非法设置值"; return 0 ;;
            esac
            cfg_set "$XDD_CONFIG/settings.conf" "$k" "$v"
            touch_reload
            ok "\"$k=$v\""
            ;;
        profiles)
            LIST=""
            for f in "$XDD_CONFIG"/profiles/*.conf; do
                [ -f "$f" ] || continue
                n=$(basename "$f" .conf)
                [ -n "$LIST" ] && LIST="$LIST,"
                LIST="$LIST\"$(jesc "$n")\""
            done
            ok "[$LIST]"
            ;;
        getprofile)
            n="$2"
            f="$XDD_CONFIG/profiles/$n.conf"
            [ -f "$f" ] || { err "档位不存在: $n"; return 0; }
            ok "\"$(b64file "$f")\""
            ;;
        setprofile)
            n="$2"; b64="$3"
            case "$n" in
                ''|*[!A-Za-z0-9_.-]*) err "非法档位名"; return 0 ;;
            esac
            f="$XDD_CONFIG/profiles/$n.conf"
            printf '%s' "$b64" | base64 -d > "$f" 2>/dev/null || { err "base64 解码失败"; return 0; }
            [ -s "$f" ] || { err "档位内容为空"; rm -f "$f"; return 0; }
            touch_reload
            ok "\"$n\""
            ;;
        list)
            LIST=""
            for f in "$XDD_CONFIG"/perapp/*.conf; do
                [ -f "$f" ] || continue
                n=$(basename "$f" .conf)
                m=$(cfg_get "$f" mode)
                [ -n "$m" ] || m="daily"
                [ -n "$LIST" ] && LIST="$LIST,"
                LIST="$LIST{\"pkg\":\"$(jesc "$n")\",\"mode\":\"$(jesc "$m")\"}"
            done
            ok "[$LIST]"
            ;;
        mode)
            pkg="$2"; m="$3"
            case "$pkg" in
                ''|*[!A-Za-z0-9_.]*) err "非法包名"; return 0 ;;
            esac
            case "$m" in
                daily|daily-plus|game|bg|off) ;;
                *) err "未知档位: $m"; return 0 ;;
            esac
            f="$XDD_CONFIG/perapp/$pkg.conf"
            if [ "$m" = "daily" ]; then
                rm -f "$f"   # 默认档：删配置即恢复默认行为
            else
                printf '# %s\nmode=%s\n' "$pkg" "$m" > "$f"
            fi
            touch_reload
            ok "\"$pkg -> $m\""
            ;;
        delapp)
            pkg="$2"
            case "$pkg" in
                ''|*[!A-Za-z0-9_.]*) err "非法包名"; return 0 ;;
            esac
            rm -f "$XDD_CONFIG/perapp/$pkg.conf"
            touch_reload
            ok "\"$pkg\""
            ;;
        logs)
            n="${2:-50}"
            case "$n" in ''|*[!0-9]*) n=50 ;; esac
            [ "$n" -gt 500 ] && n=500
            if [ -f "$XDD_HOME/log/daemon.log" ]; then
                TMPL="$XDD_TMP/fcctl.logs"
                tail -n "$n" "$XDD_HOME/log/daemon.log" > "$TMPL" 2>/dev/null
                ok "\"$(b64file "$TMPL")\""
                rm -f "$TMPL"
            else
                ok "\"\""
            fi
            ;;
        export)
            TMPE="$XDD_TMP/fcctl.export"
            : > "$TMPE"
            for f in "$XDD_CONFIG/settings.conf" "$XDD_CONFIG"/profiles/*.conf "$XDD_CONFIG"/perapp/*.conf; do
                [ -f "$f" ] || continue
                printf '===== %s =====\n' "${f#$MODDIR/}" >> "$TMPE"
                cat "$f" >> "$TMPE"
                printf '\n' >> "$TMPE"
            done
            ok "\"$(b64file "$TMPE")\""
            rm -f "$TMPE"
            ;;
        reload)
            touch_reload
            ok "true"
            ;;
        pause)
            : > "$XDD_HOME/state/pause" 2>/dev/null
            ok "true"
            ;;
        resume)
            rm -f "$XDD_HOME/state/pause" 2>/dev/null
            ok "true"
            ;;
        enable)
            v="$2"
            case "$v" in
                0|1) cfg_set "$XDD_CONFIG/settings.conf" enabled "$v"; touch_reload; ok "\"$v\"" ;;
                *) err "enable 只接受 0 或 1" ;;
            esac
            ;;
        apply)
            n="$2"
            [ -n "$n" ] || { err "apply 需要档位名"; return 0; }
            bin_ok && "$XDD_BIN" --apply "$n" || err "守护二进制缺失"
            ;;
        daemon)
            act="${2:-status}"
            PIDF="$XDD_HOME/daemon.pid"
            case "$act" in
                status)
                    # 运行中判定排除 zombie（/proc 在 zombie 下仍存在，
                    # v1.0.5 真机 diag 误报「运行中」的根因）
                    P=$(cat "$PIDF" 2>/dev/null)
                    ALIVE=0
                    if [ -n "$P" ] && [ -d "/proc/$P" ]; then
                        ST=$(sed -n 's/^.*) \([A-Z]\) .*/\1/p' "/proc/$P/stat" 2>/dev/null)
                        [ "$ST" != "Z" ] && ALIVE=1
                    fi
                    if [ "$ALIVE" = "1" ]; then
                        ok "{\"running\":true,\"pid\":$P}"
                    else
                        ok "{\"running\":false,\"pid\":0}"
                    fi
                    ;;
                stop)
                    P=$(cat "$PIDF" 2>/dev/null)
                    if [ -n "$P" ] && [ -d "/proc/$P" ]; then
                        # 停止标记：监督者不计崩溃
                        : > "$XDD_HOME/state/stopping" 2>/dev/null
                        kill -TERM "$P" 2>/dev/null
                        i=0
                        while [ -d "/proc/$P" ] && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done
                        [ -d "/proc/$P" ] && kill -9 "$P" 2>/dev/null
                    fi
                    # 监督者会自动拉起；要真正停用需在管理器里关闭模块
                    ok "true"
                    ;;
                restart)
                    P=$(cat "$PIDF" 2>/dev/null)
                    if [ -n "$P" ] && [ -d "/proc/$P" ]; then
                        : > "$XDD_HOME/state/stopping" 2>/dev/null
                        kill -TERM "$P" 2>/dev/null
                    fi
                    ok "true"
                    ;;
                *)
                    err "daemon 子命令: status/stop/restart"
                    ;;
            esac
            ;;
        diag)
            TMPD="$XDD_TMP/fcctl.diag"
            sh "$MODDIR/script/diag.sh" > "$TMPD" 2>&1
            printf '{"ok":true,"data":"%s"}\n' "$(b64file "$TMPD")"
            rm -f "$TMPD"
            ;;
        *)
            err "未知命令: $1"
            ;;
    esac
}

# ---------------- 看门狗父壳 ----------------
# 子壳 stdio 全落盘：绝不持有 stdout 管道，否则超时路径上前端等不到 EOF
OUTF="$XDD_TMP/fcctl.out"
ERRF="$XDD_TMP/fcctl.err"
: > "$OUTF" 2>/dev/null
: > "$ERRF" 2>/dev/null

(
    fc_dispatch "$CMD" "$@"
) > "$OUTF" 2> "$ERRF" &
CHILD=$!

# 计时：基于真实时间（date +%s），而非迭代次数。
# sleep 0.1 不可用退回 sleep 1 时，迭代计数会变成 10 倍超时。
START_TS=$(date +%s 2>/dev/null || echo 0)
while :; do
    kill -0 "$CHILD" 2>/dev/null || break
    NOW_TS=$(date +%s 2>/dev/null || echo 0)
    ELAPSED=$((NOW_TS - START_TS))
    [ "$ELAPSED" -ge "$TO" ] && break
    sleep 0.1 2>/dev/null || sleep 1
done

if kill -0 "$CHILD" 2>/dev/null; then
    # 超时：先给前端一个可读答案，再杀子进程树
    printf '{"ok":false,"msg":"命令超时（%ss）：%s"}\n' "$TO" "$CMD"
    kill -TERM "$CHILD" 2>/dev/null
    sleep 0.2 2>/dev/null || sleep 1
    kill -9 "$CHILD" 2>/dev/null
    # 等子壳真正退出，避免僵尸
    wait "$CHILD" 2>/dev/null
    # 杀子壳派生的孙进程（dumpsys / 二进制）：扫 /proc 找子孙
    for p in /proc/[0-9]*; do
        [ -r "$p/stat" ] || continue
        pp=$(awk '{ for(i=1;i<=NF;i++) if ($i ~ /^PPid:/) { print $(i+1); exit } }' "$p/stat" 2>/dev/null)
        [ -n "$pp" ] || continue
        if [ "$pp" = "$CHILD" ]; then
            cpid="${p#/proc/}"
            kill -9 "$cpid" 2>/dev/null
            for q in /proc/[0-9]*; do
                [ -r "$q/stat" ] || continue
                pp2=$(awk '{ for(i=1;i<=NF;i++) if ($i ~ /^PPid:/) { print $(i+1); exit } }' "$q/stat" 2>/dev/null)
                [ "$pp2" = "$cpid" ] && kill -9 "${q#/proc/}" 2>/dev/null
            done
        fi
    done
    rm -f "$OUTF" "$ERRF" 2>/dev/null
    exit 0
fi

wait "$CHILD" 2>/dev/null
cat "$OUTF" 2>/dev/null
rm -f "$OUTF" "$ERRF" 2>/dev/null
exit 0
