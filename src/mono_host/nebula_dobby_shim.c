/*
 * [R110] BepInEx6 IL2CPP 原生 detour 桥接 shim —— 干净版
 * ------------------------------------------------------------
 * BepInEx6 的 DobbyDetour 走两段式 API：
 *     DobbyPrepare(target, replace, &prepared) -> 分配 trampoline
 *     DobbyCommit(prepared)                    -> 提交（上游 hook 已在 Prepare 写完）
 *     DobbyDestroy(address)                    -> 撤销
 * 上游 jmpews/dobby 只有一次性 DobbyHook，没有 Prepare/Commit，故由本 shim 包出。
 *
 * 【R110 架构变更：根治 execute-only SIGSEGV，不再在 shim 打权限补丁】
 * R99~R109 在 shim 层反复 vm_protect 改页权限（COW/影子/fishhook/set_maximum），
 * 十一轮全被真机证伪 —— iOS code-signed __TEXT_EXEC 是 APRR 硬件级 execute-only，
 * shim 层无法给它加 CPU 数据读权限。代码已堆成屎山，R110 全部作废。
 *
 * 根治改在 Dobby 源码（src/mono_host/dobby/patches/，由 rebuild_dobby_ios.sh 重编）：
 *   - InstructionRelocationARM64.cc: relo 读源 CPU ldr -> vm_read（能读 execute-only）
 *   - code-patch-tool-darwin.cc:    写跳板 vm_protect 翻页 -> vm_write（不钉死 maximum）
 * 于是所有 target（含 execute-only 页）都能真正 hook，本 shim 回归薄封装：
 *   零降级、零跳过，每个 target 都真调 DobbyHook。
 *
 * DobbyDestroy 仍 no-op：游戏进程生命周期内 Harmony 不需要真撤销 detour，
 * 且上游 Destroy 会走 CodePatch 写回，no-op 是稳妥语义。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
int DobbyHook(void *address, void *replace_func, void **origin_func);
#ifdef __cplusplus
}
#endif

/* 宿主导出的共享计数（心跳线程与主线程无锁进展信号） */
extern volatile unsigned g_r97b_patch_seq;
extern volatile unsigned g_r97b_patch_ok;
extern volatile unsigned g_r97b_patch_down;

/* R99 分时让出统计：R99b 起分时已禁用，这两个恒 0；host.c 诊断打印仍引用，
   故在此保留定义（原来定义在 R99 权限补丁段，R110 清理时一并删掉导致链接失败）。 */
volatile unsigned g_r99_yield_count = 0;
volatile unsigned g_r99_yield_ms = 0;

/* 已 hook 地址表：Destroy no-op 后 Harmony 重打同一方法不真撤销，Prepare 可能
   收到重复 target。复用首次 trampoline，避免二次 DobbyHook。 */
#define R97_MAX_HOOKS 1024
typedef struct { void *target; void *trampoline; } r97_hook_t;
static r97_hook_t  g_r97_hooks[R97_MAX_HOOKS];
static unsigned    g_r97_hook_n = 0;

static int r97_lookup(void *target, void **trampoline) {
    for (unsigned i = 0; i < g_r97_hook_n; i++) {
        if (g_r97_hooks[i].target == target) {
            *trampoline = g_r97_hooks[i].trampoline;
            return 1;
        }
    }
    return 0;
}
static void r97_remember(void *target, void *trampoline) {
    if (g_r97_hook_n < R97_MAX_HOOKS) {
        g_r97_hooks[g_r97_hook_n].target     = target;
        g_r97_hooks[g_r97_hook_n].trampoline = trampoline;
        g_r97_hook_n++;
    }
}

static const char *r97_image_of(void *p) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (p && dladdr(p, &info) != 0 && info.dli_fname) return info.dli_fname;
    return "(jit/none)";
}

__attribute__((visibility("default")))
int DobbyPrepare(void *target, void *replace, void **prepared) {
    static unsigned seq = 0;
    static struct timeval t0;
    seq++;
    g_r97b_patch_seq = seq;

    if (seq == 1) {
        gettimeofday(&t0, NULL);
        printf("[r110][boot] ===== R110 干净版：0降级0跳过，Dobby源码已修execute-only(vm_read读源/vm_write写跳板) =====\n");
        printf("[r110][boot] shim 只做薄封装；不再 vm_protect 改页权限(R99~R109 屎山已删)\n");
    }
    if (prepared) *prepared = target;   /* 兜底 */

    void *saved = NULL;
    if (r97_lookup(target, &saved)) {
        if (prepared) *prepared = saved;
        return 0;                        /* 复用首次 trampoline */
    }

    /* 零降级：所有 target 都真调 DobbyHook（含 execute-only 页，源码已修） */
    int rc = DobbyHook(target, replace, prepared);
    if (rc == 0 && prepared && *prepared) {
        r97_remember(target, *prepared);
        g_r97b_patch_ok++;
    } else {
        if (prepared) *prepared = target;
        g_r97b_patch_down++;
        printf("[r110][dob] #%u DobbyHook rc=%d target=0x%llx [%s] -> 兜底原函数\n",
               seq, rc, (unsigned long long)(uintptr_t)target, r97_image_of(target));
    }

    if ((seq % 16u) == 0u) {
        struct timeval now; gettimeofday(&now, NULL);
        double ms = (double)(now.tv_sec - t0.tv_sec) * 1000.0
                  + (double)(now.tv_usec - t0.tv_usec) / 1000.0;
        printf("[r110][dob] --- seq=%u ok=%u down=%u hooks=%u 累计=%.0fms ---\n",
               seq, g_r97b_patch_ok, g_r97b_patch_down, g_r97_hook_n, ms);
    }
    return rc;
}

__attribute__((visibility("default")))
int DobbyCommit(void *prepared) {
    (void)prepared;
    return 0;   /* hook 已在 Prepare 写完 */
}

__attribute__((visibility("default")))
int DobbyDestroy(void *address) {
    static unsigned dseq = 0;
    dseq++;
    printf("[r110][destroy] #%u address=0x%llx -> no-op\n",
           dseq, (unsigned long long)(uintptr_t)address);
    return 0;
}
