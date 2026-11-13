/*
 * [R61] BepInEx6 IL2CPP 原生 detour 桥接 shim
 * --------------------------------------------------
 * BepInEx6 的 BepInEx.Unity.IL2CPP.Hook.Dobby.DobbyDetour 走「两段式」API：
 *     DobbyPrepare(target, replace, &prepared) -> 分配 trampoline（prepared = 原函数指针）
 *     DobbyCommit(prepared)                    -> 真正把 detour 写入内存
 *     DobbyDestroy(address)                    -> 撤销
 * 但本仓库静态链的是上游 jmpews/dobby，它只有一次性 DobbyHook（prepare+commit 一起完成），
 * 根本没有 DobbyPrepare / DobbyCommit 这两个符号。于是 IL2CPPChainloader.Initialize
 * 在 DobbyDetour.PrepareImpl() 里 P/Invoke "DobbyPrepare" 时抛：
 *     System.EntryPointNotFoundException: DobbyPrepare
 * BepInEx 随即整个退出，游戏直接进原版、NOS 不加载（真机 preloader.log 实锤）。
 *
 * 语义对齐（BepInEx 调用顺序 = Prepare 紧跟 Commit，不会在中间调用 target）：
 *   - DobbyPrepare：直接 DobbyHook，hook 在此时写好；prepared 填 trampoline（调原函数用）。
 *   - DobbyCommit ：hook 已在 Prepare 写完，这里为空操作，返回 0（成功）。
 *   - DobbyDestroy：上游 dobby 已有，不再重复定义。
 *
 * ============================================================================
 * [R95f] 方向纠偏：删掉 R95e 的 arm64e/PAC 整套误判，改成基于反汇编实证的修法
 * ----------------------------------------------------------------------------
 * 用户实锤：AU = Among Us，是 arm64 上架包（Apple 不向第三方开放 arm64e ABI），
 *           NOS = Nebula on the Ship（模组）。设备上不存在 PAC 签名指针，
 *           也不存在 PACIASP / BTI / RETAA 指令。R95e 的 strip_pac() 与
 *           arm64e 序言白名单纯属无的放矢（永不命中），本版全部删除。
 *
 * 真机（R95d, BUILD=1e3e96df3587）崩溃的反汇编实证：
 *   日志 "Memory around native instruction pointer (0x107300cf8)" 首 4 字节
 *     17 01 40 b9  ->  0xB9400117  =  ldr w23, [x8]
 *   与 libdobby.a 内 InstructionRelocationARM64.cc.o 的
 *     relo_relocate +0x19c (0x1f4): b9400117  ldr w23, [x8]
 *   完全一致；紧邻的 0x1f0 为 f9400a68  ldr x8, [x19, #0x10]，
 *   即 x8 = ctx->buffer_cursor —— dobby 正在【读取被 hook 函数开头的 ARM64 指令】
 *   （GenRelocateCode 里 ctx.buffer = ctx.buffer_cursor = 目标函数地址）。
 *   调用链：DobbyPrepare -> DobbyHook -> InterceptRouting::GenerateRelocatedCode
 *           -> GenRelocateCodeAndBranch -> GenRelocateCode -> relo_relocate。
 *   由于 0x144 处的 GenRelocateCodeAndBranch 早于 0x348 处的 DobbyCodePatch，
 *   崩点在【写内存之前】，target 尚未被改坏，降级是安全的。
 *
 *   结论：SIGSEGV 说明 target 指向的内存不可读（NULL / 低 4GB 野值 / 未映射页 /
 *   Il2CppInterop 没拿到 MethodPointer 的 stub），或 dobby 内部重定位状态异常。
 *   R95e 用 vm_read 读到了这个事实却只做 hexdump 打印、不参与判定，仍旧放行
 *   DobbyHook，所以必崩 —— 这是 R95e 的致命缺陷。
 *
 * 本版做法（只改这一个 C 文件，保持单 dylib）：
 *   1) vm_read 从「纯诊断」提升为【硬判定】：读不到 target 前 32 字节（覆盖
 *      dobby origin->size 可能值）或 replace 前 8 字节，就直接降级；
 *      跨页时按页分段读，避免 "靠近页末 -> 误判不可读"。
 *   2) dladdr 降级为纯信息打印，不再当拒绝条件 —— JIT 生成的合法代码本就不在
 *      任何 Mach-O image 内，R95e 的 in_image 硬拒绝会误伤合法 patch。
 *   3) 低 4GB / 4 字节未对齐仍拦；**删除全部 PAC 清高位、arm64e 白名单**。
 *   4) 最后一道保险：用 sigsetjmp/siglongjmp + 临时 SIGSEGV/SIGBUS handler
 *      包住 DobbyHook。即便判断漏了、dobby 内部仍崩，也当场捕获并降级该
 *      patch，进程绝不崩；崩一次后熔断（dobby 可能留下半初始化状态），
 *      后续 patch 全降级。handler 退出窗口时转发给 mono 原有 handler，
 *      不破坏托管 NullReferenceException 转换。
 *   5) 降级一律 *prepared = target（trampoline = 原函数）。DetourTo 会把
 *      *prepared 写进 modifiedNativeMethodInfo.MethodPointer，留 0 会跳 NULL 崩。
 *   6) 逐 patch 打印序号 / target / replace / dladdr 归属 / 前 32 字节 hexdump /
 *      判定结果，每行 fflush。一次真机即可定位确切坏地址，下一版精准修。
 *
 * 结果：崩不了；能装的 patch 照常生效；日志给出所有判定依据。
 */
/* Linux 本地语法检查需要 _GNU_SOURCE 才暴露 Dl_info/dladdr；Darwin 无条件提供。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>               /* dladdr：仅用于打印归属 image，不参与判定 */
#include <setjmp.h>
#include <signal.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 上游 jmpews/dobby 的真实签名（dobby_dummy_func_t == void*）。 */
int DobbyHook(void *address, void *replace_func, void **origin_func);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------------------------------------------
 * 安全读：跨页按页分段 vm_read，绝不直接 deref。读满 n 字节才返回 1。
 * 非 __APPLE__ 分支仅用于本地 gcc 语法检查，不参与 iOS 构建。
 * ------------------------------------------------------------------------- */
static int r95f_safe_read(void *addr, unsigned char *out, size_t n) {
    if (!addr || !n) return 0;
#ifdef __APPLE__
    long ps = getpagesize();
    if (ps <= 0) return 0;
    size_t done = 0;
    while (done < n) {
        uintptr_t cur = (uintptr_t)addr + done;
        uintptr_t page = cur & ~(uintptr_t)(ps - 1);
        size_t off = (size_t)(cur - page);
        size_t want = n - done;
        size_t room = (size_t)ps - off;
        if (want > room) want = room;

        vm_offset_t data = 0;
        mach_msg_type_number_t cnt = 0;
        kern_return_t kr = vm_read(mach_task_self(), (vm_address_t)page,
                                   (vm_size_t)(off + want), &data, &cnt);
        if (kr != KERN_SUCCESS || cnt < (mach_msg_type_number_t)(off + want)) {
            if (data) vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
            return 0;
        }
        memcpy(out + done, (const void *)(uintptr_t)(data + off), want);
        vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
        done += want;
    }
    return 1;
#else
    memcpy(out, addr, n);
    return 1;
#endif
}

/* dladdr 只作取证打印。返回 image 名；不在任何 image 内返回 "(jit/none)"。 */
static const char *r95f_image_of(void *p) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (p && dladdr(p, &info) != 0 && info.dli_fname) return info.dli_fname;
    return "(jit/none)";
}

/* ---------------------------------------------------------------------------
 * SIGSEGV / SIGBUS 保护窗口
 * ------------------------------------------------------------------------- */
static sigjmp_buf   g_r95f_jmp;
static volatile sig_atomic_t g_r95f_armed = 0;
static struct sigaction g_r95f_old_segv, g_r95f_old_bus;

static void r95f_crash_handler(int sig, siginfo_t *si, void *uc) {
    if (g_r95f_armed) {
        g_r95f_armed = 0;
        siglongjmp(g_r95f_jmp, 1);
    }
    /* 不在保护窗口内：原样转发给 mono 自己装的 handler，不吞异常 */
    const struct sigaction *old = (sig == SIGBUS) ? &g_r95f_old_bus : &g_r95f_old_segv;
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) old->sa_sigaction(sig, si, uc);
    } else if (old->sa_handler == SIG_DFL) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, NULL);
        raise(sig);
    } else if (old->sa_handler && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    }
}

static void r95f_guard_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = r95f_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_r95f_old_segv);
    sigaction(SIGBUS,  &sa, &g_r95f_old_bus);
}

static void r95f_guard_restore(void) {
    g_r95f_armed = 0;
    sigaction(SIGSEGV, &g_r95f_old_segv, NULL);
    sigaction(SIGBUS,  &g_r95f_old_bus,  NULL);
}

__attribute__((visibility("default")))
int DobbyPrepare(void *target, void *replace, void **prepared) {
    static unsigned seq = 0;
    static int tripped = 0;      /* dobby 内部崩过一次 -> 熔断，后续全降级 */
    seq++;

    if (seq == 1)
        printf("[host][r95f] ===== R95f：arm64 实证版（删 PAC 误判 / vm_read 硬判定 / SIGSEGV 兜底）=====\n");

    unsigned long long t = (unsigned long long)(uintptr_t)target;
    unsigned long long r = (unsigned long long)(uintptr_t)replace;

    /* 绝对兜底：trampoline = 原函数（不 patch 也不崩）。 */
    if (prepared) *prepared = target;

    /* —— 取证：安全读 target 32B / replace 8B —— */
    unsigned char head[32], rhead[8];
    memset(head, 0, sizeof(head));
    memset(rhead, 0, sizeof(rhead));
    int got_target  = r95f_safe_read(target,  head,  sizeof(head));
    int got_replace = r95f_safe_read(replace, rhead, sizeof(rhead));

    const char *tn = r95f_image_of(target);
    const char *rn = r95f_image_of(replace);

    printf("[r95f][dob] #%u target=0x%llx [%s] replace=0x%llx [%s] "
           "readT=%d readR=%d\n",
           seq, t, tn, r, rn, got_target, got_replace);
    fflush(stdout);

    if (got_target) {
        printf("[r95f][dob] #%u head=%02x%02x%02x%02x %02x%02x%02x%02x "
               "%02x%02x%02x%02x %02x%02x%02x%02x "
               "%02x%02x%02x%02x %02x%02x%02x%02x "
               "%02x%02x%02x%02x %02x%02x%02x%02x\n",
               seq,
               head[0],  head[1],  head[2],  head[3],  head[4],  head[5],  head[6],  head[7],
               head[8],  head[9],  head[10], head[11], head[12], head[13], head[14], head[15],
               head[16], head[17], head[18], head[19], head[20], head[21], head[22], head[23],
               head[24], head[25], head[26], head[27], head[28], head[29], head[30], head[31]);
    } else {
        printf("[r95f][dob] #%u head=<vm_read 失败：目标地址不可读>\n", seq);
    }
    fflush(stdout);

    /* —— 硬判定（不再有 PAC 清高位、不再有 arm64e 白名单）—— */
    int nullish  = (t < 0x100000000ull);            /* NULL / 低 4GB 非法区 */
    int misalign = ((t & 3ull) != 0);               /* ARM64 指令须 4 字节对齐 */
    int bad = (!got_target || !got_replace || nullish || misalign);

    if (tripped) {
        printf("[r95f][dob] #%u 熔断降级：dobby 已崩过一次，后续 patch 全部静默失效（进程不崩）\n", seq);
        fflush(stdout);
        return 0;
    }
    if (bad) {
        printf("[r95f][dob] #%u 降级：readT=%d readR=%d nullish=%d misalign=%d "
               "-> 不调 DobbyHook，*prepared=原函数\n",
               seq, got_target, got_replace, nullish, misalign);
        fflush(stdout);
        return 0;   /* 返回码被 IL pop 丢弃；*prepared 已填好 */
    }

    /* —— 最后一道保险：把 DobbyHook 包在信号保护窗口里 —— */
    r95f_guard_install();
    int rc;
    if (sigsetjmp(g_r95f_jmp, 1) == 0) {
        g_r95f_armed = 1;
        rc = DobbyHook(target, replace, prepared);
        r95f_guard_restore();
        if (rc != 0) {
            printf("[r95f][dob] #%u DobbyHook rc=%d（失败）-> 兜底 *prepared=原函数\n", seq, rc);
            fflush(stdout);
            if (prepared) *prepared = target;
        } else if (prepared && *prepared == NULL) {
            printf("[r95f][dob] #%u DobbyHook 成功但 trampoline=NULL -> 兜底 *prepared=原函数\n", seq);
            fflush(stdout);
            *prepared = target;
        }
        return rc;
    }

    /* siglongjmp 回来：dobby 内部崩了 */
    r95f_guard_restore();
    tripped = 1;
    if (prepared) *prepared = target;
    printf("[r95f][dob] #%u !! DobbyHook 内部 SIGSEGV/SIGBUS（target=0x%llx [%s] "
           "head=%02x%02x%02x%02x）-> 该 patch 降级并熔断，进程不崩\n",
           seq, t, tn, head[0], head[1], head[2], head[3]);
    fflush(stdout);
    return 0;
}

__attribute__((visibility("default")))
int DobbyCommit(void *prepared) {
    (void)prepared;
    return 0;
}
