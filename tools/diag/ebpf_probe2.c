/*
 * 一个调度 · eBPF 链路探针 v2（诊断工具，不进模块 zip）
 * ============================================================
 * 用途：v1 探针已定位 fas-rs 失败根因（libgui.so 中旧符号被 Android 移除，
 * aya 用户态符号解析失败）。本探针验证「换用设备上存在的稳定符号
 * hook_queueBuffer」后，frame-analyzer 的完整 eBPF 链路能否跑通：
 *   1) 创建 RING_BUF map（type=27, max_entries=4096）
 *   2) 加载 frame_analyzer_ebpf 程序（17 条指令，从 v4.9.1 二进制提取，
 *      imm64 重定位填入 map fd）——BPF_PROG_TYPE_KPROBE
 *   3) perf_event_open uprobe（config1=libgui.so 路径, config2=符号偏移）
 *      + PERF_EVENT_IOC_SET_BPF + ENABLE，目标 pid=-1（全进程）
 *   4) mmap ringbuf 消费事件，验证 (pid, ip) 事件真实到达
 * 全部通过 = 自研帧监控方案（替换 fas-rs）技术可行。
 *
 * ringbuf 记录布局（内核 kernel/bpf/ringbuf.c 实锤）：
 *   hdr->len = round_up(size + 8, 8)（已含 8 字节头）；记录总步进 = round_up(len, 8)；
 *   数据区 = hdr + 8，数据大小 = len - 8。
 *
 * 构建：<ndk>/.../aarch64-linux-android26-clang.cmd -O2 tools/diag/ebpf_probe2.c -o tools/diag/ebpf_probe2
 * 运行：adb push ... /data/local/tmp/ && adb shell su -c /data/local/tmp/ebpf_probe2
 * ============================================================
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/bpf.h>
#include <linux/perf_event.h>

#define PERF_EVENT_IOC_DISABLE 0x2401u      /* _IO ('$', 1) */
#define PERF_EVENT_IOC_SET_BPF 0x40042408u  /* _IOW('$', 8, __u32) */
#define PERF_EVENT_IOC_ENABLE  0x2400u      /* _IO ('$', 0) */

#define RINGBUF_SIZE    (1u << 12)          /* 4096 */
#define RINGBUF_MASK    (RINGBUF_SIZE - 1)
#define RINGBUF_HDR_SZ  8
#define BPF_RINGBUF_BUSY_BIT     (1u << 31)
#define BPF_RINGBUF_DISCARD_BIT  (1u << 30)

/* hook_queueBuffer 在设备 libgui.so 中的符号值（llvm-nm 实测，2026-10-24） */
#define HOOK_QUEUE_BUFFER_OFFSET 0x16f308u

static int g_ok, g_fail;

static void report(const char *name, int ok, int err) {
    if (ok) { printf("[ OK ] %s\n", name); g_ok++; }
    else    { printf("[FAIL] %-40s errno=%d (%s)\n", name, err, strerror(err)); g_fail++; }
}

static int bpf_cmd(enum bpf_cmd cmd, union bpf_attr *attr) {
    return (int)syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

/* frame_analyzer_ebpf 程序（从 fas-rs v4.9.1 内嵌 ELF 提取，17 条指令）：
 * insn[1]/insn[2] 为 ld_imm64 对，imm64 = RING_BUF map fd（运行时填入）。
 * 注意：对象文件里 insn[1].src_reg=0（plain imm64），加载器（libbpf/aya）在应用
 * map 重定位时会置 src_reg=BPF_PSEUDO_MAP_FD(1)——否则 verifier 把 r1 当标量，
 * 调用 bpf_ringbuf_reserve 时报参数类型不符（-EACCES）。本探针直接按加载后形态写。
 * 程序只做：ringbuf_reserve(16) → 写 (pid_tgid, ip) → ringbuf_submit。 */
#define BPF_PSEUDO_MAP_FD 1
static struct bpf_insn s_prog[] = {
    { 0xbf, 6, 1, 0, 0 },        /* r6 = r1（ctx=pt_regs） */
    { 0x18, 1, BPF_PSEUDO_MAP_FD, 0, 0 },  /* r1 = map fd（imm64 lo，下面填） */
    { 0x00, 0, 0, 0, 0 },        /* imm64 hi = 0 */
    { 0xb7, 2, 0, 0, 16 },       /* r2 = 16（预留 16 字节） */
    { 0xb7, 3, 0, 0, 0 },        /* r3 = 0 */
    { 0x85, 0, 0, 0, 0x83 },     /* call bpf_ringbuf_reserve */
    { 0xbf, 7, 0, 0, 0 },        /* r7 = r0（buf 指针） */
    { 0x15, 7, 0, 7, 0 },        /* if r7 == 0 goto +7 */
    { 0x85, 0, 0, 0, 5 },        /* call bpf_get_current_pid_tgid */
    { 0x79, 1, 6, 0, 0 },        /* r1 = *(u64 *)(r6 + 0)（regs->ip） */
    { 0x7b, 7, 1, 8, 0 },        /* *(u64 *)(r7 + 8) = r1（ip） */
    { 0x7b, 7, 0, 0, 0 },        /* *(u64 *)(r7 + 0) = r0（pid_tgid） */
    { 0xbf, 1, 7, 0, 0 },        /* r1 = r7 */
    { 0xb7, 2, 0, 0, 0 },        /* r2 = 0 */
    { 0x85, 0, 0, 0, 0x84 },     /* call bpf_ringbuf_submit */
    { 0xb7, 0, 0, 0, 0 },        /* r0 = 0 */
    { 0x95, 0, 0, 0, 0 },        /* exit */
};

static int read_pmu_type(void) {
    FILE *f = fopen("/sys/bus/event_source/devices/uprobe/type", "r");
    if (!f) return -1;
    int t = -1;
    if (fscanf(f, "%d", &t) != 1) t = -1;
    fclose(f);
    return t;
}

/* 在 /proc 里找 SurfaceFlinger 的 pid（它每帧合成都走 queueBuffer） */
static int find_sf_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char buf[128] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n > 0 && strstr(buf, "surfaceflinger")) { found = atoi(e->d_name); break; }
    }
    closedir(d);
    return found;
}

static long perf_event_open_probe(int pmu_type, const char *path, uint64_t offset,
                                  int pid, int cpu) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = pmu_type;
    attr.config1 = (uint64_t)(uintptr_t)path;  /* 文件路径（用户态指针，内核 strncpy_from_user） */
    attr.config2 = offset;                     /* 符号偏移（st_value） */
    return syscall(__NR_perf_event_open, &attr, pid, cpu, -1, 0UL);
}

int main(void) {
    printf("=== eBPF 链路探针 v2（hook_queueBuffer 路线）===\n");

    /* 1) RING_BUF map */
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.map_type = BPF_MAP_TYPE_RINGBUF;
    a.key_size = 0;
    a.value_size = 0;
    a.max_entries = RINGBUF_SIZE;
    int map_fd = bpf_cmd(BPF_MAP_CREATE, &a);
    report("创建 RING_BUF map（4096B）", map_fd >= 0, errno);
    if (map_fd < 0) return 1;

    /* 2) 加载程序（imm64 = map fd，src_reg=BPF_PSEUDO_MAP_FD） */
    static char vlog[4096];
    s_prog[1].imm = (int32_t)(uint32_t)map_fd;
    s_prog[2].imm = (int32_t)(uint32_t)((uint64_t)map_fd >> 32);
    memset(&a, 0, sizeof(a));
    a.prog_type = BPF_PROG_TYPE_KPROBE;
    a.insns = (uintptr_t)s_prog;
    a.insn_cnt = sizeof(s_prog) / sizeof(s_prog[0]);
    a.license = (uintptr_t)"GPL";
    a.log_level = 1;
    a.log_buf = (uintptr_t)vlog;
    a.log_size = sizeof(vlog);
    int prog_fd = bpf_cmd(BPF_PROG_LOAD, &a);
    report("加载 frame_analyzer 程序（KPROBE）", prog_fd >= 0, errno);
    if (prog_fd < 0) {
        if (vlog[0]) printf("       verifier: %s\n", vlog);
        close(map_fd); return 1;
    }

    /* 3) uprobe attach：同时挂多个候选符号，用事件 ip 区分哪个真触发。
     * hook_queueBuffer 实测计数=0（符号在但不在调用链上），需找出热路径入口。 */
    int pmu = read_pmu_type();
    if (pmu < 0) { report("读 uprobe PMU type", 0, errno); return 1; }
    struct { uint64_t off; const char *name; } probes[] = {
        { 0x16f308, "hook_queueBuffer(ANativeWindow*,buf*,int)" },
        { 0x170538, "queueBufferInternal(ANativeWindow*,buf*,int)" },
        { 0x17051c, "queueBufferBegin()" },
        { 0x170764, "queueBuffer(sp<GraphicBuffer>,sp<Fence>,out*)" },
        { 0x172b58, "queueBuffer(sp<GraphicBuffer>,QueueBufferInput,out*)" },
        { 0xfa550,  "queueBufferImpl(...)" },
        { 0x1706d8, "queueBufferMiddle()" },
    };
    size_t n_probes = sizeof(probes) / sizeof(probes[0]);
    int ev_fds[8];
    const char *ev_names[8];
    size_t n_ev = 0;
    for (size_t i = 0; i < n_probes; i++) {
        errno = 0;
        long fd = perf_event_open_probe(pmu, "/system/lib64/libgui.so",
                                        probes[i].off, -1, 0);
        if (fd < 0) {
            printf("[FAIL] uprobe %-46s errno=%d (%s)\n", probes[i].name, errno, strerror(errno));
            g_fail++;
            continue;
        }
        if (ioctl((int)fd, PERF_EVENT_IOC_SET_BPF, (unsigned long)prog_fd) != 0) {
            printf("[FAIL] SET_BPF %-44s errno=%d (%s)\n", probes[i].name, errno, strerror(errno));
            g_fail++;
            close((int)fd);
            continue;
        }
        ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0);
        ev_fds[n_ev] = (int)fd;
        ev_names[n_ev] = probes[i].name;
        n_ev++;
        printf("[ OK ] uprobe 已挂载: %s (off=0x%llx)\n", probes[i].name,
               (unsigned long long)probes[i].off);
        g_ok++;
    }
    if (n_ev == 0) { close(prog_fd); close(map_fd); return 1; }

    /* 3b) kprobe 对照测试：挂 do_nanosleep——探针自己的 usleep 循环就会触发。
     * kprobe 计数 >0 而 uprobe 全 0 → 内核 uprobe 安装被阉割（vendor 限制）。 */
    {
        FILE *f = fopen("/sys/bus/event_source/devices/kprobe/type", "r");
        if (f) {
            int kpmu = -1;
            if (fscanf(f, "%d", &kpmu) == 1 && kpmu >= 0) {
                struct perf_event_attr ka;
                memset(&ka, 0, sizeof(ka));
                ka.size = sizeof(ka);
                ka.type = kpmu;
                ka.config1 = (uint64_t)(uintptr_t)"__arm64_sys_nanosleep";
                ka.config2 = 0;
                long kfd = syscall(__NR_perf_event_open, &ka, (int)getpid(), -1, -1, 0UL);
                if (kfd >= 0) {
                    ioctl((int)kfd, PERF_EVENT_IOC_SET_BPF, (unsigned long)prog_fd);
                    ioctl((int)kfd, PERF_EVENT_IOC_ENABLE, 0);
                    ev_fds[n_ev] = (int)kfd;
                    ev_names[n_ev] = "kprobe:__arm64_sys_nanosleep（对照组）";
                    n_ev++;
                    printf("[ OK ] kprobe 对照组已挂载: do_nanosleep\n");
                    g_ok++;
                } else {
                    printf("[FAIL] kprobe do_nanosleep errno=%d (%s)\n", errno, strerror(errno));
                    g_fail++;
                }
            }
            fclose(f);
        }
    }

    /* 4) mmap ringbuf 并等待事件。
     * 内核约束（ringbuf_map_mmap_kern）：可写映射只允许恰好 1 页 @ 偏移 0
     * （consumer_pos 页）；生产者页与数据区只能只读映射。
     * 与 libbpf 相同做法：consumer 页单独 RW 映射，其余只读映射（2 倍数据
     * 大小以支持回环绕读）。 */
    long page = sysconf(_SC_PAGESIZE);
    void *cons_map = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, map_fd, 0);
    if (cons_map == MAP_FAILED) {
        report("mmap consumer 页", 0, errno);
        for (size_t i = 0; i < n_ev; i++) close(ev_fds[i]);
        close(prog_fd); close(map_fd); return 1;
    }
    report("mmap consumer 页", 1, 0);
    size_t ro_sz = (size_t)page + 2 * RINGBUF_SIZE;
    void *ro_map = mmap(NULL, ro_sz, PROT_READ, MAP_SHARED, map_fd, page);
    if (ro_map == MAP_FAILED) {
        report("mmap producer+data 区", 0, errno);
        munmap(cons_map, (size_t)page);
        for (size_t i = 0; i < n_ev; i++) close(ev_fds[i]);
        close(prog_fd); close(map_fd); return 1;
    }
    report("mmap producer+data 区", 1, 0);

    uint64_t *cons = (uint64_t *)cons_map;
    uint64_t *prod = (uint64_t *)ro_map;
    char *data = (char *)ro_map + page;

    printf("       等待 uprobe 事件（最多 8 秒，依赖前台有界面渲染）...\n");
    long events = 0;
    for (int i = 0; i < 80; i++) {
        uint64_t c = *cons, p = *prod;
        while (p > c) {
            uint32_t *lp = (uint32_t *)(data + (c & RINGBUF_MASK));
            uint32_t len = *lp;
            if (len & BPF_RINGBUF_BUSY_BIT) break;      /* 未提交，等下一轮 */
            uint32_t dlen = len & ~(BPF_RINGBUF_BUSY_BIT | BPF_RINGBUF_DISCARD_BIT);
            uint32_t step = (dlen + 7u) & ~7u;           /* 内核：len 已含头，步进=round_up(len,8) */
            if (!(len & BPF_RINGBUF_DISCARD_BIT) && dlen >= RINGBUF_HDR_SZ + 16) {
                uint64_t *rec = (uint64_t *)((char *)lp + RINGBUF_HDR_SZ);
                if (events < 5) {
                    printf("       事件#%ld: pid_tgid=0x%llx ip=0x%llx\n", events + 1,
                           (unsigned long long)rec[0], (unsigned long long)rec[1]);
                }
                events++;
            }
            c += step;
        }
        *cons = c;
        if (events >= 10) break;
        usleep(100000);
    }
    if (events > 0) {
        char name[64];
        snprintf(name, sizeof(name), "ringbuf 收到 %ld 个帧事件", events);
        report(name, 1, 0);
    } else {
        report("ringbuf 收到帧事件（8 秒无事件）", 0, ETIME);
    }

    /* 诊断：逐个读 perf 事件计数——哪个符号计数 >0 就在热路径上 */
    printf("       ---- 各探针触发计数（8 秒窗口）----\n");
    size_t fired = 0;
    for (size_t i = 0; i < n_ev; i++) {
        uint64_t count = 0;
        ssize_t rd = read(ev_fds[i], &count, sizeof(count));
        printf("       计数=%-8llu %s\n", (unsigned long long)count, ev_names[i]);
        if (count > 0) fired++;
    }
    if (events == 0 && fired > 0) {
        report("探针有触发但 ringbuf 无数据", 0, ETIME);
    } else if (events == 0) {
        report("全部探针均未触发（符号都不在热路径）", 0, ETIME);
    }

    for (size_t i = 0; i < n_ev; i++) ioctl(ev_fds[i], PERF_EVENT_IOC_DISABLE, 0);
    for (size_t i = 0; i < n_ev; i++) close(ev_fds[i]);
    close(prog_fd);
    close(map_fd);
    munmap(cons_map, (size_t)page);
    munmap(ro_map, ro_sz);
    printf("=== 汇总：%d 通过，%d 失败 ===\n", g_ok, g_fail);
    return g_fail > 0 ? 1 : 0;
}
