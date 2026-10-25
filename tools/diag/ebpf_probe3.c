/*
 * 一个调度 · eBPF 链路探针 v3（诊断工具，不进模块 zip）
 * ============================================================
 * 终局判定：v1/v2 已确认 map/程序/ringbuf/SET_BPF/ENABLE 全通，
 * 唯一未明的是 uprobe 是否真能触发（v2 用 aya 的 cpu=0 组合，可能因渲染
 * 线程在大核而漏掉；kprobe 对照组选错过两次函数名）。
 * 本探针：
 *   1) uprobe queueBufferImpl(0xfa550) → 挂到**目标游戏进程 pid + cpu=-1**
 *   2) kprobe __arm64_sys_nanosleep → 挂到自身 pid + cpu=-1（对照组）
 *   3) kprobe hrtimer_nanosleep → 挂到自身 pid + cpu=-1（对照组 2）
 * 任一 uprobe 触发即证明链路可用；对照组用于确认 kprobe 机制本身。
 *
 * 用法：ebpf_probe3 <目标进程pid>
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

#define IOC_DISABLE 0x2401u
#define IOC_SET_BPF 0x40042408u
#define IOC_ENABLE  0x2400u

#define RINGBUF_SIZE (1u << 12)
#define RINGBUF_MASK (RINGBUF_SIZE - 1)
#define HDR_SZ 8
#define BUSY_BIT (1u << 31)
#define DISCARD_BIT (1u << 30)

static int g_ok, g_fail;
static void report(const char *name, int ok, int err) {
    if (ok) { printf("[ OK ] %s\n", name); g_ok++; }
    else    { printf("[FAIL] %-44s errno=%d (%s)\n", name, err, strerror(err)); g_fail++; }
}
static int bpf_cmd(enum bpf_cmd cmd, union bpf_attr *attr) {
    return (int)syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

#define BPF_PSEUDO_MAP_FD 1
static struct bpf_insn s_prog[] = {
    { 0xbf, 6, 1, 0, 0 },
    { 0x18, 1, BPF_PSEUDO_MAP_FD, 0, 0 },
    { 0x00, 0, 0, 0, 0 },
    { 0xb7, 2, 0, 0, 16 },
    { 0xb7, 3, 0, 0, 0 },
    { 0x85, 0, 0, 0, 0x83 },
    { 0xbf, 7, 0, 0, 0 },
    { 0x15, 7, 0, 7, 0 },
    { 0x85, 0, 0, 0, 5 },
    { 0x79, 1, 6, 0, 0 },
    { 0x7b, 7, 1, 8, 0 },
    { 0x7b, 7, 0, 0, 0 },
    { 0xbf, 1, 7, 0, 0 },
    { 0xb7, 2, 0, 0, 0 },
    { 0x85, 0, 0, 0, 0x84 },
    { 0xb7, 0, 0, 0, 0 },
    { 0x95, 0, 0, 0, 0 },
};

static int read_pmu_type(const char *pmu) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", pmu);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int t = -1;
    if (fscanf(f, "%d", &t) != 1) t = -1;
    fclose(f);
    return t;
}

/* kind: 0=uprobe(config1=路径,config2=偏移) 1=kprobe(config1=函数名,config2=0) */
static long open_probe(int pmu_type, int kind, const char *s1, uint64_t s2,
                       int pid, int cpu) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.type = pmu_type;
    a.config1 = (uint64_t)(uintptr_t)s1;
    a.config2 = s2;
    /* 关键：BPF 附着的事件需要 sample_period=1 才会真正触发
     * （纯计数事件不会调用 BPF 程序——v3 首次测试漏了它，零事件） */
    a.sample_type = 1u << 3;  /* PERF_SAMPLE_RAW */
    a.sample_period = 1;
    a.wakeup_events = 1;
    (void)kind;
    return syscall(__NR_perf_event_open, &a, pid, cpu, -1, 0UL);
}

int main(int argc, char **argv) {
    /* 用法: ebpf_probe3 <目标pid> [路径] [偏移hex] */
    int target_pid = (argc > 1) ? atoi(argv[1]) : -1;
    const char *target_path = (argc > 2) ? argv[2] : "/system/lib64/libgui.so";
    uint64_t target_off = (argc > 3) ? (uint64_t)strtoull(argv[3], NULL, 16) : 0xfa550;
    printf("=== eBPF 链路探针 v3（pid=%d, 自身=%d）===\n", target_pid, getpid());
    printf("       目标: %s + 0x%llx\n", target_path, (unsigned long long)target_off);
    if (target_pid <= 0) { printf("用法: ebpf_probe3 <目标进程pid> [路径] [偏移hex]\n"); return 2; }

    /* 1) RING_BUF map */
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.map_type = BPF_MAP_TYPE_RINGBUF;
    a.key_size = 0; a.value_size = 0; a.max_entries = RINGBUF_SIZE;
    int map_fd = bpf_cmd(BPF_MAP_CREATE, &a);
    report("创建 RING_BUF map", map_fd >= 0, errno);
    if (map_fd < 0) return 1;

    /* 2) 加载程序 */
    static char vlog[4096];
    s_prog[1].imm = (int32_t)(uint32_t)map_fd;
    s_prog[2].imm = 0;
    memset(&a, 0, sizeof(a));
    a.prog_type = BPF_PROG_TYPE_KPROBE;
    a.insns = (uintptr_t)s_prog;
    a.insn_cnt = sizeof(s_prog) / sizeof(s_prog[0]);
    a.license = (uintptr_t)"GPL";
    a.log_level = 1; a.log_buf = (uintptr_t)vlog; a.log_size = sizeof(vlog);
    int prog_fd = bpf_cmd(BPF_PROG_LOAD, &a);
    report("加载程序", prog_fd >= 0, errno);
    if (prog_fd < 0) { if (vlog[0]) printf("       verifier: %s\n", vlog); close(map_fd); return 1; }

    /* 3) 三个探针 */
    int upmu = read_pmu_type("uprobe");
    int kpmu = read_pmu_type("kprobe");
    printf("       uprobe PMU=%d, kprobe PMU=%d\n", upmu, kpmu);
    struct { int fd; const char *name; } probes[4];
    int np = 0;
    long fd;

    /* uprobe: 目标符号 @ 目标 pid, cpu=-1 */
    fd = open_probe(upmu, 0, target_path, target_off, target_pid, -1);
    if (fd >= 0 && ioctl((int)fd, IOC_SET_BPF, (unsigned long)prog_fd) == 0) {
        ioctl((int)fd, IOC_ENABLE, 0);
        probes[np].fd = (int)fd; probes[np].name = "uprobe:目标符号 @目标pid,cpu=-1"; np++;
        report("uprobe 目标符号 @目标pid", 1, 0);
    } else { report("uprobe 目标符号 @目标pid", 0, errno); if (fd >= 0) close((int)fd); }

    /* kprobe 对照 1: __arm64_sys_nanosleep @ 自身 */
    fd = open_probe(kpmu, 1, "__arm64_sys_nanosleep", 0, (int)getpid(), -1);
    if (fd >= 0 && ioctl((int)fd, IOC_SET_BPF, (unsigned long)prog_fd) == 0) {
        ioctl((int)fd, IOC_ENABLE, 0);
        probes[np].fd = (int)fd; probes[np].name = "kprobe:__arm64_sys_nanosleep @self"; np++;
        report("kprobe 对照1", 1, 0);
    } else { report("kprobe 对照1", 0, errno); if (fd >= 0) close((int)fd); }

    /* kprobe 对照 2: hrtimer_nanosleep @ 自身 */
    fd = open_probe(kpmu, 1, "hrtimer_nanosleep", 0, (int)getpid(), -1);
    if (fd >= 0 && ioctl((int)fd, IOC_SET_BPF, (unsigned long)prog_fd) == 0) {
        ioctl((int)fd, IOC_ENABLE, 0);
        probes[np].fd = (int)fd; probes[np].name = "kprobe:hrtimer_nanosleep @self"; np++;
        report("kprobe 对照2", 1, 0);
    } else { report("kprobe 对照2", 0, errno); if (fd >= 0) close((int)fd); }

    /* 4) ringbuf mmap（libbpf 方式） */
    long page = sysconf(_SC_PAGESIZE);
    void *cons_map = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, map_fd, 0);
    size_t ro_sz = (size_t)page + 2 * RINGBUF_SIZE;
    void *ro_map = mmap(NULL, ro_sz, PROT_READ, MAP_SHARED, map_fd, page);
    if (cons_map == MAP_FAILED || ro_map == MAP_FAILED) {
        report("mmap ringbuf", 0, errno);
        return 1;
    }
    report("mmap ringbuf", 1, 0);
    uint64_t *cons = (uint64_t *)cons_map;
    uint64_t *prod = (uint64_t *)ro_map;
    char *data = (char *)ro_map + page;

    printf("       等待 10 秒（游戏渲染中 + 自身 usleep 循环）...\n");
    long events = 0;
    for (int i = 0; i < 100; i++) {
        uint64_t c = *cons, p = *prod;
        while (p > c) {
            uint32_t *lp = (uint32_t *)(data + (c & RINGBUF_MASK));
            uint32_t len = *lp;
            if (len & BUSY_BIT) break;
            uint32_t dlen = len & ~(BUSY_BIT | DISCARD_BIT);
            if (dlen == 0) break;  /* 防御：len=0 会死循环 */
            uint32_t step = (dlen + 7u) & ~7u;
            if (!(len & DISCARD_BIT) && dlen >= HDR_SZ + 16) {
                uint64_t *rec = (uint64_t *)((char *)lp + HDR_SZ);
                if (events < 6)
                    printf("       事件#%ld: pid_tgid=0x%llx x0=0x%llx\n", events + 1,
                           (unsigned long long)rec[0], (unsigned long long)rec[1]);
                events++;
            }
            c += step;
        }
        *cons = c;
        if (events >= 20) break;
        usleep(100000);
    }
    printf("       ringbuf 事件数=%ld\n", events);
    printf("       ---- 各探针触发计数 ----\n");
    for (int i = 0; i < np; i++) {
        uint64_t cnt = 0;
        ssize_t rd = read(probes[i].fd, &cnt, sizeof(cnt));
        printf("       计数=%-8llu %s (rc=%zd)\n", (unsigned long long)cnt, probes[i].name, rd);
    }

    for (int i = 0; i < np; i++) { ioctl(probes[i].fd, IOC_DISABLE, 0); close(probes[i].fd); }
    close(prog_fd); close(map_fd);
    printf("=== 汇总：%d 通过，%d 失败 ===\n", g_ok, g_fail);
    return 0;
}
