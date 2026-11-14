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
 *
 * ============================================================================
 * [R98] 唯一改动：砍逐 patch 的 dladdr / 取证 IO（判定逻辑一行不动）
 * ----------------------------------------------------------------------------
 * R97 判定（vm_read 硬门槛 / DobbyDestroy no-op / hook 表复用）真机已证实「不崩」，
 * 本版绝不碰。只做加速：R97b 每个 patch 无条件调 2 次 dladdr（带 dyld 锁），
 * 202 个 patch 累计 400+ 次；成功路径根本用不到 image 名。改为仅在「降级」
 * 与「每 32 个汇总」时才取证，并给汇总加累计耗时，真机可直接对比 patch 速度。
 *
 * [R98b] 只加诊断（判定仍一行不动）：R98 真机崩在 seq=162 之后，日志停在
 *   #160 降级行，无法确定是 #161/#162/#163 之后崩。把汇总间隔 32->16，
 *   并给 seq>=150 加每 8 个 patch 一条极简编号日志（不打 image / 不 fflush），
 *   若再崩可精确锁定停在哪一号之后，供 R99 精准处理。
 *
 * ============================================================================
 * [R99] 唯一改动：分时主线程（cooperative patching），破「同步 PatchAll 冻结 Unity」死锁
 * ----------------------------------------------------------------------------
 * R98 / R98b 两次真机崩在完全相同的位置 seq=162（BUILD=ad0443cec1be / 4d7aef27f68）：
 *   nebula_r56_invoke_manual_execute() 是同步 mono_runtime_invoke，在主线程独占
 *   约 40s 跑完 202 个 Harmony patch。这 40s 里 Unity 主循环被整段冻死：
 *     ① Unity 日志永远停在 Addressables 早期（R98b 17:08:49.366，相对 t≈1.9s），
 *        0 条 Begin async loading MainMenu / RunStartUp；#162 反射读静态字段触发
 *        Il2CppClassPointerStore<T>.cctor -> il2cpp 类取不到 -> libsystem_platform
 *        SIGSEGV。R97b（Unity 跑到 RunStartUp t≈9.1s 才 PatchAll）跑完 #202 不崩，
 *        唯一差异就是 Unity 启动进度，不是时间窗口大小。
 *     ② Innersloth 动画停摆。
 *     ③ NOS 的 NebulaPlugin.LoadInternal() 在 Harmony.PatchAll() 之后才
 *        set_sceneLoaded 注册回调，回调要靠 Unity 场景切换（sceneLoaded）触发，
 *        再由 NebulaManager.StartCoroutine 逐帧推进加载链；Unity 一冻结，
 *        回调触发不了、协程饿死 -> NebulaLog.txt 永远只有标题行。
 *   R98b 给 2000ms 窗口仍崩在 #162：只要 PatchAll 同步占住主线程，
 *   「冻结」与「Unity 推进」就不可兼得，给 0 / 2000 / 9000ms 都逃不出这个死锁。
 *
 * 修法：不碰 PatchAll 的单线程顺序（Harmony 有顺序依赖、共享 detour 表、
 *   改同一代码页，并行必崩），而是在每个 patch 的 C 层回调 DobbyPrepare 里
 *   周期性把主线程还给 Unity 主循环跑几帧（嵌套 CFRunLoopRunInMode），
 *   让 Unity 启动进度 / il2cpp 类型 / 动画 / 异步场景加载 / NOS 协程持续推进。
 *
 * 两段式让出（默认按 R97b 真机时间线标定，全部可用环境变量微调，无需重打包）：
 *   - 前段 seq<=100：每 4 个 patch 让出 80ms（25 次 = 2.0s）。叠加阶段A 窗口
 *     Unity 已跑的 ~1.1s -> 累计 ~3.1s，越过 Begin async loading MainMenu
 *     （R97b 实测 t≈2.2s，il2cpp 类型可访问 -> #162 不崩）。
 *   - 后段 seq>100：每 4 个 patch 让出 20ms（25 次 = 0.5s），只保活主循环 /
 *     动画 / 协程，不让 Unity 快速推进到 RunStartUp（R97b 实测 t≈9.1s）。
 *     场景切换必须留到 PatchAll 完成、set_sceneLoaded 注册好之后。
 *   全程让出约 2.5s，PatchAll 40s -> ~43s，仍在品牌页 120s 安全兜底内。
 *   PatchAll 完成后主线程全交还，Unity 继续走完异步加载 -> sceneLoaded 触发
 *   -> NebulaManager + StartCoroutine 加载链自然接上（此时回调早已注册）。
 *
 * 安全约束：
 *   - 让出期间若 Unity 主循环重入 DobbyPrepare，一律降级返回（不二次 patch、
 *     不递归让出），绝不破坏 Harmony 一致性。
 *   - R97 判定（vm_read 硬门槛 / DobbyDestroy no-op / hook 表复用）一行不动。
 *   - NEBULA_R99_YIELD=0 一键关闭让出，完整回退 R98b 行为，便于 A/B 对照。
 *   - 品牌页 @"AUI Mod Loader by Aiden" 页面/署名/双闸门一律不动（硬红线）。
 *
 * ============================================================================
 * [R99b] R99 真机复盘（BUILD=d203bd88465e）—— 分时让出被证伪
 *   ✅ 让出确实让 Unity 主循环推进了：R98/R98b 完全没有的
 *      Begin async loading MainMenu / Done initializing refdata /
 *      Hooked up SceneManager callback / <InitializeRefdata>d__10:MoveNext()
 *      / settings.amogus 全部出现。
 *   ❌ 但 seq=10（t+13s）即 Got a SIGBUS + Got a SIGSEGV + double fault。
 *      崩栈：r99_pump_main_loop -> DobbyPrepare -> mono_jit_runtime_invoke；
 *      另有 QuartzCore -> UnitySendMessage、以及 libsystem_pthread 的
 *      _pthread_start 线程栈。即 Unity 渲染/worker 线程在 Harmony 改代码页
 *      期间并发执行 il2cpp 代码 -> 撞车。
 *   => 「PatchAll 期间让 Unity 跑」与「Harmony 改代码页必须独占」不可兼得。
 *
 * 用户纠正（关键）：R97/R98/R99 从没「进原版主菜单可玩」，也没被成功
 *   「提前劫持不放行」。真机画面是：原版主菜单【加载界面】出来了，一直
 *   停在加载中卡死。host 日志里没有 BeginAsync 只是 stdout 抓取在主线程
 *   被占时积压丢失，不能据此判断 Unity 没走到。
 *
 * 真因：Nebula.dll LoadInternal() 里 Harmony.PatchAll() 在 IL_0068，
 *   SceneManager.sceneLoaded += 回调 在 IL_0099（PatchAll 之后）。
 *   MainMenu 场景启动 ~2s 就开始异步加载，PatchAll 要占 40s，回调注册时
 *   sceneLoaded 早已触发 -> NebulaManager 永不创建 -> 加载协程饿死。
 *
 * 修法（R99b，见 nebula_mono_host.c）：不再猜毫秒数、不再让出。
 *   信号驱动把 PatchAll 冻结点锚在 Unity 打出 "Begin async loading MainMenu"
 *   那一刻：类型已注册(#162 不崩)、sceneLoaded 尚未触发；冻结期间不并发；
 *   PatchAll 里注册好回调，主线程交还后 Unity 完成场景加载 ->
 *   sceneLoaded 触发 -> NOS 加载链自然接上。
 * ========================================================================== */
/* Linux 本地语法检查需要 _GNU_SOURCE 才暴露 Dl_info/dladdr；Darwin 无条件提供。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>               /* dladdr：仅用于打印归属 image，不参与判定 */
#include <sys/time.h>            /* [R98] gettimeofday：每 32 个 patch 打印累计耗时 */
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>  /* [R99] CFRunLoopRunInMode：分时让出主线程 */
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
 * [R99] 分时主线程：PatchAll 期间把主线程还给 Unity 主循环。
 * g_r99_yield_* 由宿主 nebula_r56_invoke_manual_execute 在结束时打印，供真机核对。
 * ------------------------------------------------------------------------- */
volatile unsigned g_r99_yield_count = 0;
volatile unsigned g_r99_yield_ms    = 0;

static volatile int r99_yielding = 0;   /* 嵌套 runloop 重入保护（同线程） */

static void r99_pump_main_loop(double seconds) {
#ifdef __APPLE__
    /* 当前正处于主队列 block 内的 mono_runtime_invoke(PatchAll) 调用栈。
       嵌套跑一次 default mode：Unity 的 CADisplayLink / 主循环 source 得以推进，
       而 GCD 主队列是串行的，当前 block 未返回前不会重入阶段B。 */
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
#else
    (void)seconds;   /* Linux gcc -fsyntax-only stub，不参与 iOS 构建 */
#endif
}

static int      r99_cfg_enabled = -1;   /* -1 未初始化 / 0 关 / 1 开 */
static unsigned r99_every       = 4;
static unsigned r99_early_seq   = 100;
static unsigned r99_early_ms    = 80;
static unsigned r99_late_ms     = 20;

static void r99_cfg_init(void) {
    if (r99_cfg_enabled >= 0) return;
    /* [R99b] 默认关闭。R99 真机（BUILD=d203bd88465e）崩在 seq=10：
       嵌套 CFRunLoopRunInMode 把主线程还给 Unity 后，Unity 的 QuartzCore 渲染回调 /
       UnitySendMessage / il2cpp worker 线程在 Harmony 正改代码页时并发执行 ->
       SIGBUS + SIGSEGV + double fault（比 R98b 的 seq=162 更早）。
       结论：PatchAll 期间绝不能让 Unity 跑起来。保留代码仅作 A/B 对照，
       NEBULA_R99_YIELD=1 才开启。 */
    r99_cfg_enabled = 0;
    const char *on = getenv("NEBULA_R99_YIELD");
    if (on && !strcmp(on, "1")) r99_cfg_enabled = 1;
    const char *e;
    e = getenv("NEBULA_R99_YIELD_EVERY");
    if (e && *e) { int v = atoi(e); if (v >= 1 && v <= 64)   r99_every = (unsigned)v; }
    e = getenv("NEBULA_R99_YIELD_EARLY_SEQ");
    if (e && *e) { int v = atoi(e); if (v >= 0 && v <= 4096) r99_early_seq = (unsigned)v; }
    e = getenv("NEBULA_R99_YIELD_EARLY_MS");
    if (e && *e) { int v = atoi(e); if (v >= 1 && v <= 1000) r99_early_ms = (unsigned)v; }
    e = getenv("NEBULA_R99_YIELD_LATE_MS");
    if (e && *e) { int v = atoi(e); if (v >= 1 && v <= 1000) r99_late_ms = (unsigned)v; }
    printf("[r99b][cfg] 分时让出(R99) %s enabled=%d every=%u early_seq=%u early_ms=%u late_ms=%u "
           "（R99b 默认禁用；NEBULA_R99_YIELD=1 才开回 R99 做 A/B）\n",
           r99_cfg_enabled ? "已开启[A/B]" : "已禁用[真机证伪]",
           r99_cfg_enabled, r99_every, r99_early_seq, r99_early_ms, r99_late_ms);
}

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
    /* [R99] 让出期间 Unity 主循环若重入本函数：一律降级，绝不二次 patch、
       绝不递归让出（Harmony patch 途中被 Unity 回调重入会破坏 detour 表）。 */
    if (r99_yielding) {
        if (prepared) *prepared = target;
        return 0;
    }
    r99_cfg_init();

    static unsigned seq = 0;
    static struct timeval r98_t0;
    seq++;

    /* [R97b] 每个 patch 都推进心跳看到的序号 —— 这是「是否在加载」的数据源。 */
    g_r97b_patch_seq = seq;

    /* [R99] 分时主线程：只在「两个 patch 之间」把主线程还给 Unity 主循环跑几帧。
       前段重让出尽快越过 Unity 启动早期（il2cpp 类型可访问 -> #162 不崩），
       后段轻让出只保活动画 / 异步加载 / NOS 协程，不提前放行场景切换。 */
    if (r99_cfg_enabled == 1 && (seq % r99_every) == 0u) {
        unsigned ms = (seq <= r99_early_seq) ? r99_early_ms : r99_late_ms;
        r99_yielding = 1;
        r99_pump_main_loop((double)ms / 1000.0);
        r99_yielding = 0;
        g_r99_yield_count++;
        g_r99_yield_ms += ms;
        if ((g_r99_yield_count % 16u) == 0u)
            printf("[r99][yield] 已让出 %u 次 / 累计 %ums（seq=%u，Unity 主循环持续推进）\n",
                   g_r99_yield_count, g_r99_yield_ms, seq);
    }

    if (seq == 1) {
        gettimeofday(&r98_t0, NULL);
        printf("[r99b][boot] ===== R99b：删分时让出(证伪) + 信号驱动闸门(等 BeginAsync 再冻结) =====\n");
        printf("[r99][boot] (R99 分时主线程已默认禁用，真机 seq=10 SIGBUS+double fault 证伪)\n");
        printf("[host][r98] ===== R98：判定沿用 R97（vm_read 硬门槛），砍逐 patch dladdr/取证 IO，主线程 QoS 抬档 =====\n");
        printf("[host][r97b] ===== R97b：vm_read 硬门槛（R97 判定不变）/ 共享进度计数 / 日志节流去 fflush =====\n");
    }

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

    /* [R98] 成功路径不再逐 patch 做 2 次 dladdr（带 dyld 锁）；只在降级/汇总时取证。 */

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
        printf("[r98][dob] #%u 降级 target=0x%llx [%s] readT=%d nullish=%d misalign=%d garbage=%d\n",
               seq, t, r97_image_of(target), got_target, nullish, misalign, garbage);
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
    if ((seq % 16u) == 0u) {   /* [R98b] 32->16：崩点附近更细的进度 */
        struct timeval now; gettimeofday(&now, NULL);
        double ms = (double)(now.tv_sec - r98_t0.tv_sec) * 1000.0
                  + (double)(now.tv_usec - r98_t0.tv_usec) / 1000.0;
        printf("[r98][dob] --- 进度 seq=%u ok=%u down=%u hooks=%u 累计=%.0fms "
               "last_target=0x%llx [%s] replace=0x%llx [%s] readR=%d ---\n",
               seq, g_r97b_patch_ok, g_r97b_patch_down, g_r97_hook_n, ms,
               t, r97_image_of(target), r, r97_image_of(replace), got_replace);
    }
    /* [R98b] 崩点高发区（R98 停在 #162）：每 8 个 patch 一条极简编号，
       不打 image、不 fflush，几乎零开销。再崩即可锁定停在哪一号之后。 */
    if (seq >= 150u && (seq % 8u) == 0u)
        printf("[r98b][dob] seq=%u 仍在推进\n", seq);

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
