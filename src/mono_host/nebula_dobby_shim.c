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
 *      DobbyHook —— 即「执行的时候翻转回来」在 Dobby 之前就完成。
 *
 * [R100c / R101 / R102 真机结论 —— 改页保护这条线已彻底作废]
 *   R100c（358402cc5aeb）：冒烟 JIT 链路全绿、零降级字面成立（ok=7 down=0），
 *     但第 8 个 patch 崩在 relo_relocate —— Dobby 读不到 target 原指令。
 *   R101（985a734a8647）：改回 vm_protect + VM_PROT_COPY 补 READ，仍是
 *     seq=4 ok=4 down=0 后崩在同一个 relo_relocate；managed 栈钉死
 *     NebulaPlugin:LoadInternal -> Harmony:PatchAll -> DobbyLib:Prepare。
 *     决定性事实：前 4 个 patch 的 vm_read 都成功（got_target=1），R101 的
 *     提权条件写的是 !got_target —— 根本没进 COW 分支；[r101][cow] 0 条且
 *     崩时 stdio 缓冲把诊断全吞了。崩栈 libsystem_platform(memcpy) 说明
 *     【Dobby 用 CPU 直接 deref target 读指令】，和 vm_read 是两回事：
 *     补 READ 就算成功也管不到 vm_read 本来就能读的那些页。
 *   => R102（本版）：彻底删除所有改页保护逻辑（vm_protect / VM_PROT_COPY /
 *      mprotect 加权限），改从【Mach-O 文件读原始机器码】，用 MAP_FIXED 在
 *      target 原地址重建可读私有映射。详见下方 [R102] 段。
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
#include <sys/mman.h>            /* [R102] mmap(MAP_FIXED) / mprotect / PROT_* */
#include <unistd.h>              /* [R102] getpagesize() / pread() / close() */
#include <errno.h>               /* [R102] open/pread/mmap/mprotect 失败原因 */
#include <fcntl.h>               /* [R102] open(O_RDONLY)：读 Mach-O 原始机器码 */
#include <sys/stat.h>            /* [R102] 文件完整性诊断 */
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>  /* [R99] CFRunLoopRunInMode：分时让出主线程 */
#include <mach/mach.h>
#include <mach/vm_map.h>            /* r97_safe_read 的 vm_read（R97 起真机验证） */
#include <libkern/OSCacheControl.h> /* [R102] sys_icache_invalidate：影子页翻 RX 后刷缓存 */
/* [R102] vm_protect / VM_PROT_COPY / 提权补 READ 全部删除 —— R100/R100c/R101
   三版真机证伪：code-signed 共享 __TEXT_EXEC 页加保护被内核拒，且 Dobby 是
   CPU 直接 deref，补 READ 也拦不住。改为「从文件读机器码 + MAP_FIXED 影子页」。
   mach/vm_map.h 仅保留给 r97_safe_read 的 vm_read（取证/验证，不参与判定）。 */
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
 * [R102] 降维打击：放弃改权限，从 Mach-O 文件读原始机器码铺回 target 原地址
 * ---------------------------------------------------------------------------
 * R101 真机（BUILD=985a734a…，nebula_host.txt 1300~1402）managed 栈钉死崩点：
 *   NebulaPlugin:LoadInternal -> Harmony:PatchAll -> Il2CppInteropDetour:Apply
 *   -> DobbyLib:Prepare -> DobbyHook -> relo_relocate -> libsystem_platform(memcpy)
 *   —— Dobby 用【CPU 直接读 target 原指令】，与 vm_read（Mach 内核读）是两回事。
 *   R101 前 4 个 patch vm_read 成功（got_target=1），提权条件 !got_target 没进，
 *   第 5 个 patch 直接崩；[r101][cow] 0 条还叠加了崩溃时 stdio 缓冲丢失。
 *   => 「临时改页保护补 READ」这条线（R100 / R100c / R101 三版）彻底作废。
 *
 * DeepSeek 方向（用户转述）：放弃强行读内存，改从 Mach-O 文件读原始机器码。
 * 但「把 32B 塞进临时内存喂给 Dobby」不成立：Dobby 从 target 读指令、往
 * target 写跳板、按 target 算跳回地址，三者都是同一个原地址；伪造块地址不同
 * 会让跳回错位。R102 的正确落地是让【target 原地址本身】变成可读私有映射：
 *   dladdr -> image 基址(含 slide) + 文件路径
 *   -> 解析 LC_SEGMENT_64 把 (target - image_base) 换算成文件偏移
 *   -> pread 读整页原始机器码（DeepSeek 第 2 点）
 *   -> mmap(MAP_FIXED) 在该页原地址重建匿名私有 RW 映射（无 X）填入机器码
 *   -> 立刻 mprotect 翻 PROT_READ|PROT_EXEC（无 W）+ 刷 icache
 *   -> 之后 Dobby 从 target 读/写/算跳回全部落在私有副本，不再碰签名共享页
 *
 * 每页只接管一次（首次接管时该页还没有任何 detour），后续同页 patch 累积保留。
 * 严格 W^X：写态 R+W（无 X）、执行态 R+X（无 W），任何时刻无 W+X 同时态。
 * 零降级零跳过保持：接管失败也照常送 DobbyHook，绝不用原版顶替。
 * 失败原因（FairPlay 加密 / open / pread / mmap / mprotect）全量记录并 fflush，
 * 不再让崩溃吞掉诊断（R101 的坑）。
 * ========================================================================= */

#define R102_MH_MAGIC_64          0xfeedfacfu
#define R102_LC_SEGMENT_64        0x19u
#define R102_LC_ENCRYPTION_INFO64 0x2Cu
#define R102_CPU_TYPE_ARM64       0x0100000cu
#define R102_MAX_SHADOW_PAGES     512

struct r102_mach_header_64 {
    uint32_t magic, cputype, cpusubtype, filetype;
    uint32_t ncmds, sizeofcmds, flags, reserved;
};
struct r102_load_command { uint32_t cmd, cmdsize; };
struct r102_segment_command_64 {
    uint32_t cmd, cmdsize;
    char     segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int32_t  maxprot, initprot;
    uint32_t nsects, flags;
};
struct r102_encryption_info_64 {
    uint32_t cmd, cmdsize, cryptoff, cryptsize, cryptid, pad;
};

/* 已接管页表：每页只重建一次，避免抹掉同页已写入的 detour。 */
static uint64_t g_r102_pages[R102_MAX_SHADOW_PAGES];
static unsigned g_r102_page_n = 0;

static int r102_page_taken(uint64_t base) {
    for (unsigned i = 0; i < g_r102_page_n; i++)
        if (g_r102_pages[i] == base) return 1;
    return 0;
}
static void r102_page_remember(uint64_t base) {
    if (g_r102_page_n < R102_MAX_SHADOW_PAGES)
        g_r102_pages[g_r102_page_n++] = base;
}

/* 把 image 内相对偏移换算成 Mach-O 文件偏移；顺带检出 FairPlay 加密。
   自包含结构体，不依赖 iOS SDK 的 mach-o/loader.h（R100 首轮 CI 被 SDK 头坑过）。

   rel_off = addr - dli_fbase（dli_fbase 指向 mach_header / __TEXT 段加载基址）。
   换算要注意：Mach-O 段 vmaddr 是【未 slide 的链接地址】，主可执行文件的
   __TEXT.vmaddr = 0x100000000，framework 通常为 0。dladdr 不告诉我们这个基准。
   因此先扫一遍缓存所有段，取 fileoff==0 的最小 vmaddr 作 vmaddr_base，则
       unslid_vmaddr = vmaddr_base + rel_off
       file_off      = seg.fileoff + (unslid_vmaddr - seg.vmaddr)
   对 framework（vmaddr_base=0）和主程序（vmaddr_base=0x100000000）都成立。 */
#define R102_MAX_SEGS 24
struct r102_seg_cache {
    uint64_t vmaddr, vmsize, fileoff, filesize;
    char     name[16];
};
static int r102_image_fileoff(const char *path, uint64_t rel_off,
                              uint64_t *file_off_out, int *encrypted_out,
                              char *seg_out, size_t segcap) {
    if (!path || !file_off_out) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -10;
    struct r102_mach_header_64 mh;
    if (pread(fd, &mh, sizeof(mh), 0) != (ssize_t)sizeof(mh) ||
        mh.magic != R102_MH_MAGIC_64) {
        close(fd); return -11;                       /* 非 64 位 Mach-O */
    }
    if (mh.cputype != R102_CPU_TYPE_ARM64) {
        close(fd); return -12;                       /* 不是 arm64（用户：AU 纯 arm64） */
    }
    uint64_t off = sizeof(mh);
    int enc = 0, nseg = 0;
    uint64_t vmaddr_base = UINT64_MAX;
    struct r102_seg_cache segs[R102_MAX_SEGS];
    memset(segs, 0, sizeof(segs));
    for (uint32_t k = 0; k < mh.ncmds && k < 4096u; k++) {
        struct r102_load_command lc;
        if (pread(fd, &lc, sizeof(lc), (off_t)off) != (ssize_t)sizeof(lc) ||
            lc.cmdsize < sizeof(struct r102_load_command)) break;
        if (lc.cmd == R102_LC_SEGMENT_64 &&
            lc.cmdsize >= sizeof(struct r102_segment_command_64)) {
            struct r102_segment_command_64 sg;
            if (pread(fd, &sg, sizeof(sg), (off_t)off) == (ssize_t)sizeof(sg)) {
                if (sg.fileoff == 0 && sg.vmaddr < vmaddr_base)
                    vmaddr_base = sg.vmaddr;         /* __TEXT 基准 */
                if (nseg < R102_MAX_SEGS) {
                    segs[nseg].vmaddr   = sg.vmaddr;
                    segs[nseg].vmsize   = sg.vmsize;
                    segs[nseg].fileoff  = sg.fileoff;
                    segs[nseg].filesize = sg.filesize;
                    memcpy(segs[nseg].name, sg.segname, 16);
                    nseg++;
                }
            }
        } else if (lc.cmd == R102_LC_ENCRYPTION_INFO64 &&
                   lc.cmdsize >= sizeof(struct r102_encryption_info_64)) {
            struct r102_encryption_info_64 ei;
            if (pread(fd, &ei, sizeof(ei), (off_t)off) == (ssize_t)sizeof(ei) &&
                ei.cryptid != 0) enc = 1;
        }
        off += lc.cmdsize;
        if (off > (uint64_t)sizeof(mh) + mh.sizeofcmds) break;
    }
    close(fd);
    if (encrypted_out) *encrypted_out = enc;
    if (enc) return -13;                             /* 文件加密，机器码不可读 */
    if (vmaddr_base == UINT64_MAX || nseg == 0) return -14;
    uint64_t unslid = vmaddr_base + rel_off;
    for (int i = 0; i < nseg; i++) {
        if (unslid >= segs[i].vmaddr &&
            (unslid - segs[i].vmaddr) < segs[i].filesize) {
            *file_off_out = segs[i].fileoff + (unslid - segs[i].vmaddr);
            if (seg_out && segcap) {
                memset(seg_out, 0, segcap);
                memcpy(seg_out, segs[i].name,
                       sizeof(segs[i].name) < segcap ? sizeof(segs[i].name) : segcap - 1);
            }
            return 0;
        }
    }
    return -15;                                      /* 该 vmaddr 不在任何文件段内 */
}

/* 让 target 所在页的原地址变成「可读 + 原始机器码」的私有映射。
   返回 tier：1=vm_read 保留现状重建 / 2=文件原始机器码重建 / 0=失败(原因写 reason)。 */
static int r102_shadow_page(void *target, void *image_base, const char *image_path,
                            int *errno_out, uint64_t *file_off_out,
                            unsigned char *insn4_out, char *reason, size_t reasoncap) {
    long pg = (long)getpagesize();
    if (pg <= 0) pg = 16384;
    uint64_t mask  = (uint64_t)(pg - 1);
    uint64_t base  = (uint64_t)(uintptr_t)target & ~mask;
    if (errno_out)   *errno_out = 0;
    if (file_off_out)*file_off_out = 0;
    if (r102_page_taken(base)) {
        snprintf(reason, reasoncap, "已接管(跳过重建，detour 累积保留)");
        return 1;
    }

    uint64_t rel_page = base - (uint64_t)(uintptr_t)image_base;
    uint64_t foff = 0;
    int enc = 0;
    char seg[24] = "?";
    int fr = r102_image_fileoff(image_path, rel_page, &foff, &enc,
                                seg, sizeof(seg));
    if (file_off_out) *file_off_out = foff;
    if (fr != 0) {
        snprintf(reason, reasoncap,
                 "fileoff失败 rc=%d enc=%d cputype需arm64 path=%s", fr, enc, image_path);
        return 0;
    }

    unsigned char *pagebuf = (unsigned char *)malloc((size_t)pg);
    if (!pagebuf) { snprintf(reason, reasoncap, "malloc(%ld)失败", pg); return 0; }
    int got_mem = 0;
#ifdef __APPLE__
    /* 优先读当前内存整页：若该页恰好可读，能保留 dyld fixup / 已写 detour。 */
    vm_offset_t data = 0;
    mach_msg_type_number_t cnt = 0;
    kern_return_t kr = vm_read(mach_task_self(), (vm_address_t)base,
                               (vm_size_t)pg, &data, &cnt);
    if (kr == KERN_SUCCESS && data && cnt >= (mach_msg_type_number_t)pg) {
        memcpy(pagebuf, (const void *)(uintptr_t)data, (size_t)pg);
        vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
        got_mem = 1;
    } else if (data) {
        vm_deallocate(mach_task_self(), (vm_address_t)data, (vm_size_t)cnt);
    }
#else
    memcpy(pagebuf, (const void *)(uintptr_t)base, (size_t)pg);
    got_mem = 1;
#endif
    /* 读不到 -> 从 Mach-O 文件读整页原始机器码（DeepSeek 核心）。 */
    if (!got_mem) {
        int fd = open(image_path, O_RDONLY);
        if (fd < 0) {
            if (errno_out) *errno_out = errno;
            snprintf(reason, reasoncap, "open失败 errno=%d", errno);
            free(pagebuf); return 0;
        }
        errno = 0;
        ssize_t n = pread(fd, pagebuf, (size_t)pg, (off_t)foff);
        int perr = errno;
        close(fd);
        if (n <= 0) {
            if (errno_out) *errno_out = perr;
            snprintf(reason, reasoncap, "pread失败 n=%zd errno=%d off=0x%llx",
                     n, perr, (unsigned long long)foff);
            free(pagebuf); return 0;
        }
        if (n < pg) memset(pagebuf + n, 0, (size_t)pg - (size_t)n);
    }

    /* [R102] 覆盖前字节校验（保险）：target 在页内的偏移处 4 字节必须像合法
       arm64 指令。若文件被 FairPlay 加密 / 段换算错 / 预读失败，宁可如实报错
       并仍送 DobbyHook（零跳过），也绝不把错误字节铺进正在执行的 __TEXT_EXEC 页。 */
    size_t insn_off = (size_t)((uintptr_t)target - (uintptr_t)base);
    if (insn_off + 4 <= (size_t)pg) {
        uint32_t probe = (uint32_t)pagebuf[insn_off]
                       | ((uint32_t)pagebuf[insn_off + 1] << 8)
                       | ((uint32_t)pagebuf[insn_off + 2] << 16)
                       | ((uint32_t)pagebuf[insn_off + 3] << 24);
        if (probe == 0u || probe == 0xffffffffu) {
            snprintf(reason, reasoncap,
                     "原始码 insn0=0x%08x 疑似加密/换算错，拒绝覆盖真代码页",
                     probe);
            free(pagebuf); return 0;
        }
    }

    /* MAP_FIXED 在【原地址】重建匿名私有映射：RW 态（无 X）填充原始机器码。
       覆盖的是 code-signed 共享映射，新页是本进程私有副本，不写回文件、
       不影响其他映射者，也不再受签名页「改保护被内核拒绝」的限制。 */
    errno = 0;
    void *m = mmap((void *)(uintptr_t)base, (size_t)pg,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED || m != (void *)(uintptr_t)base) {
        if (errno_out) *errno_out = errno;
        snprintf(reason, reasoncap, "mmap(MAP_FIXED)失败 errno=%d got=%p", errno, m);
        free(pagebuf); return 0;
    }
    memcpy((void *)(uintptr_t)base, pagebuf, (size_t)pg);
    free(pagebuf);

    /* 立刻翻回严格 RX（无 W）+ 刷 icache：执行态绝不带 W。 */
    errno = 0;
    if (mprotect((void *)(uintptr_t)base, (size_t)pg,
                 PROT_READ | PROT_EXEC) != 0) {
        if (errno_out) *errno_out = errno;
        snprintf(reason, reasoncap, "mprotect(RX)失败 errno=%d", errno);
        return 0;
    }
#ifdef __APPLE__
    sys_icache_invalidate((void *)(uintptr_t)base, (size_t)pg);
#else
    __builtin___clear_cache((char *)(uintptr_t)base,
                            (char *)(uintptr_t)(base + (uint64_t)pg));
#endif
    r102_page_remember(base);
    if (insn4_out) memcpy(insn4_out, (void *)(uintptr_t)target, 4);
    snprintf(reason, reasoncap, "%s seg=%s fileoff=0x%llx -> 严格RX",
             got_mem ? "vm_read保留现状" : "文件原始机器码重建",
             seg, (unsigned long long)foff);
    return got_mem ? 1 : 2;
}

/* 开关：默认开；NEBULA_R102_SHADOW=0 关闭做 A/B（崩了能干净归因）。 */
static int r102_cfg_shadow = -1;
static void r102_cfg_init(void) {
    if (r102_cfg_shadow >= 0) return;
    r102_cfg_shadow = 1;
    const char *v = getenv("NEBULA_R102_SHADOW");
    if (v && (!strcmp(v, "0"))) r102_cfg_shadow = 0;
    printf("[r102][cfg] 影子页接管 enabled=%d 页大小=%ld "
           "（NEBULA_R102_SHADOW=0 关闭做 A/B）\n",
           r102_cfg_shadow, (long)getpagesize());
    fflush(stdout);
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
        r102_cfg_init();
        printf("[r102][boot] ===== R102：放弃改权限 / 从 Mach-O 文件读原始机器码 / MAP_FIXED 影子页 =====\n");
        printf("[r102][boot] R101 真机证伪：Dobby 用 CPU 直接读 target（relo_relocate 崩），vm_read 能读也拦不住；补 READ 三版全作废\n");
        printf("[r102][boot] 降维打击：dladdr->LC_SEGMENT_64->pread 整页机器码->MAP_FIXED 原地址重建私有映射->翻严格RX\n");
        printf("[r102][boot] 红线保持：零降级零跳过；写态 R+W 无 X、执行态 R+X 无 W，任何时刻无 W+X 同帧\n");
        printf("[r102][boot] 诊断每条 fflush(stdout)：R101 崩前日志被 stdio 缓冲吞掉的坑不再犯\n");
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

    /* [R102] 只在 vm_read 读不到时接管该页；可读页交给 Dobby 原路径
       （R97b 86 个真 hook / R101 前 4 个 ok 已证 Dobby 能处理可读页）。
       诊断每条都 fflush(stdout)：R101 崩前 [cow] 全被 stdio 缓冲吞掉的坑。 */
    if (r102_cfg_shadow && !nullish && !misalign) {
        long pg = (long)getpagesize();
        if (pg <= 0) pg = 16384;
        uint64_t page_base = t & ~((uint64_t)(pg - 1));
        int taken = r102_page_taken(page_base);
        if (!got_target && !taken) {
            Dl_info di;
            memset(&di, 0, sizeof(di));
            int has_img = (dladdr(target, &di) != 0 && di.dli_fbase && di.dli_fname);
            if (has_img) {
                uint64_t foff = 0;
                int ern = 0;
                unsigned char i4[4] = {0, 0, 0, 0};
                char why[320] = "";
                int tier = r102_shadow_page(target, di.dli_fbase, di.dli_fname,
                                            &ern, &foff, i4, why, sizeof(why));
                printf("[r102][shadow] #%u t=0x%llx page=0x%llx tier=%d errno=%d "
                       "fileoff=0x%llx insn4=%02x%02x%02x%02x [%s] %s\n",
                       seq, t, (unsigned long long)page_base, tier, ern,
                       (unsigned long long)foff, i4[0], i4[1], i4[2], i4[3],
                       di.dli_fname, why);
                fflush(stdout);
                if (tier > 0) got_target = r97_safe_read(target, head, sizeof(head));
            } else {
                printf("[r102][shadow] #%u t=0x%llx 无 image 归属(dladdr失败)，"
                       "仍送 DobbyHook（零跳过）\n", seq, t);
                fflush(stdout);
            }
        } else {
            printf("[r102][ok] #%u t=0x%llx readT=%d taken=%d [%s]\n",
                   seq, t, got_target, taken, r97_image_of(target));
            fflush(stdout);
        }
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

    /* [R102] 走到这里，若该页被接管过，target 原地址已经是【私有 RX 映射】，
       内容是 Mach-O 文件里的原始机器码：Dobby 的 CPU 读指令、写跳板、算跳回
       三处全部落在这个私有副本上，不再碰 code-signed 共享页（R96b #9 /
       R100c #8 / R101 #5 的 relo_relocate 崩点被根治）。
       未被接管的页是 vm_read 可读页，走 R97b 86 个真 hook 已实证的原路径。
       全程无 W+X 同时态：填充用 R+W（无 X），执行前已翻 R+X（无 W）。
       接管失败也照常送 DobbyHook —— 零跳过，不降级，不用原版顶替。 */
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
        /* [R102] 真 hook 字节级验证：DobbyHook 返回后重读 target 前 16B，
           与原始 head 对比 —— 不同才说明跳板真的写进去了（不是走原版）。 */
        unsigned char post[16]; memset(post, 0, sizeof(post));
        int got_post = r97_safe_read(target, post, sizeof(post));
        int really = got_post && memcmp(post, head, 16) != 0;
        printf("[r102][dob] --- 进度 seq=%u ok=%u down=%u hooks=%u 累计=%.0fms "
               "last_target=0x%llx [%s] replace=0x%llx [%s] readR=%d "
               "post=%d 跳板已写入=%d ---\n",
               seq, g_r97b_patch_ok, g_r97b_patch_down, g_r97_hook_n, ms,
               t, r97_image_of(target), r, r97_image_of(replace), got_replace,
               got_post, really);
        fflush(stdout);
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
