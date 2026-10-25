/*
 * 一个调度 · eBPF 能力探针（诊断工具，不进模块 zip——tools/ 被 pack.ps1 排除）
 * ============================================================
 * 用途：判定设备内核的 eBPF 能力边界，定位 fas-rs（frame-analyzer eBPF 版）
 * 启动失败的病灶层级。fas-rs 的真实链路（frame-analyzer 0.2.6 源码）：
 *   1) load_bpf()      aya Bpf::load(内嵌 eBPF ELF)：建 map + BPF_PROG_LOAD
 *   2) program.load()  程序类型 BPF_PROG_TYPE_KPROBE
 *   3) program.attach() uprobe 挂 /system/lib64/libgui.so 的
 *      _ZN7android7Surface11queueBufferEP19ANativeWindowBufferi（目标 pid）
 * 本探针按同一链路逐层测试，任一层失败即定位病灶。
 *
 * 构建（NDK clang，arm64，动态链接——dlopen 测试需要）：
 *   <ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android26-clang.cmd \
 *       -O2 tools/diag/ebpf_probe.c -o tools/diag/ebpf_probe
 * 运行（设备 root）：
 *   adb push tools/diag/ebpf_probe /data/local/tmp/
 *   adb shell su -c "/data/local/tmp/ebpf_probe"
 * ============================================================
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/bpf.h>
#include <linux/perf_event.h>

static int g_ok = 0;
static int g_fail = 0;

static void report(const char *name, int ok, int err) {
    if (ok) {
        printf("[ OK ] %s\n", name);
        g_ok++;
    } else {
        printf("[FAIL] %-44s errno=%d (%s)\n", name, err, strerror(err));
        g_fail++;
    }
}

static int bpf_cmd(enum bpf_cmd cmd, union bpf_attr *attr) {
    return (int)syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}

/* 最小合法 eBPF 程序：r0 = 0; exit——不碰上下文，无需 BTF。
 * 手写操作码而非 BPF_MOV64_IMM/BPF_EXIT_INSN 宏（在 <linux/filter.h>，
 * NDK sysroot 不一定带）；位置初始化避开 dst_reg/dst_regs 字段命名差异：
 *   { code, dst_reg, src_reg, off, imm }
 *   0xb7 = BPF_ALU64|BPF_MOV|BPF_K（mov64 r0, imm）
 *   0x95 = BPF_JMP|BPF_EXIT（exit）
 */
static struct bpf_insn s_trivial[] = {
    { 0xb7, 0, 0, 0, 0 },
    { 0x95, 0, 0, 0, 0 },
};

/* ---- 1) BTF 文件可读性 ---- */
static void test_btf_file(void) {
    struct stat st;
    if (stat("/sys/kernel/btf/vmlinux", &st) != 0) {
        report("/sys/kernel/btf/vmlinux 存在", 0, errno);
        return;
    }
    printf("[ OK ] /sys/kernel/btf/vmlinux 存在（%lld 字节）\n", (long long)st.st_size);
    g_ok++;
}

/* ---- 2) bpf() 系统调用：建 map ---- */
static void test_map_create(const char *label, enum bpf_map_type type) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = type;
    attr.key_size = 4;
    attr.value_size = 8;
    attr.max_entries = 4;
    int fd = bpf_cmd(BPF_MAP_CREATE, &attr);
    char name[96];
    snprintf(name, sizeof(name), "bpf MAP_CREATE %s", label);
    report(name, fd >= 0, errno);
    if (fd >= 0) close(fd);
}

/* ---- 3) bpf() 系统调用：加载程序（指定类型） ---- */
static void test_prog_load(const char *label, enum bpf_prog_type type) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = type;
    attr.insns = (uintptr_t)s_trivial;
    attr.insn_cnt = 2;
    attr.license = (uintptr_t)"GPL";
    int fd = bpf_cmd(BPF_PROG_LOAD, &attr);
    char name[96];
    snprintf(name, sizeof(name), "bpf PROG_LOAD %s", label);
    report(name, fd >= 0, errno);
    if (fd >= 0) close(fd);
}

/* ---- 4) bpf() 系统调用：BTF 加载（aya 加载程序 BTF 走这条） ---- */
static void test_btf_load(void) {
    /* 最小合法 BTF：24 字节头 + 1 字节空字符串节（无类型） */
    static unsigned char blob[] = {
        0x9F, 0xEB,             /* magic = 0xeB9F（小端） */
        0x01,                   /* version = 1 */
        0x00,                   /* flags = 0 */
        0x18, 0x00, 0x00, 0x00, /* hdr_len = 24 */
        0x00, 0x00, 0x00, 0x00, /* type_off = 0 */
        0x00, 0x00, 0x00, 0x00, /* type_len = 0 */
        0x00, 0x00, 0x00, 0x00, /* str_off = 0 */
        0x01, 0x00, 0x00, 0x00, /* str_len = 1 */
        0x00,                   /* 字符串节：仅 NUL */
    };
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.btf = (uintptr_t)blob;
    attr.btf_size = sizeof(blob);
    int fd = bpf_cmd(BPF_BTF_LOAD, &attr);
    report("bpf BTF_LOAD（最小 BTF）", fd >= 0, errno);
    if (fd >= 0) close(fd);
}

/* ---- 5) uprobe PMU 存在性（aya attach 的 perf_event_open 目标） ---- */
static void test_uprobe_pmu(void) {
    FILE *f = fopen("/sys/bus/event_source/devices/uprobe/type", "r");
    if (!f) {
        report("uprobe PMU 存在", 0, errno);
        return;
    }
    int type = -1;
    if (fscanf(f, "%d", &type) != 1) type = -1;
    fclose(f);
    if (type < 0) {
        report("uprobe PMU 存在（type 不可读）", 0, EIO);
        return;
    }
    printf("[ OK ] uprobe PMU 存在（type=%d）\n", type);
    g_ok++;
}

/* ---- 6) tracefs uprobe 基础设施（创建+删除探测点） ---- */
static void test_tracefs_uprobe(void) {
    const char *dir = "/sys/kernel/debug/tracing";
    if (access(dir, W_OK) != 0) {
        report("tracefs 可写（/sys/kernel/debug/tracing）", 0, errno);
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/uprobe_events", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        report("uprobe_events 可写", 0, errno);
        return;
    }
    /* p:EVENT PATH:OFFSET——offset 0（ELF 头，永不执行，无副作用） */
    int rc = fprintf(f, "p:ebpf_probe_test /system/lib64/libgui.so:0x0");
    if (fclose(f) != 0 || rc < 0) {
        report("uprobe_events 创建探测点", 0, errno);
        return;
    }
    /* 清理：删除探测点 */
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "-:ebpf_probe_test");
        fclose(f);
    }
    report("uprobe_events 创建+删除探测点", 1, 0);
}

/* ---- 7) fas-rs 的目标符号是否在 libgui.so 中（dlsym 代理检查） ---- */
static void test_symbol(void) {
    const char *sym = "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi";
    void *h = dlopen("/system/lib64/libgui.so", RTLD_NOW);
    if (!h) {
        report("dlopen libgui.so", 0, errno);
        return;
    }
    dlerror();
    void *p = dlsym(h, sym);
    const char *e = dlerror();
    printf("[%s] libgui.so 符号 %s\n", p ? " OK " : "FAIL", sym);
    if (p) {
        printf("       （地址 %p，dlsym 可见；.symtab 内的符号此方法查不到，需主机侧 nm 复核）\n", p);
        g_ok++;
    } else {
        printf("       （%s）\n", e ? e : "not found");
        g_fail++;
    }
    dlclose(h);
}

int main(void) {
    printf("=== eBPF 能力探针（pid=%d uid=%d）===\n", getpid(), getuid());

    test_btf_file();
    printf("-- bpf() 系统调用层 --\n");
    test_map_create("ARRAY", BPF_MAP_TYPE_ARRAY);
    test_map_create("RINGBUF（fas-rs 用）", BPF_MAP_TYPE_RINGBUF);
    test_prog_load("SOCKET_FILTER（无 BTF）", BPF_PROG_TYPE_SOCKET_FILTER);
    test_prog_load("KPROBE（fas-rs 用）", BPF_PROG_TYPE_KPROBE);
    test_btf_load();
    printf("-- uprobe 附着层 --\n");
    test_uprobe_pmu();
    test_tracefs_uprobe();
    printf("-- fas-rs 目标符号层 --\n");
    test_symbol();

    printf("=== 汇总：%d 项通过，%d 项失败 ===\n", g_ok, g_fail);
    return g_fail > 0 ? 1 : 0;
}
