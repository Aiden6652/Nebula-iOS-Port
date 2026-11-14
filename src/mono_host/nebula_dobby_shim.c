/*
 * [R61] BepInEx6 IL2CPP 原生 detour 桥接 shim
 * --------------------------------------------------
 * BepInEx6 的 BepInEx.Unity.IL2CPP.Hook.Dobby.DobbyDetour 走「两段式」API：
 *     DobbyPrepare(target, replace, &prepared) -> 分配 trampoline
 *     DobbyCommit(prepared)                    -> 真正把 detour 写入内存
 *     DobbyDestroy(address)                    -> 撤销
 * 但本仓库静态链的是上游 jmpews/dobby，只有一次性 DobbyHook，
 * 没有 DobbyPrepare / DobbyCommit 这两个符号，故由本 shim 包出。
 *
 * ============================================================================
 * [R96] 修掉 R95f 真机暴露的两个根因，目标：NOS 完成 PatchAll 并加载
 * ----------------------------------------------------------------------------
 * R95f 真机日志（BUILD=f2122022399c）native stack：
 *     libsystem_platform(memcpy) <- DobbyCodePatch <- DobbyDestroy
 * managed: DobbyDetour.UndoImpl -> DobbyLib.Destroy(OriginalMethodPtr)
 *          <- Undo <- Dispose <- Il2CppDetourMethodPatcher.DetourTo <- PatchAll
 *
 * 根因 1（致命）：本 shim 只接管 DobbyPrepare / DobbyCommit，没接管
 *   DobbyDestroy。ikdasm 反编译 BepInEx.Unity.IL2CPP.dll 实锤：
 *     .method UndoImpl(): ldarg.0; call get_OriginalMethodPtr();
 *       call int32 DobbyLib::Destroy(native int)   // [DllImport("dobby")] "DobbyDestroy"
 *   即 Destroy 的参数就是 target 地址，不是 handle。它命中 -force_load
 *   全量链进来的上游 dobby.cpp.o::_DobbyDestroy，对「从未真正 hook 过」
 *   的地址（R95f 降级产生的假 handle）或「已 hook 但 iOS 代码页不可写回」
 *   的地址做 DobbyCodePatch(memcpy) -> SIGSEGV。
 *   本版：接管 DobbyDestroy 为 no-op（游戏生命周期内 Harmony 不真撤销
 *   detour），并让 build_ios.sh 把 dobby 归档展开成裸 .o 列表、排除
 *   dobby.cpp.o，避免与上游强符号重复定义。llvm-nm 全量核验过归档内
 *   没有任何其它 .o 引用 _DobbyDestroy / _DobbyGetVersion，排除安全。
 *
 * 根因 2：R95f 的 vm_read 按 16KB 页首对齐、从 page 起始读 (off+want)，
 *   跨到相邻 region 时把合法地址误判为不可读。真机 #35 0x1159540f0
 *   (readT=1) 与 #36 0x115955d60 (readT=0) 相差仅 7KB，都在
 *   UnityFramework __TEXT、dladdr 都能解析 —— 34/55 个合法 patch 被错杀。
 *   本版直接从 addr 读 n 字节（Mach vm_read 的 address 参数无需页对齐，
 *   内核自行处理跨页），消除假阴性。
 *
 * 附带：DobbyDestroy 变 no-op 后，Harmony 重打同方法时不会真撤销，
 *   Prepare 可能对同一 target 二次 DobbyHook。加「已 hook 地址表」，
 *   重复 target 直接复用首次生成的 trampoline，不二次 hook。
 *   保留 R95f 的 SIGSEGV/SIGBUS 保护窗口 + 熔断，防止 dobby 内部
 *   relo_relocate 之类崩溃拖垮进程。
 *
 * 架构前提（用户实锤，R95e 的 PAC 误判已彻底删除）：
 *   AU = Among Us 是 arm64 上架包，进程内不存在 PAC / PACIASP / BTI / RETAA。
 *   iPad M4 芯片兼容 arm64e 不代表本进程有 PAC 签名指针。
 *   NOS = Nebula on the Ship。
 *
 * 保持单 dylib：只改这个 C 文件 + build_ios.sh 的 dobby 链接方式。
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
 * 安全读：直接从 addr 读 n 字节，绝不直接 deref。
 * [R96] 不再按 16KB 页首对齐、不再从 page 起始读 (off+want)——
 *        Mach vm_read 的 address 参数无需页对齐，内核会处理跨页；
 *        从页首读一大块反而会跨到相邻不可读 region，造成假阴性。
 * 非 __APPLE__ 分支仅用于本地 gcc 语法检查，不参与 iOS 构建。
 * ------------------------------------------------------------------------- */
static int r96_safe_read(void *addr, unsigned char *out, size_t n) {
    if (!addr || !n) return 0;
#ifdef __APPLE__
    vm_offset_t data = 0;
    mach_msg_type_number_t cnt = 0;
    kern_return_t kr = vm_read(mach_task_self(),
                               (vm_address_t)(uintptr_t)addr,
                               (vm_size_t)n, &data, &cnt);
    if (kr != KERN_SUCCESS || !data || cnt < (mach_msg_type_number_t)n) {
        if (data) vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
        return 0;
    }
    memcpy(out, (const void *)(uintptr_t)data, n);
    vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
    return 1;
#else
    memcpy(out, addr, n);
    return 1;
#endif
}

/* dladdr 只作取证打印。返回 image 名；不在任何 image 内返回 "(jit/none)"。 */
static const char *r96_image_of(void *p) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (p && dladdr(p, &info) != 0 && info.dli_fname) return info.dli_fname;
    return "(jit/none)";
}

/* ---------------------------------------------------------------------------
 * SIGSEGV / SIGBUS 保护窗口
 * ------------------------------------------------------------------------- */
static sigjmp_buf              g_r96_jmp;
static volatile sig_atomic_t  g_r96_armed = 0;
static struct sigaction       g_r96_old_segv, g_r96_old_bus;

static void r96_crash_handler(int sig, siginfo_t *si, void *uc) {
    if (g_r96_armed) {
        g_r96_armed = 0;
        siglongjmp(g_r96_jmp, 1);
    }
    /* 不在保护窗口内：原样转发给 mono 自己装的 handler，不吞异常 */
    const struct sigaction *old = (sig == SIGBUS) ? &g_r96_old_bus : &g_r96_old_segv;
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

static void r96_guard_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = r96_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_r96_old_segv);
    sigaction(SIGBUS,  &sa, &g_r96_old_bus);
}

static void r96_guard_restore(void) {
    g_r96_armed = 0;
    sigaction(SIGSEGV, &g_r96_old_segv, NULL);
    sigaction(SIGBUS,  &g_r96_old_bus,  NULL);
}

/* ---------------------------------------------------------------------------
 * [R96] 已 hook 地址表：DobbyDestroy 变 no-op 后，Harmony 重打同一方法
 *   时不会真撤销，Prepare 可能收到重复 target。复用首次 trampoline，
 *   避免对同一地址二次 DobbyHook（上游行为不确定，且 iOS 代码页反复写危险）。
 * ------------------------------------------------------------------------- */
#define R96_MAX_HOOKS 1024
typedef struct { void *target; void *trampoline; } r96_hook_t;
static r96_hook_t g_r96_hooks[R96_MAX_HOOKS];
static unsigned   g_r96_hook_n = 0;

static int r96_lookup(void *target, void **trampoline) {
    for (unsigned i = 0; i < g_r96_hook_n; i++) {
        if (g_r96_hooks[i].target == target) {
            *trampoline = g_r96_hooks[i].trampoline;
            return 1;
        }
    }
    return 0;
}

static void r96_remember(void *target, void *trampoline) {
    if (g_r96_hook_n < R96_MAX_HOOKS) {
        g_r96_hooks[g_r96_hook_n].target     = target;
        g_r96_hooks[g_r96_hook_n].trampoline = trampoline;
        g_r96_hook_n++;
    }
}

/* ---------------------------------------------------------------------------
 * DobbyPrepare：核心判定 + 真 hook（带信号兜底）
 * ------------------------------------------------------------------------- */
__attribute__((visibility("default")))
int DobbyPrepare(void *target, void *replace, void **prepared) {
    static unsigned seq = 0;
    static int tripped = 0;      /* dobby 内部崩过一次 -> 熔断，后续全降级 */
    seq++;

    if (seq == 1)
        printf("[host][r96] ===== R96：接管 DobbyDestroy(no-op) / 修 vm_read 页对齐误判 / 复用 trampoline =====\n");

    unsigned long long t = (unsigned long long)(uintptr_t)target;
    unsigned long long r = (unsigned long long)(uintptr_t)replace;

    /* 绝对兜底：trampoline = 原函数（不 patch 也不崩）。 */
    if (prepared) *prepared = target;

    /* —— 重复 target：复用首次 trampoline，不二次 hook —— */
    void *saved_tramp = NULL;
    if (r96_lookup(target, &saved_tramp)) {
        if (prepared) *prepared = saved_tramp;
        printf("[r96][dob] #%u target=0x%llx 复用 trampoline=0x%llx（Destroy 已 no-op，不二次 hook）\n",
               seq, t, (unsigned long long)(uintptr_t)saved_tramp);
        fflush(stdout);
        return 0;
    }

    /* —— 取证：安全读 target 32B / replace 8B —— */
    unsigned char head[32], rhead[8];
    memset(head, 0, sizeof(head));
    memset(rhead, 0, sizeof(rhead));
    int got_target  = r96_safe_read(target,  head,  sizeof(head));
    int got_replace = r96_safe_read(replace, rhead, sizeof(rhead));

    const char *tn = r96_image_of(target);
    const char *rn = r96_image_of(replace);

    printf("[r96][dob] #%u target=0x%llx [%s] replace=0x%llx [%s] "
           "readT=%d readR=%d\n",
           seq, t, tn, r, rn, got_target, got_replace);
    fflush(stdout);

    if (got_target) {
        printf("[r96][dob] #%u head=%02x%02x%02x%02x %02x%02x%02x%02x "
               "%02x%02x%02x%02x %02x%02x%02x%02x "
               "%02x%02x%02x%02x %02x%02x%02x%02x "
               "%02x%02x%02x%02x %02x%02x%02x%02x\n",
               seq,
               head[0],  head[1],  head[2],  head[3],  head[4],  head[5],  head[6],  head[7],
               head[8],  head[9],  head[10], head[11], head[12], head[13], head[14], head[15],
               head[16], head[17], head[18], head[19], head[20], head[21], head[22], head[23],
               head[24], head[25], head[26], head[27], head[28], head[29], head[30], head[31]);
    } else {
        printf("[r96][dob] #%u head=<vm_read 失败：目标地址不可读>\n", seq);
    }
    fflush(stdout);

    /* —— 判定（无 PAC 清高位、无 arm64e 白名单）——
     * [R96] R95f 教训：vm_read 硬拒绝错杀了 34/55 个合法 patch，它们的 target
     *   全部 dladdr 命中 UnityFramework __TEXT。iOS 上 __TEXT 代码页的
     *   vm_read 存在保护/对齐层面的假阴性，不能作为唯一判据。
     *   合法 = 不 nullish、不 misalign，且【可读 或 命中已知 Mach-O image】。
     *   replace 是 Harmony/JIT 生成的委托指针，不 deref，仅打印不做拒绝条件。
     *   万一放行到坏地址，下面还有 SIGSEGV 兜底 + 熔断保底，进程绝不崩。
     */
    int nullish   = (t < 0x100000000ull);           /* NULL / 低 4GB 非法区 */
    int misalign  = ((t & 3ull) != 0);              /* ARM64 指令须 4 字节对齐 */
    int in_image  = (strcmp(tn, "(jit/none)") != 0);/* target 命中已知 Mach-O image */
    int addr_ok   = (got_target || in_image);
    int bad       = (nullish || misalign || !addr_ok);

    if (tripped) {
        printf("[r96][dob] #%u 熔断降级：dobby 已崩过一次，后续 patch 全部静默失效（进程不崩）\n", seq);
        fflush(stdout);
        return 0;
    }
    if (bad) {
        printf("[r96][dob] #%u 降级：readT=%d readR=%d in_image=%d nullish=%d misalign=%d "
               "-> 不调 DobbyHook，*prepared=原函数\n",
               seq, got_target, got_replace, in_image, nullish, misalign);
        fflush(stdout);
        return 0;
    }
    if (!got_target) {
        printf("[r96][dob] #%u 放行：vm_read 假阴性但 dladdr 命中 [%s]，仍尝试 hook（信号兜底）\n",
               seq, tn);
        fflush(stdout);
    }

    /* —— 最后一道保险：把 DobbyHook 包在信号保护窗口里 —— */
    r96_guard_install();
    int rc;
    if (sigsetjmp(g_r96_jmp, 1) == 0) {
        g_r96_armed = 1;
        rc = DobbyHook(target, replace, prepared);
        r96_guard_restore();
        if (rc != 0) {
            printf("[r96][dob] #%u DobbyHook rc=%d（失败）-> 兜底 *prepared=原函数\n", seq, rc);
            fflush(stdout);
            if (prepared) *prepared = target;
        } else if (prepared && *prepared == NULL) {
            printf("[r96][dob] #%u DobbyHook 成功但 trampoline=NULL -> 兜底 *prepared=原函数\n", seq);
            fflush(stdout);
            *prepared = target;
        } else {
            r96_remember(target, prepared ? *prepared : NULL);
            printf("[r96][dob] #%u DobbyHook 成功 trampoline=0x%llx（已登记，共 %u 个）\n",
                   seq, (unsigned long long)(uintptr_t)(prepared ? *prepared : NULL), g_r96_hook_n);
            fflush(stdout);
        }
        return rc;
    }

    /* siglongjmp 回来：dobby 内部崩了 */
    r96_guard_restore();
    tripped = 1;
    if (prepared) *prepared = target;
    printf("[r96][dob] #%u !! DobbyHook 内部 SIGSEGV/SIGBUS（target=0x%llx [%s] "
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

/* ---------------------------------------------------------------------------
 * [R96] 接管 DobbyDestroy —— 这是 R95f 崩溃的直接修法。
 * BepInEx DobbyDetour.UndoImpl 会 P/Invoke Destroy(OriginalMethodPtr)，
 * 上游实现会 DobbyCodePatch(memcpy) 写回代码页，在 iOS 上必崩
 * （假 handle 找不到 entry / 代码签名页不可写）。游戏进程生命周期内
 * Harmony 不需要真撤销 detour，no-op 是正确且唯一的稳妥语义。
 * ------------------------------------------------------------------------- */
__attribute__((visibility("default")))
int DobbyDestroy(void *address) {
    static unsigned dseq = 0;
    dseq++;
    printf("[r96][destroy] #%u address=0x%llx -> no-op（不写回代码页，进程内不撤销 detour）\n",
           dseq, (unsigned long long)(uintptr_t)address);
    fflush(stdout);
    return 0;   /* 0 = 成功，BepInEx UndoImpl 直接 pop 丢弃 */
}
