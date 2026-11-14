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
 *
 * ============================================================================
 * [R100] 用户放开额度到 R110，但加两条硬红线
 * ----------------------------------------------------------------------------
 * R99b 真机（BUILD=e8223bac6c89）：并没有「越过 #162」，日志 1398 行有
 *   Native Crash Reporting + SIGSEGV，栈是 RunClassConstructor /
 *   mono_runtime_class_init_full，和 R98/R98b 同一个 il2cpp cctor；
 *   只是 mono 崩溃处理器没杀掉进程，主线程挂住（心跳 idle 涨到 127518ms），
 *   看起来像「没崩卡死」。闸门打印「等待 0ms」= Unity 信号在闸门启动前
 *   就到了（迟到命中），闸门形同虚设。
 *
 * 用户定论：降级 = 用原版，且「所有 patch 不能跳过不能降级」。
 * 逐行统计两版真机：R97b 86 真 hook / 112 降级，R99b 74 / 84，
 * 降级 100% 是 readT=0（vm_read 读不到 target 32B），地址全合法。
 * 超过一半 Harmony patch 是原版 —— 这才是 NOS 一直加载不起来的根因，
 * 之前 R98~R99b 全在调放行时机 / 信号闸门 / 分时让出，方向偏了。
 *
 * 用户给的钥匙：进程有调试 JIT 权限，探测/写入阶段可临时改页保护，
 *   「执行的时候翻转回来就行」。
 * 用户追加纠正（本轮）：「不能在 RWX 页上 X 会被 TMX 整爆炸」——
 *   TXM/SPTM 的 W^X 是硬件强制，W+X 同时置位的映射上一执行就违规。
 *   => 绝不出现 RWX 第三态：写态 READ|WRITE（无 X，COW 私有副本）只包住
 *      一次 memcpy 读指令，随即 READ|EXECUTE（无 W）+ 刷 icache，然后才轮到
 *      DobbyHook —— 即「执行的时候翻转回来」在 Dobby 之前就完成。详见 r100_make_rw。
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
#include <sys/mman.h>            /* [R100c] mprotect：POSIX，iOS/Linux 通用 */
#include <unistd.h>              /* [R100c] getpagesize() */
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>  /* [R99] CFRunLoopRunInMode：分时让出主线程 */
#include <mach/mach.h>
#include <mach/vm_map.h>            /* r97_safe_read 的 vm_read（R97 起真机验证） */
#include <libkern/OSCacheControl.h> /* [R100] sys_icache_invalidate */
/* [R100c] 为什么是 mprotect 而不是 mach_vm_protect / vm_protect：
   iOS SDK 的 <mach/mach_vm.h> 首行 #error "unsupported"，mach_vm_protect /
   mach_vm_region 在 iOS 用户态不存在（R100 首轮 CI 编译失败即因此）；
   vm_protect 又牵涉 vm_address_t 宽度与 VM_PROT_COPY 可用性。
   mprotect(void*, size_t, int) 是 POSIX：iOS 必然可用、地址是 64 位 void*
   （可承载 0x1_xxxxxxxx 代码地址）、prot 用 PROT_READ/PROT_WRITE/PROT_EXEC，
   且 Linux 本地能真编译同一分支，不用 stub 猜。
   补 READ 是「只读、不改内容」，根本不需要 VM_PROT_COPY/COW 私有副本；
   Dobby 自己写 detour 时会处理可写（R97b 86 个真 hook 已实证）。 */
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

/* ===========================================================================
 * [R100] 零降级核心：给代码页「补 READ」，全程无 W（用户两条硬红线）
 * ---------------------------------------------------------------------------
 * 红线① 所有 patch 必须真 hook，不跳过、不降级（不接受 *prepared=target 用原版顶替）。
 * 红线② 绝不能让页出现 W+X 同时置位。Apple Silicon / iOS 的 TXM(Trusted
 *   Execution Monitor) / SPTM 硬件强制 W^X：一个映射只要同时可写可执行，
 *   在上面执行代码就被判违规炸掉。用户原话「不能在 RWX 页上 X 会被 TMX 整爆炸」。
 *   => 上一版 r100_make_rwx 用了 READ|WRITE|EXECUTE|COPY，正好踩雷，已删除。
 *
 * 根因重新钉死（R96b #9 崩栈 + R99b 84 个 readT=0 统计）：
 *   这些 target 全在 UnityFramework 的 __TEXT 代码页，4 对齐、地址合法，
 *   vm_read 读不到 32B 指令；R96b 硬送 DobbyHook 崩在 relo_relocate 的
 *   ldr —— 也就是 Dobby 内部【同样读不到原指令】。所以缺的是 READ 权限
 *   （iOS 上签名代码页常是 --x / 无 R 的 COW 映射），不是 WRITE。
 *   R97b 86 个真 hook 成功，证明「页可读时 Dobby 自己能改权限写 detour」。
 *   => 正解：把该页补成 READ|EXECUTE（+ VM_PROT_COPY 走 COW 私有副本），
 *      补完 vm_read / Dobby relo_relocate 都能读，Dobby 按已验证路径真 hook。
 *      这一步从头到尾【没有 W】，TXM 完全没有触发条件。
 *
 * 三档（按安全性排序，前两档是纯 RX、永不产生 W+X）：
 *   A 主路径  mprotect(PROT_READ|PROT_EXEC) —— 把 --x 页补成 r-x，全程无 W，
 *             不写内容、不改签名 hash，TXM/SPTM 完全没有触发条件。
 *   B 退化    mprotect(PROT_READ|PROT_EXEC) 再试一次（含 icache 同步）
 *   C 兜底    mprotect(PROT_READ|PROT_WRITE)【无 PROT_EXEC】 —— 仅当 A/B 后
 *             vm_read 仍读不到，用一次 memcpy 读 32B，【立刻】r100_restore_rx
 *             翻回严格 RX，然后才调 DobbyHook。写态窗口=一次 memcpy 且无 X。
 *
 * 安全：
 *   - 任何时刻都不存在 W+X 同时态：A/B 无 W；C 无 X；翻回后无 W。
 *   - 只对 [target, target+32) 覆盖到的页改属性，不整段 region。
 *   - 页大小运行时 getpagesize()，不写死 16384；无需 region 查询。
 *   - 只补 READ、不写任何字节，因此不需要 VM_PROT_COPY / COW 私有副本；
 *     进程内所有线程（含 Unity worker）看同一份页表，Dobby 的 detour 生效。
 *   - [R100c] 用 POSIX mprotect（iOS 禁用 mach_vm_protect）。
 *   - 全部失败也不降级/不跳过：仍照常送 DobbyHook，由真机日志说话。
 * ========================================================================= */
static int r100_promote_rx(void *addr, size_t len,
                           uint64_t *base, uint64_t *size_out) {
    if (!addr || !base || !size_out) return 0;
    long pg = (long)getpagesize();       /* iOS arm64 = 16384，运行时探测不写死 */
    if (pg <= 0) pg = 16384;
    uint64_t mask  = (uint64_t)(pg - 1);
    uint64_t p     = (uint64_t)(uintptr_t)addr;
    uint64_t start = p & ~mask;
    uint64_t end   = (p + (uint64_t)len + mask) & ~mask;
    if (end <= start) return 0;
    *base = start; *size_out = end - start;

    /* A：补 READ，页变 r-x（无 W）。只读不改内容，签名 hash 不变。 */
    int rc = mprotect((void *)(uintptr_t)start, (size_t)(end - start),
                      PROT_READ | PROT_EXEC);
    if (rc != 0)
        /* B：再试一次（某些页需先同步 icache） */
        rc = mprotect((void *)(uintptr_t)start, (size_t)(end - start),
                      PROT_READ | PROT_EXEC);
    return rc == 0;
}

/* C 档兜底：把页切到「无 X 写态」PROT_READ|PROT_WRITE，仅供一次 memcpy 读指令。
   写态没有任何执行权限，因此即便 worker 撞上也不会触发 TXM 的 W+X 违规。 */
static int r100_make_rw(uint64_t base, uint64_t size) {
    if (!base || !size) return 0;
    return mprotect((void *)(uintptr_t)base, (size_t)size,
                    PROT_READ | PROT_WRITE) == 0;   /* 无 PROT_EXEC */
}

/* 执行态：显式清 W、补 R+X，刷干净整段 icache 后再翻回（纯 RX、无 W）。 */
static void r100_restore_rx(uint64_t base, uint64_t size) {
    if (!base || !size) return;
#ifdef __APPLE__
    sys_icache_invalidate((void *)(uintptr_t)base, (size_t)size);
#else
    __builtin___clear_cache((char *)(uintptr_t)base,
                            (char *)(uintptr_t)(base + size));
#endif
    mprotect((void *)(uintptr_t)base, (size_t)size, PROT_READ | PROT_EXEC);
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
    /* [R100] 零降级：重入也绝不 *prepared=target / 提前 return。
       继续走下面的 r97_lookup（重复 target 复用真 trampoline，不会二次
       DobbyHook）/ 真 hook 流程。R99 分时让出默认禁用，本分支实际不可达，
       保留判断只为不破坏 A/B 对照开关。 */
    if (r99_yielding)
        printf("[r100][warn] DobbyPrepare 重入（让出已默认禁用，理论不可达）-> 仍走真 hook 流程，不降级\n");
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
        printf("[r100][boot] ===== R100：零降级零跳过（补 READ 为主，全程无 W+X 同时态）=====\n");
        printf("[r100][boot] 用户硬红线①：所有 patch 真 hook，不降级不跳过（版本额度放宽到 R110）\n");
        printf("[r100][boot] 用户硬红线②：绝不在可写页上执行（W^X）；主路径把 --x 代码页补成纯 RX，兜底写态无 X 且立刻翻回\n");
        printf("[r100][boot] 本版只改这一个变量：闸门时序沿用 R99b 不动（RunStartUp 真机计数=0，锚它已证伪），便于崩了干净归因\n");
        printf("[r100c][boot] CI 修复：iOS 禁用 mach_vm.h，改用 POSIX mprotect 补 READ（无 W，签名 hash 不变）\n");
        printf("[r99b][boot] ===== R99b：删分时让出(证伪) + 信号驱动闸门(等 BeginAsync 再冻结) =====\n");
        printf("[r99][boot] (R99 分时主线程已默认禁用，真机 seq=10 SIGBUS+double fault 证伪)\n");
        printf("[host][r98] ===== R98：判定沿用 R97（vm_read 硬门槛），砍逐 patch dladdr/取证 IO，主线程 QoS 抬档 =====\n");
        printf("[host][r97b] ===== R97b：vm_read 硬门槛（R97 判定不变）/ 共享进度计数 / 日志节流去 fflush =====\n");
    }

    unsigned long long t = (unsigned long long)(uintptr_t)target;
    unsigned long long r = (unsigned long long)(uintptr_t)replace;

    /* [R100] 零降级：兜底不再是「原函数」。trampoline 先置 NULL，
       DobbyHook 成功才填真 trampoline；失败如实返回，绝不用原版顶替。 */
    if (prepared) *prepared = NULL;

    /* —— 重复 target：复用首次 trampoline（真 trampoline，非原版），不二次 hook —— */
    void *saved_tramp = NULL;
    if (r97_lookup(target, &saved_tramp)) {
        if (prepared) *prepared = saved_tramp;
        return 0;
    }

    /* —— 参数合法性（仅诊断，绝不据此跳过；红线：所有 patch 不跳过）—— */
    int nullish  = (t < 0x100000000ull);            /* NULL / 低 4GB 非法区 */
    int misalign = ((t & 3ull) != 0);               /* ARM64 指令须 4 字节对齐 */

    /* —— 取证：安全读 target 32B / replace 8B —— */
    unsigned char head[32], rhead[8];
    memset(head, 0, sizeof(head));
    memset(rhead, 0, sizeof(rhead));
    int got_replace = r97_safe_read(replace, rhead, sizeof(rhead));
    int got_target  = r97_safe_read(target,  head,  sizeof(head));

    /* [R100] 零降级：vm_read 读不到 ≠ 地址坏，是代码页缺 READ（--x）。
       主路径 r100_promote_rx 把该页补成 READ|EXECUTE（+ COW 私有副本），
       补完用 vm_read 重读；只有仍读不到才短暂切「无 X 写态」读一次并
       【立刻】r100_restore_rx 翻回严格 RX。DobbyHook 之前页一定是 RX。 */
    uint64_t r100_base = 0, r100_size = 0;
    int      r100_promoted = 0, r100_wstate = 0;
    if (!got_target && !nullish && !misalign) {
        r100_promoted = r100_promote_rx(target, sizeof(head),
                                        &r100_base, &r100_size);
        if (r100_promoted)
            got_target = r97_safe_read(target, head, sizeof(head));  /* 补 R 后重读 */
        if (!got_target && r100_promoted && r100_make_rw(r100_base, r100_size)) {
            /* C 档兜底：写态无 X，只做一次 memcpy，随即翻回严格 RX */
            memcpy(head, target, sizeof(head));
            r100_restore_rx(r100_base, r100_size);
            got_target = 1;
            r100_wstate = 1;
        }
        if ((seq % 16u) == 0u || seq < 4u)
            printf("[r100][wx] #%u target=0x%llx 补RX=%d 无X写态=%d 读到指令=%d "
                   "（页=0x%llx/%lluB）-> %s\n",
                   seq, t, r100_promoted, r100_wstate, got_target,
                   (unsigned long long)r100_base, (unsigned long long)r100_size,
                   got_target ? "真 hook" : "仍照常送 DobbyHook（不降级不跳过）");
    }

    uint32_t insn0 = (uint32_t)head[0]
                   | ((uint32_t)head[1] << 8)
                   | ((uint32_t)head[2] << 16)
                   | ((uint32_t)head[3] << 24);
    int garbage = got_target && (insn0 == 0u || insn0 == 0xffffffffu);

    /* 零跳过：物理可疑 / 垃圾页也照常送 DobbyHook，不在 C 层提前 return。
       真机统计这三类应为 0；真出现时由 Dobby 的 rc 说话，不替它决定走原版。 */
    if (nullish || misalign)
        printf("[r100][warn] #%u 物理可疑 target=0x%llx nullish=%d misalign=%d"
               " -> 仍送 DobbyHook（零跳过）\n", seq, t, nullish, misalign);
    if (garbage)
        printf("[r100][warn] #%u 指令头全0/全f target=0x%llx insn0=0x%08x"
               " -> 仍送 DobbyHook（零跳过）\n", seq, t, insn0);

    /* [R100] 走到这里，做过提权的页【已经是严格 RX】：
         A/B 档补 READ 后保持 RX 不变；C 档写态已在 memcpy 后立刻翻回 RX。
       DobbyHook 在 RX 页上写 detour，走的是 R97b 86 个真 hook 已实证的路径；
       补上的 READ 让 Dobby 的 relo_relocate 能读到原指令（R96b #9 崩点）。
       全程不存在 W+X 同时态，DobbyHook 之前不残留任何写态 —— 用户红线②。 */
    int rc = DobbyHook(target, replace, prepared);

    if (rc != 0) {
        printf("[r100][dob] #%u DobbyHook rc=%d（真失败，不用原版顶替）\n", seq, rc);
        g_r97b_patch_down++;
    } else if (!prepared || *prepared == NULL) {
        printf("[r100][dob] #%u DobbyHook 返回 0 但 trampoline=NULL（真失败）\n", seq);
        g_r97b_patch_down++;
    } else {
        r97_remember(target, *prepared);
        g_r97b_patch_ok++;
    }

    /* [R97b] 每 32 个 patch 一条汇总（替代 R97 每个 patch 2~3 次同步 fflush）。
       心跳线程另有独立的 1s stdout 刷新，日志不会因节流而看不到进展。 */
    if ((seq % 16u) == 0u) {   /* [R98b] 32->16：崩点附近更细的进度 */
        struct timeval now; gettimeofday(&now, NULL);
        double ms = (double)(now.tv_sec - r98_t0.tv_sec) * 1000.0
                  + (double)(now.tv_usec - r98_t0.tv_usec) / 1000.0;
        printf("[r100][dob] --- 进度 seq=%u ok=%u down=%u hooks=%u 累计=%.0fms "
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
