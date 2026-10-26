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
    # 不删 supervisor.lock：flock 随进程退出自动释放，删文件会换 inode
}

# ---------------- 单实例保护（flock 原子锁） ----------------
# 演进史：pid 文件单实例（v1.0.5 前）→ mkdir+pid 目录锁（v1.0.6）→ flock（v1.0.20）。
# mkdir+pid 的真机教训（v1.0.19 测）：刷模块后双 supervisor 并存、失败方每秒
# 重拉 daemon 刷屏 600+ 条/11 分钟。根因是目录锁的两处硬伤：
#   ① mkdir 成功到写 pid 文件之间有 TOCTOU 窗口，第二个实例读到空 pid
#      会删锁重建，两个实例都越过互斥；
#   ② 陈旧锁回收只认 pid 活性，认不出本模块在跑的监督者。
# flock 是内核级原子锁：第二个实例拿不到锁立即退出；且退出点在大清底
# **之前**，根本不会执行 cleanup 杀别人的 daemon。
# 锁文件永不删除：删除会换 inode，新旧实例锁不同 inode 会同时"持锁"
# （daemon.lock 的注释记载过同类教训，pid 文件互斥就是这么来的）。
SUP_LOCK="$XDD_HOME/supervisor.lock"

# 兼容旧版目录锁（v1.0.19 及之前）：目录存在且持有者是本模块活监督者 → 退出，
# 不制造第三个 supervisor。覆盖"老监督者活着但其 pid 文件陈旧"的升级窗口。
if [ -d "$SUP_LOCK" ]; then
    OLD=$(cat "$SUP_LOCK/pid" 2>/dev/null)
    if [ -n "$OLD" ] && [ -d "/proc/$OLD" ] && \
       grep -aq "service\.sh" "/proc/$OLD/cmdline" 2>/dev/null; then
        xdd_log "监督者已在运行 (pid $OLD)，本次启动退出"
        exit 0
    fi
    rm -rf "$SUP_LOCK" 2>/dev/null
fi

# flock 缺失时安全失败（没有监督者好过双监督者互踩）
if ! command -v flock >/dev/null 2>&1; then
    xdd_log "flock 不可用，无法保证单实例，监督者退出"
    exit 0
fi
exec 9>"$SUP_LOCK"
if ! flock -n 9; then
    xdd_log "另一个监督者持有 supervisor.lock，本次启动退出"
    exit 0
fi
echo $$ > "$XDD_HOME/supervisor.pid"   # 仅诊断展示，不参与互斥

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
    # 不删 supervisor.lock：flock 随进程退出自动释放（v1.0.20）
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

# v1.2.0：清理旧版 fas-rs 的 USER_CONFIG。只删本模块生成的文件，
# 避免误删用户自己安装的 fas-rs 配置（首行必须是本模块的生成标记）。
USER_TOML=/sdcard/Android/fas-rs/games.toml
if [ -f "$USER_TOML" ] && head -n 1 "$USER_TOML" 2>/dev/null | grep -q '由一个调度自动生成'; then
    rm -f "$USER_TOML" 2>/dev/null
    rmdir /sdcard/Android/fas-rs 2>/dev/null
    xdd_log "已清理旧版帧感知调度 USER_CONFIG"
fi

# 权限自愈（post-fs-data 已做过一次；service 阶段再确保，
# 覆盖「安装器在 customize 之后又改权限」等时序问题）。
# 范围：根目录 .sh、script/*.sh、守护二进制。
XDD_SELFHEAL=0
for f in "$MODDIR"/*.sh "$MODDIR"/script/*.sh "$BIN"; do
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
    # 只杀本模块的守护二进制与旧版监督者：
    # xindiaodud（守护二进制名，v1.0.16 起模块 id 改为 WY925976742
    # 但二进制名保留）；
    # 以及旧版/本版监督者（cmdline 同时含模块 id 与 service.sh）：
    # v1.0.19 真机教训——旧匹配只认 v1.0.16 之前的老 id "xindiaodu"，
    # 现行 id "WY925976742" 的老监督者永不被清理，与新监督者并存刷屏。
    # 不能宽匹配 service.sh——其它模块的常驻 service.sh 会被误杀！
    # （NUL 分隔，grep -a 处理）
    if grep -aq "xindiaodud" "$p/cmdline" 2>/dev/null || \
       { grep -aq "xindiaodu" "$p/cmdline" 2>/dev/null && \
         grep -aq "service\.sh" "$p/cmdline" 2>/dev/null; } || \
       { grep -aq "WY925976742" "$p/cmdline" 2>/dev/null && \
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
    CONFLICTS=0   # v1.0.20：连续锁冲突退让次数（另一个 daemon 持锁）
    LAST_WAIT=0   # v1.0.20：上一次退避秒数（仅台阶变化时打日志）
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

        # 进程已退出：三分支——「按请求停止」「锁冲突退让」「崩溃」
        # （fcctl daemon stop / 卸载 / 禁用都会先落 stopping 标记，
        #  不计崩溃——否则手动停 3 次会误触自动禁用；
        #  lock_yield 是 daemon 互斥退让标记，v1.0.20 从 stopping 拆出）
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
            if [ -f "$XDD_STATE/lock_yield" ]; then
                # v1.0.20：互斥退让（另一个 daemon 持锁）独立处理。
                # 曾经的坑：daemon 冲突退出也写 stopping，被按请求停止路径
                # 重置 CRASH + 1 秒重拉，形成无限循环且崩溃自禁被绕过
                # （v1.0.19 真机刷屏 600+ 条/11 分钟）。
                # 现：指数退避（1s→30s 上限）+ 仅状态变更打日志；
                # 不退出——赢家 daemon 死后 30s 内本监督者自动接管。
                rm -f "$XDD_STATE/lock_yield"
                CONFLICTS=$((CONFLICTS + 1))
                SHIFT=$CONFLICTS
                [ "$SHIFT" -gt 5 ] && SHIFT=5
                WAIT=$((1 << SHIFT))
                [ "$WAIT" -gt 30 ] && WAIT=30
                if [ "$CONFLICTS" = "1" ]; then
                    xdd_log "daemon 互斥退让（另一 daemon 持锁），转入 ${WAIT}s 退避观察"
                elif [ "$WAIT" != "$LAST_WAIT" ]; then
                    xdd_log "锁冲突持续（第 $CONFLICTS 次），退避延长至 ${WAIT}s"
                fi
                LAST_WAIT=$WAIT
                sleep "$WAIT"
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

        # 守护存活：解除此前的锁冲突退让状态（v1.0.20）
        if [ "$CONFLICTS" -gt 0 ]; then
            xdd_log "锁冲突解除，恢复看护（pid $DAEMON_PID）"
            CONFLICTS=0
            LAST_WAIT=0
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
