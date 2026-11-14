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
 * [R97] 修掉 R96b 真机暴露的最后一个崩溃点，目标：NOS 完成 PatchAll 并加载
 * ----------------------------------------------------------------------------
 * R96b 真机日志（BUILD=edd83cd019f9-20261114）逐 patch 自证：
 *   #1~#8 全部 readT=1 -> DobbyHook 成功（vm_read 页对齐修复生效）
 *   #9  target=0x11ad567c0 readT=0 readR=1
 *       -> R96 的「dladdr 命中已知 image 就放行」把它送进 DobbyHook
 *       -> native stack: libsystem_platform <- relo_relocate <-
 *          GenRelocateCodeAndBranch <- GenerateRelocatedCode <- DobbyHook
 *          <- DobbyPrepare，SIGSEGV（崩溃指令 17 01 40 b9 = ldr w23,[x8]）
 *   managed 栈顶: DobbyLib:Prepare <- PrepareImpl（崩在 Prepare，不是 Destroy）
 *   同轮 NOS 加载器已创建自己的 NebulaLog.txt，说明主体已进初始化入口。
 *
 * 真机对四个机制的判定（证实 / 证伪）：
 *  【证实·保留】DobbyDestroy no-op：崩点已从 R95f 的 Destroy 移到 Prepare。
 *  【证实·保留】vm_read 直接从 addr 读（不再按 16KB 页首对齐）：#1~#8 全可读。
 *  【证实·保留】已 hook 地址表复用：Destroy 变 no-op 后避免同 target 二次 hook。
 *  【证伪·删除】「in_image 放行」：dladdr 命中只说明地址落在 image 映射范围，
 *          不等于该地址开头 32 字节指令可读。#9 正是坏地址，送进 dobby 必崩。
 *  【证伪·删除】SIGSEGV/SIGBUS 信号兜底：iOS 上 EXC_BAD_ACCESS 由 Mach 异常
 *          先转 mono handler，BSD sigaction 抢不到，实证完全没触发。
 *
 * R97 修法（只改判定，不加新机制）：
 *   vm_read 能读满 target 32B 且地址合法且指令头非垃圾 -> 真调 DobbyHook。
 *   其余一律降级：*prepared = 原函数，return 0，绝不调 DobbyHook。
 *   降级对 Harmony 完全合法：R95f 降级 34 个 patch 时 PatchAll 照常推进到
 *   NebulaPlugin.LoadInternal，只是该 patch 不生效，NOS 仍能完成加载。
 *
 * 架构前提（用户实锤，R95e 的 PAC 误判已彻底删除）：
 *   AU = Among Us 是纯 arm64 上架包，进程内不存在 PAC / PACIASP / BTI / RETAA。
 *   iPad M4 芯片兼容 arm64e 不代表本进程有 PAC 签名指针。
 *   NOS = Nebula on the Ship。
 *
 * ============================================================================
 * [R97b] 本版唯一改动：诊断数据源 + 砍同步 IO（判定逻辑一行不动）
 * ----------------------------------------------------------------------------
 * R97 真机（BUILD=e3595bcfeefa-20261114）：进程【首次不崩】走完 PatchAll
 *   (#202，86 真 hook / 112 降级 / 0 SIGSEGV)，但 Innersloth 动画后长时间黑屏，
 *   用户手动杀进程。两次运行日志停在不同编号(#202 / #128)，无法区分是
 *   「慢加载(33MB NOS 首次 JIT)」还是「卡死(含 NOS 劫持自旋假设)」。
 *
 * 本版做两件事，均为零风险改动：
 *  ① 把 patch 进度写进宿主导出的共享计数（g_r97b_patch_seq/ok/down），
 *     由宿主 R97b 心跳线程每秒打印 stage + seq + 静止时长 —— 一次真机即可
 *     客观定性「在加载 / 静止」，不再靠猜，也不再靠「日志零增长几秒」。
 *  ② 砍掉逐 patch 的 printf+fflush：R97 每个 patch 做 2~3 次同步 fflush，
 *     202 个 patch 累计 600+ 次同步 IO（stdio 已重定向到 nebula_host.txt），
 *     是主线程被拖住的直接嫌疑。改为：首条 banner + 每 32 个 patch 一行汇总
 *     + 降级原因/异常仍单条打印（不打 32B hexdump）。判定结果完全不变。
 *
 * 保持单 dylib：只改这个 C 文件；build_ios.sh 沿用 R96b，不动链接方式。
 */
/* Linux 本地语法检查需要 _GNU_SOURCE 才暴露 Dl_info/dladdr；Darwin 无条件提供。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>               /* dladdr：仅用于打印归属 image，不参与判定 */
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
 * [R97b] 宿主 nebula_mono_host.c 导出的共享计数（同一 dylib 内链接）。
 * 心跳线程是裸 pthread，只 printf 不碰 mono；这几个 volatile 计数是它与
 * 主线程之间唯一的、无锁的进展信号。写丢失无所谓，只要能观察到推进即可。
 * ------------------------------------------------------------------------- */
extern volatile unsigned g_r97b_patch_seq;
extern volatile unsigned g_r97b_patch_ok;
extern volatile unsigned g_r97b_patch_down;

/* ---------------------------------------------------------------------------
 * 安全读：直接从 addr 读 n 字节，绝不直接 deref。
 * R96 起不再按 16KB 页首对齐读；Mach vm_read 的 address 无需页对齐。
 * 非 __APPLE__ 分支仅用于本地 gcc 语法检查，不参与 iOS 构建。
 * ------------------------------------------------------------------------- */
static int r97_safe_read(void *addr, unsigned char *out, size_t n) {
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
static const char *r97_image_of(void *p) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (p && dladdr(p, &info) != 0 && info.dli_fname) return info.dli_fname;
    return "(jit/none)";
}

/* ---------------------------------------------------------------------------
 * 已 hook 地址表：DobbyDestroy 变 no-op 后，Harmony 重打同一方法不会真撤销，
 * Prepare 可能收到重复 target。复用首次 trampoline，避免二次 DobbyHook。
 * ------------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------------
 * DobbyPrepare：vm_read 硬门槛 + 真 hook
 * 判定逻辑与 R97 完全一致，仅：① 更新共享计数 ② 日志节流、去掉 fflush。
 * ------------------------------------------------------------------------- */
__attribute__((visibility("default")))
int DobbyPrepare(void *target, void *replace, void **prepared) {
    static unsigned seq = 0;
    seq++;

    /* [R97b] 每个 patch 都推进心跳看到的序号 —— 这是「是否在加载」的数据源。 */
    g_r97b_patch_seq = seq;

    if (seq == 1)
        printf("[host][r97b] ===== R97b：vm_read 硬门槛（R97 判定不变）/ 共享进度计数 / 日志节流去 fflush =====\n");

    unsigned long long t = (unsigned long long)(uintptr_t)target;
    unsigned long long r = (unsigned long long)(uintptr_t)replace;

    /* 绝对兜底：trampoline = 原函数（不 patch 也不崩）。 */
    if (prepared) *prepared = target;

    /* —— 重复 target：复用首次 trampoline，不二次 hook —— */
    void *saved_tramp = NULL;
    if (r97_lookup(target, &saved_tramp)) {
        if (prepared) *prepared = saved_tramp;
        /* 复用属正常路径，交给 32 步长汇总，不单独刷盘 */
        return 0;
    }

    /* —— 取证：安全读 target 32B / replace 8B —— */
    unsigned char head[32], rhead[8];
    memset(head, 0, sizeof(head));
    memset(rhead, 0, sizeof(rhead));
    int got_target  = r97_safe_read(target,  head,  sizeof(head));
    int got_replace = r97_safe_read(replace, rhead, sizeof(rhead));

    const char *tn = r97_image_of(target);
    const char *rn = r97_image_of(replace);

    /* —— 硬门槛（R97：不再有 dladdr/in_image 放行、不再有 PAC 相关判定）——
     * R96b 真机实锤：readT=0 的地址（#9）进 DobbyHook 必崩在 relo_relocate。
     * vm_read 能读满 32B == dobby 重定位所需指令可读（#1~#8 已真机验证安全）。
     * 读不到 / NULL 低 4GB / 非 4 字节对齐 / 指令头全 0 或全 f（垃圾页）
     * 一律降级，绝不调 DobbyHook。PatchAll 会继续走完，NOS 正常加载。
     */
    int nullish  = (t < 0x100000000ull);            /* NULL / 低 4GB 非法区 */
    int misalign = ((t & 3ull) != 0);               /* ARM64 指令须 4 字节对齐 */
    uint32_t insn0 = (uint32_t)head[0]
                   | ((uint32_t)head[1] << 8)
                   | ((uint32_t)head[2] << 16)
                   | ((uint32_t)head[3] << 24);
    int garbage = got_target && (insn0 == 0u || insn0 == 0xffffffffu);

    if (nullish || misalign || !got_target || garbage) {
        g_r97b_patch_down++;
        /* 降级原因保留单条（无 fflush），供真机定位坏地址来源 */
        printf("[r97b][dob] #%u 降级 target=0x%llx [%s] readT=%d nullish=%d misalign=%d garbage=%d\n",
               seq, t, tn, got_target, nullish, misalign, garbage);
        return 0;
    }

    int rc = DobbyHook(target, replace, prepared);
    if (rc != 0) {
        printf("[r97b][dob] #%u DobbyHook rc=%d（失败）-> 兜底 *prepared=原函数\n", seq, rc);
        if (prepared) *prepared = target;
        g_r97b_patch_down++;
    } else if (prepared && *prepared == NULL) {
        printf("[r97b][dob] #%u DobbyHook 成功但 trampoline=NULL -> 兜底 *prepared=原函数\n", seq);
        *prepared = target;
        g_r97b_patch_down++;
    } else {
        r97_remember(target, prepared ? *prepared : NULL);
        g_r97b_patch_ok++;
    }

    /* [R97b] 每 32 个 patch 一条汇总（替代 R97 每个 patch 2~3 次同步 fflush）。
       心跳线程另有独立的 1s stdout 刷新，日志不会因节流而看不到进展。 */
    if ((seq % 32u) == 0u) {
        printf("[r97b][dob] --- 进度 seq=%u ok=%u down=%u hooks=%u "
               "last_target=0x%llx [%s] replace=0x%llx [%s] readR=%d ---\n",
               seq, g_r97b_patch_ok, g_r97b_patch_down, g_r97_hook_n,
               t, tn, r, rn, got_replace);
    }
    return rc;
}

__attribute__((visibility("default")))
int DobbyCommit(void *prepared) {
    (void)prepared;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 接管 DobbyDestroy（R96 修复，真机已证实有效，R97/R97b 保留）
 * BepInEx DobbyDetour.UndoImpl 会 P/Invoke Destroy(OriginalMethodPtr)，
 * 上游实现会 DobbyCodePatch(memcpy) 写回代码页，在 iOS 上必崩。
 * 游戏进程生命周期内 Harmony 不需要真撤销 detour，no-op 是唯一稳妥语义。
 * ------------------------------------------------------------------------- */
__attribute__((visibility("default")))
int DobbyDestroy(void *address) {
    static unsigned dseq = 0;
    dseq++;
    printf("[r97b][destroy] #%u address=0x%llx -> no-op（不写回代码页）\n",
           dseq, (unsigned long long)(uintptr_t)address);
    return 0;   /* 0 = 成功，BepInEx UndoImpl 直接 pop 丢弃 */
}
