#!/system/bin/sh
# ============================================================
# 一个调度 · 守护进程监督者
# 启动 xindiaodud 并看护：pid + 心跳双重判活，
# 退出按退避重启；连续崩溃 3 次自动禁用并还原节点，绝不开机循环
# ============================================================

MODDIR="${0%/*}"
. "$MODDIR/script/pathinfo.sh"

BIN="$XDD_BIN"

# 监督者自身防冻结/防杀：与 daemon 同等对待（ColorOS 后台冻结曾把
# 监督者一起冻住，导致卡死的 daemon 没人重启——v1.0.3 真机教训）
echo -1000 > /proc/self/oom_score_adj 2>/dev/null

xdd_log() {
    echo "$(date '+%m-%d %H:%M:%S') [supervisor] $1" >> "$XDD_LOG/supervisor.log" 2>/dev/null
}

# 连续崩溃/卡死达到阈值：还原节点、落自禁标记、退出监督
crash_disable() {
    [ -x "$BIN" ] && "$BIN" --restore >/dev/null 2>&1
    touch "$XDD_STATE/disabled_crash"
    echo "$(date '+%m-%d %H:%M:%S') 连续崩溃 ${CRASH} 次，已自动禁用并还原节点；删除本文件可重新启用" \
        > "$XDD_STATE/disabled_crash.reason" 2>/dev/null
    xdd_log "连续崩溃 ${CRASH} 次，已自动禁用（原因见 state/disabled_crash.reason）"
    rm -f "$XDD_HOME/daemon.pid" 2>/dev/null
    rm -rf "$SUP_LOCK" 2>/dev/null
}

# ---------------- 单实例保护（mkdir 原子锁） ----------------
# 真机教训（v1.0.5 六测）：pid 文件单实例有时序漏洞——KSU 可能短跑两次
# service.sh，两个实例同时读到空/旧 pid 文件都启动，各自的 daemon 互踩
# 锁与 enforce，开机日志爆刷。mkdir 是原子操作，只有一个实例能建成，
# 从根上杜绝双监督者。
SUP_LOCK="$XDD_HOME/supervisor.lock"
if ! mkdir "$SUP_LOCK" 2>/dev/null; then
    OLD=$(cat "$SUP_LOCK/pid" 2>/dev/null)
    if [ -n "$OLD" ] && [ -d "/proc/$OLD" ]; then
        xdd_log "监督者已在运行 (pid $OLD)，本次启动退出"
        exit 0
    fi
    # 持有者已死（上次被杀未清理）：清掉陈旧锁再竞争
    rm -rf "$SUP_LOCK" 2>/dev/null
    mkdir "$SUP_LOCK" 2>/dev/null || exit 0
fi
echo $$ > "$SUP_LOCK/pid"

DAEMON_PID=""
STOP=0

shutdown() {
    STOP=1
    if [ -n "$DAEMON_PID" ] && [ -d "/proc/$DAEMON_PID" ]; then
        kill -TERM "$DAEMON_PID" 2>/dev/null
        i=0
        while [ -d "/proc/$DAEMON_PID" ] && [ "$i" -lt 10 ]; do
            sleep 1
            i=$((i + 1))
        done
        [ -d "/proc/$DAEMON_PID" ] && kill -9 "$DAEMON_PID" 2>/dev/null
    fi
    # 退出前按基线还原节点，不留残余
    [ -x "$BIN" ] && "$BIN" --restore >/dev/null 2>&1
    rm -f "$XDD_HOME/daemon.pid" 2>/dev/null
    rm -rf "$SUP_LOCK" 2>/dev/null
    xdd_log "监督者退出，内核节点已还原"
    exit 0
}
trap 'shutdown' TERM INT HUP

# ---------------- 等开机完成 ----------------
i=0
while [ "$(getprop sys.boot_completed 2>/dev/null)" != "1" ]; do
    sleep 2
    i=$((i + 1))
    [ "$i" -ge 90 ] && break
done
sleep 5

# 权限自愈（post-fs-data 已做过一次；service 阶段再确保，
# 覆盖「安装器在 customize 之后又改权限」等时序问题）。
# 范围：根目录 .sh、script/*.sh、守护二进制、捆绑的 fas-rs。
# fas-rs 执行位缺失会导致 daemon 反复 fork 失败、启动超时刷屏
# （v1.0.18 真机实测），故一并纳入自愈。
XDD_SELFHEAL=0
FAS_BIN="$MODDIR/libs/arm64-v8a/fas-rs"
for f in "$MODDIR"/*.sh "$MODDIR"/script/*.sh "$BIN" "$FAS_BIN"; do
    if [ -f "$f" ] && [ ! -x "$f" ]; then
        chmod 0755 "$f" 2>/dev/null && XDD_SELFHEAL=1
    fi
done
[ "$XDD_SELFHEAL" = "1" ] && xdd_log "权限自愈：已恢复执行位（service 阶段）"

if [ ! -x "$BIN" ]; then
    xdd_log "守护二进制缺失：$BIN，监督者退出"
    rm -rf "$SUP_LOCK" 2>/dev/null
    exit 0
fi

# ---------------- 开机大清底（仅本次监督者启动时执行一次） ----------------
# 杀光所有残留 xindiaodud（不限 daemon.pid）：手动启动的、旧监督者孤儿、
# 上次被杀未清理的，都在这覆盖。多 daemon 互踩 enforce 会让 cpuset 永远
# 写不进（v1.0.4/v1.0.5 真机教训）。
# 同时补杀 v1.0.5 时代 pid 文件单实例的旧监督者（cmdline 含 service.sh）——
# 旧监督者循环启动 daemon 被 flock 挡，产生「另一个守护实例持有 daemon.lock」
# 刷屏（v1.0.6 七测）。排除自身（$$）与父进程（KSU 安装器可能是父）。
touch "$XDD_STATE/stopping" 2>/dev/null
KILLED=""
for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    pid="${p#/proc/}"
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "$PPID" ] && continue
    # 只杀本模块的守护二进制、捆绑的 fas-rs 与旧版监督者：
    # xindiaodud（守护二进制名，v1.0.16 起模块 id 改为 WY925976742
    # 但二进制名保留）、fas-rs（v1.0.17 捆绑的帧感知调度——监督者重启时
    # 杀掉孤儿，让 daemon 用最新 games.toml 重新拉起）、
    # xindiaodu+service.sh（v1.0.16 之前的旧 id 监督者——若用户没按
    # 提示先卸载旧版，这里兜底接管，防止两个调度互相杀）。
    # 不能宽匹配 service.sh——其它模块的常驻 service.sh 会被误杀！
    # （NUL 分隔，grep -a 处理）
    if grep -aq "xindiaodud" "$p/cmdline" 2>/dev/null || \
       grep -aq "fas-rs" "$p/cmdline" 2>/dev/null || \
       { grep -aq "xindiaodu" "$p/cmdline" 2>/dev/null && \
         grep -aq "service\.sh" "$p/cmdline" 2>/dev/null; }; then
        kill -TERM "$pid" 2>/dev/null
        KILLED="$KILLED $pid"
    fi
done
if [ -n "$KILLED" ]; then
    i=0
    while [ "$i" -lt 3 ]; do
        ALIVE=""
        for pid in $KILLED; do
            [ -d "/proc/$pid" ] && ALIVE="$ALIVE $pid"
        done
        [ -z "$ALIVE" ] && break
        sleep 1
        i=$((i + 1))
    done
    for pid in $ALIVE; do
        kill -9 "$pid" 2>/dev/null
    done
    xdd_log "开机大清底：清理残留 daemon$KILLED"
fi
rm -f "$XDD_STATE/stopping"

# ---------------- 启动与看护 ----------------
CRASH=0
BACKOFF=1
STALE=0

while [ "$STOP" = "0" ]; do
    # 模块被禁用 / 已崩溃自禁：待命，不启动守护
    if [ -f "$MODDIR/disable" ] || [ -f "$XDD_STATE/disabled_crash" ]; then
        if [ -n "$DAEMON_PID" ] && [ -d "/proc/$DAEMON_PID" ]; then
            touch "$XDD_STATE/stopping" 2>/dev/null
            kill -TERM "$DAEMON_PID" 2>/dev/null
        fi
        DAEMON_PID=""
        sleep 5
        continue
    fi

    "$BIN" --daemon >> "$XDD_LOG/daemon.out" 2>&1 &
    DAEMON_PID=$!
    echo "$DAEMON_PID" > "$XDD_HOME/daemon.pid"
    CRASH=0
    BACKOFF=1
    STALE=0
    xdd_log "守护进程启动 pid=$DAEMON_PID"

    # 内层循环：看护当前这一代守护进程
    while [ "$STOP" = "0" ]; do
        sleep 2
        [ "$STOP" = "1" ] && break

        # 禁用检查：停掉守护并回到外层待命
        if [ -f "$MODDIR/disable" ] || [ -f "$XDD_STATE/disabled_crash" ]; then
            touch "$XDD_STATE/stopping" 2>/dev/null
            kill -TERM "$DAEMON_PID" 2>/dev/null
            sleep 1
            [ -d "/proc/$DAEMON_PID" ] && kill -9 "$DAEMON_PID" 2>/dev/null
            DAEMON_PID=""
            break
        fi

        # 进程已退出：先区分「按请求停止」与「崩溃」
        # （fcctl daemon stop / 卸载 / 禁用都会先落 stopping 标记，
        #  不计崩溃——否则手动停 3 次会误触自动禁用）
        if [ ! -d "/proc/$DAEMON_PID" ]; then
            # 回收僵尸：后台子进程退出后不 wait 会留 zombie，
            # /proc/<pid> 仍在会让 diag 误报「运行中」（v1.0.5 真机）
            wait "$DAEMON_PID" 2>/dev/null
            DAEMON_PID=""
            if [ -f "$XDD_STATE/stopping" ]; then
                rm -f "$XDD_STATE/stopping"
                CRASH=0
                xdd_log "守护进程按请求停止"
                sleep 1   # 退避：防止互斥退让时 tight loop 刷屏
                break
            fi
            CRASH=$((CRASH + 1))
            xdd_log "守护进程退出（本轮第 $CRASH 次），${BACKOFF}s 后重启"
            if [ "$CRASH" -ge 3 ]; then
                crash_disable
                exit 0
            fi
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF * 2))
            [ "$BACKOFF" -gt 30 ] && BACKOFF=30
            break
        fi

        # 心跳判活：算真实年龄。守护循环含 dumpsys（最长 3s）与空闲退避
        # （最长 5s），最坏间隔约 8s；阈值 12s、连续 3 次才判卡死，避免误杀。
        if [ -f "$XDD_STATE/heartbeat" ]; then
            NOW=$(date +%s 2>/dev/null || echo 0)
            HB=$(stat -c %Y "$XDD_STATE/heartbeat" 2>/dev/null || echo 0)
            AGE=$((NOW - HB))
            if [ "$AGE" -gt 12 ]; then
                STALE=$((STALE + 1))
                # 判活日志化：卡死时能从 supervisor.log 看出监督者是否在判活
                xdd_log "心跳超时（${AGE}s，连续第 $STALE 次）"
                if [ "$STALE" -ge 3 ]; then
                    xdd_log "守护进程心跳超时（${AGE}s），强杀重启"
                    kill -9 "$DAEMON_PID" 2>/dev/null
                    sleep 1
                    DAEMON_PID=""
                    CRASH=$((CRASH + 1))
                    if [ "$CRASH" -ge 3 ]; then
                        crash_disable
                        exit 0
                    fi
                    break
                fi
            else
                STALE=0
            fi
        else
            STALE=0
        fi
    done
done
