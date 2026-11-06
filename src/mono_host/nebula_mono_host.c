// Nebula iOS Port — Mono/MonoVM 嵌入宿主
//
// 职责：在 AU(iOS/IL2CPP) 进程里初始化 Mono 运行时，然后把控制权交给 BepInEx：
//   加载 BepInEx/core/BepInEx.Unity.IL2CPP.dll → 调 Doorstop.Entrypoint.Start()
//   剩下的事（Il2CppInterop 绑定、chainloader、加载 plugins 里的 NebulaLoader → Nebula.dll）
//   全部由 BepInEx 自己接管。
//   入口格式是 doorstop_config.ini 里写死的：
//     target_assembly = BepInEx\core\BepInEx.Unity.IL2CPP.dll
//     entrypoint = static void Doorstop.Entrypoint.Start()
//
// 两个必须遵守的约束：
//   1) 构造器里必须异步 boot（pthread），绝不在 constructor/DllMain 同步初始化
//      Mono —— 会与加载器锁死锁。
//   2) 内存：全量 NOS 在 iOS 上很容易被 jetsam 杀，且有 RSS 软顶看门狗 + 硬顶
//      （默认 500MB 激进 GC）。统一编译器（JIT）版需要 dynamic-codesigning 权限，
//      没有则 mono 初始化会被内核秒杀（见下方 JIT 闸门）。
//      详见 docs/ios-memory-budget.md
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dlfcn.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>    // mkdir（BepInEx 可写根目录）
#include <strings.h>    // strcasecmp
#include <errno.h>      // EEXIST（R47 配置目录幂等创建）

#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/appdomain.h>   // mono_domain_assembly_open
#include <mono/metadata/class.h>       // mono_class_from_name / mono_class_get_method_from_name
#include <mono/metadata/object.h>      // mono_runtime_invoke
#include <mono/metadata/mono-config.h>
#include <mono/metadata/image.h>      // mono_image_open_from_data_with_name
#include <mono/metadata/mono-gc.h>

// mono 公开 API：在 mono_jit_init 之前调用可强制 AOT-only 模式。
// iOS 不能运行时生成 trampoline 代码（generic / breakpoint trampoline 都会
// g_assert_not_reached），AOT-only 让 mono_trampolines_init 直接 return，
// 彻底避开整组雷。这正是 .NET MAUI iOS 的运行方式。
extern void mono_jit_set_aot_only (int aot_only);
// mono 公开 API：强制让 JIT 内存走 MAP_JIT 分配。iOS 严格 W^X 下，非 MAP_JIT 的
// 可执行内存非法；dotnet/runtime 的 mono 只在 macOS 接了 MAP_JIT（mono-mmap.c 里
// TARGET_OSX 硬绑），iOS 必须我们显式打开。参考 OpenJDK/Pojav 的 W^X 处理。
extern void mono_setmmapjit (int flag);

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task_info.h>
#include <mach/exception.h>        // EXC_BAD_ACCESS / MACH_EXCEPTION_CODES（第十五轮 mach 取证）
#include <setjmp.h>                // sigsetjmp/siglongjmp（冒烟段错误兜底）
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <TargetConditionals.h>   // TARGET_OS_IPHONE / TARGET_OS_MAC
#include <os/log.h>
#include <signal.h>
#include <dispatch/dispatch.h>     // 启动占位文本的延时/主线程销毁
#include <CoreFoundation/CoreFoundation.h>   // CFRunLoopRunInMode：boot 阻塞等待期泵浦主运行循环
#include <sys/mman.h>              // mmap / MAP_JIT（JIT 冒烟测试）
#include <mach/mach.h>              // vm_protect / mach_task_self（detour 翻页）
#include <mach/vm_map.h>             // vm_remap / VM_FLAGS_ANYWHERE（双映射别名；iOS SDK 无 mach_vm.h）
#include <libkern/OSCacheControl.h> // sys_icache_invalidate（指令缓存刷新）
#include <errno.h>
#include <dlfcn.h>                 // dlsym 探测翻转符号（冒烟）
#include <unistd.h>                // getpid
// sys/codesign.h 在 iOS SDK 中被 Apple 剥离（交叉编译会 'file not found'），
// 故手动声明 csops 及其常量。csops 是 libSystem 真实导出的符号，运行/链接均可用。
extern int csops(int pid, unsigned int ops, void *useraddr, unsigned long usersize);
// mach_vm.h 在 iOS SDK 带 #error unsupported（Apple 剥离），且 mach_vm_remap 在 iOS
// 上根本不导出（链接期 undefined symbol）。改用 iOS SDK 确定可用的经典 vm_* API：
// vm_remap / vm_region_64 / vm_write（均在 <mach/vm_map.h>，64 位 Darwin 下等价）。
// 手动声明 vm_region_64 / vm_write 以绕开头文件屏蔽；用途：JIT 冒烟调试态写入原语
// （debugserver 同款）与真实保护位校验。
extern kern_return_t vm_write(vm_map_t target_task,
                              vm_address_t address,
                              vm_offset_t data,
                              mach_msg_type_number_t dataCnt);
extern kern_return_t vm_region_64(vm_map_t target_task,
                                  vm_address_t *address,
                                  vm_size_t *size,
                                  vm_region_flavor_t flavor,
                                  vm_region_info_t info,
                                  mach_msg_type_number_t *infoCnt,
                                  mach_port_t *object_name);
#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0          // 查询进程代码签名状态
#endif
#ifndef CS_DEBUGGED
#define CS_DEBUGGED 0x10000000   // 进程被调试器附加 → 内核授予 JIT 能力
#endif

// [Nebula iOS] JIT 内存路线（2026-10-25 多轮实测定案，详见 nebula_jit_smoke 注释）：
//  · pthread_jit_write_protect_np 在 iOS 官方不导出（Apple 文档明言），dlsym 全路 NULL。
//  · plain RWX + mprotect 补 EXEC：内核假成功（rw-/rwx），执行 SIGBUS —— 死路。
//  · 统一走 MAP_JIT（mono_setmmapjit(1)）；翻转/写入手段由冒烟测试四路线实测决定：
//    mach_vm_write（调试态首选）/ mprotect+WRITE / 翻转函数 / RWX 直配。
//  · 冒烟通过哪条路线，后续就把同款手段注入 mono-mmap.c（(b2)/(b2-b) 已撤销待重做）。
#else
#include <signal.h>
typedef void *os_log_t;
#define os_log(...)
#define os_log_create(...) NULL
#endif

// ---------------------------------------------------------------- 嵌入式 BCL/BepInEx blob
//
// 构建期把 BCL(.NET 8 基础类库, 171 个程序集) + BepInEx/core 托管 dll 打成一个 NBL1 blob，
// 用 ld -sectcreate 嵌进 dylib 的 __DATA,__nebulabcl 段。运行时从这里【内存加载】程序集，
// 不再依赖任何散落的 .dll 文件 —— 成品就一个 .dylib。
//
// blob 格式(NBL1, 小端):
//   uint32 magic = 'NBL1'
//   uint32 count
//   count × {
//     uint16 namelen
//     char   name[namelen]      UTF-8（可带 BepInEx/core/ 子路径，查找只看 basename）
//     uint32 datalen
//     byte   data[datalen]
//   }
typedef struct { const char *name; const uint8_t *data; uint32_t size; } blob_entry;
static blob_entry *g_entries = NULL;
static int         g_n       = 0;

static void blob_load(void) {
    if (g_entries) return;
#ifdef __APPLE__
    // 取自身 dylib 的 mach_header：iOS 上没有 _dyld_get_image_header_containing_address，
    // 改用 dladdr 拿路径 + 遍历 _dyld_image_count 匹配。
    const struct mach_header *hdr = NULL;
    Dl_info info;
    if (dladdr((void *)blob_load, &info) && info.dli_fname) {
        uint32_t cnt = _dyld_image_count();
        for (uint32_t i = 0; i < cnt; i++) {
            const char *nm = _dyld_get_image_name(i);
            if (nm && strcmp(nm, info.dli_fname) == 0) { hdr = _dyld_get_image_header(i); break; }
        }
    }
    if (!hdr) { printf("[host] 取自身 mach_header 失败，无嵌入式 blob\n"); return; }
    unsigned long sz = 0;
    char *p = (char *)getsectiondata((const struct mach_header_64 *)hdr, "__DATA", "__nebulabcl", &sz);
    if (!p || sz < 8) { printf("[host] 未找到 __nebulabcl 段（构建未嵌入 blob，走文件兜底）\n"); return; }
    const uint8_t *q = (const uint8_t *)p;
    uint32_t magic; memcpy(&magic, q, 4); q += 4;
    if (magic != 0x4E424C31u) { printf("[host] blob magic 错误(0x%08X)，跳过\n", magic); return; }
    uint32_t count; memcpy(&count, q, 4); q += 4;
    g_entries = (blob_entry *)calloc(count ? count : 1, sizeof(blob_entry));
    for (uint32_t i = 0; i < count; i++) {
        uint16_t nl; memcpy(&nl, q, 2); q += 2;
        char *nm = (char *)malloc(nl + 1); memcpy(nm, q, nl); nm[nl] = 0; q += nl;
        uint32_t dl; memcpy(&dl, q, 4); q += 4;
        g_entries[i].name = nm; g_entries[i].data = q; g_entries[i].size = dl; q += dl;
    }
    g_n = (int)count;
    printf("[host] 嵌入式 blob: %d 个程序集已就位（全部内存加载，无散文件）\n", g_n);
#else
    printf("[host] 非 Apple 平台，无嵌入式 blob（桌面测试走文件）\n");
#endif
}

// 按程序集简单名查找（去掉路径与 .dll），命中返回 entry
static const blob_entry *blob_find(const char *aname) {
    if (!g_n || !aname) return NULL;
    // [R48] unity-libs/ 前缀装的不是"可加载的程序集"，而是 Il2CppInterop 生成器当参考用的
    // Unity 基础库散文件。它们的文件名（UnityEngine.CoreModule 之类）与后面真正要加载的
    // interop 程序集同名，若参与简单名匹配，会抢占 blob 加载、把插件要的真程序集顶掉。
    // 所以对这段前缀一律跳过，只由 nebula_blob_extract_dir 按文件释放。
    if (!strncmp(aname, "unity-libs/", 11)) return NULL;
    const char *slash = strrchr(aname, '/');
    const char *s = slash ? slash + 1 : aname;
    char base[512];
    strncpy(base, s, sizeof(base) - 1); base[sizeof(base) - 1] = 0;
    size_t L = strlen(base);
    if (L > 4 && !strcasecmp(base + L - 4, ".dll")) base[L - 4] = 0;
    for (int i = 0; i < g_n; i++) {
        const char *full = g_entries[i].name;
        // [R60] 这三段前缀装的是【按文件释放的资源】，不是可内存加载的普通程序集：
        //   unity-libs/  → Il2CppInterop 生成器的 Cecil 参考程序集（R48）
        //   interop/     → 预生成的 iOS 桥接 dll（131 个，必须落成散文件给
        //                  Il2CppInteropManager 用 Assembly.Load 按路径加载；
        //                  若在这里被简单名匹配抢走，会顶掉真正的桥接加载）
        //   native/      → CoreLib P/Invoke 的 iOS shim dylib（fallback 释放用）
        if (!strncmp(full, "unity-libs/", 11) ||
            !strncmp(full, "interop/", 8)    ||
            !strncmp(full, "native/", 7)) continue;
        const char *bn = strrchr(full, '/');
        const char *en = bn ? bn + 1 : full;
        size_t EL = strlen(en); char ebase[512];
        strncpy(ebase, en, sizeof(ebase) - 1); ebase[sizeof(ebase) - 1] = 0;
        if (EL > 4 && !strcasecmp(ebase + EL - 4, ".dll")) ebase[EL - 4] = 0;
        if (!strcasecmp(ebase, base)) return &g_entries[i];
    }
    return NULL;
}

// 从 blob 内存加载一个程序集（open image from data + load from full）
static MonoAssembly *blob_load_assembly(const blob_entry *e) {
    MonoImageOpenStatus status;
    MonoImage *img = mono_image_open_from_data_with_name((char *)e->data, e->size, 1, &status, 0, e->name);
    if (!img || status != MONO_IMAGE_OK) { if (img) mono_image_close(img); return NULL; }
    MonoAssembly *as = mono_assembly_load_from_full(img, e->name, &status, 0);
    if (!as) { mono_image_close(img); return NULL; }
    printf("[host]   内存加载 %s (%u bytes)\n", e->name, e->size);
    return as;
}

// Mono 预加载钩子：解析程序集依赖时先查嵌入式 blob，命中就内存加载，否则交给文件系统
static MonoAssembly *nebula_preload(MonoAssemblyName *aname, char **assemblies_path, void *user_data) {
    (void)assemblies_path; (void)user_data;
    if (!g_n) return NULL;
    const char *name = mono_assembly_name_get_name(aname);
    if (!name) return NULL;
    const blob_entry *e = blob_find(name);
    if (!e) return NULL;
    return blob_load_assembly(e);
}

// ---------------------------------------------------------------- 配置

typedef struct {
    char runtime_mode[16];      // jit（解释器已废除，统一编译器版）
    int  gc_nursery_mb;
    int  gc_soft_limit_mb;      // 软顶：超过即常规 GC（默认 700MB）
    int  gc_hard_limit_mb;      // 硬顶：超过即激进全量 GC + 告警（默认 900MB，见 #12）
    int  gc_watchdog_interval_ms;
    char addons_mode[16];       // all（职业在 addon 里，必须全加载）
    int  addons_texture_max_px;
    int  addons_texture_cache_mb;
    int  addons_audio_lazy;
    int  roslyn_enabled;
    int  render_target_fps;     // 0 = 不限制
    int  render_adaptive;
} host_cfg;

// 内置默认 = Standard 档
static host_cfg CFG = {
    .runtime_mode = "jit",
    .gc_nursery_mb = 16,
    .gc_soft_limit_mb = 1100,
    .gc_hard_limit_mb = 1500,   // [R50] 原 700/900：interop 生成期 Cpp2IL 峰值轻松破 900MB，
                              // 硬顶贴着峰值会让看门狗一直在触发激进全量 GC，表现为
                              // 进程还活着、没被 iOS 杀，但看着像卡死。抬到 1100/1500
                              // 给足空间（实测设备没有被 jetsam 杀，容器内存吃得住）。
    .gc_watchdog_interval_ms = 500,
    .addons_mode = "all",
    .addons_texture_max_px = 512,
    .addons_texture_cache_mb = 64,
    .addons_audio_lazy = 1,
    .roslyn_enabled = 0,
    .render_target_fps = 60,
    .render_adaptive = 1,
};

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) *e-- = '\0';
    return s;
}

// 单一预设：编译器（JIT）版。解释器已被废除（见下方 mono_setmmapjit 处说明），
// standard / pro 合并为一份二进制，始终走 JIT 编译执行。
static void load_cfg(const char *dir) {
    const char *profile = getenv("NEBULA_PROFILE");
    if (!profile || !*profile) profile = "standard";

    char path[PATH_MAX];
    const char *envp = getenv("NEBULA_HOST_CFG");
    if (envp && *envp) {
        snprintf(path, sizeof(path), "%s", envp);
    } else {
        snprintf(path, sizeof(path), "%s/nebula_host.%s.cfg", dir, profile);
        FILE *probe = fopen(path, "r");
        if (!probe) snprintf(path, sizeof(path), "%s/nebula_host.cfg", dir);
        else fclose(probe);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[host] 未找到 cfg(profile=%s)，用内置默认值(standard)\n", profile);
        return;
    }
    printf("[host] profile=%s cfg=%s\n", profile, path);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(line);
        char *v = trim(eq + 1);
        if (!*k || !*v) continue;

        if      (!strcmp(k, "runtime.mode"))             snprintf(CFG.runtime_mode, sizeof(CFG.runtime_mode), "%s", v);
        else if (!strcmp(k, "gc.nursery_mb"))            CFG.gc_nursery_mb = atoi(v);
        else if (!strcmp(k, "gc.soft_limit_mb"))         CFG.gc_soft_limit_mb = atoi(v);
        else if (!strcmp(k, "gc.hard_limit_mb"))         CFG.gc_hard_limit_mb = atoi(v);
        else if (!strcmp(k, "gc.watchdog_interval_ms"))   CFG.gc_watchdog_interval_ms = atoi(v);
        else if (!strcmp(k, "addons.mode"))              snprintf(CFG.addons_mode, sizeof(CFG.addons_mode), "%s", v);
        else if (!strcmp(k, "addons.texture_max_px"))    CFG.addons_texture_max_px = atoi(v);
        else if (!strcmp(k, "addons.texture_cache_mb"))  CFG.addons_texture_cache_mb = atoi(v);
        else if (!strcmp(k, "addons.audio_lazy"))        CFG.addons_audio_lazy = atoi(v);
        else if (!strcmp(k, "roslyn.enabled"))           CFG.roslyn_enabled = atoi(v);
        else if (!strcmp(k, "render.target_fps"))        CFG.render_target_fps = atoi(v);
        else if (!strcmp(k, "render.adaptive"))          CFG.render_adaptive = atoi(v);
    }
    fclose(f);

    // 透传给托管侧（BepInEx/Nebula 那边直接读环境变量，省得再解析一遍文件）
    char buf[32];
    setenv("NEBULA_ADDONS_MODE", CFG.addons_mode, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.addons_texture_max_px);   setenv("NEBULA_TEXTURE_MAX_PX", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.addons_texture_cache_mb); setenv("NEBULA_TEXTURE_CACHE_MB", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.roslyn_enabled);          setenv("NEBULA_ROSLYN", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.render_target_fps);       setenv("NEBULA_TARGET_FPS", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.render_adaptive);         setenv("NEBULA_ADAPTIVE", buf, 1);

    printf("[host] cfg: mode=%s nursery=%dMB soft_limit=%dMB hard_limit=%dMB | addons=%s 贴图≤%dpx 缓存%dMB roslyn=%d\n",
           CFG.runtime_mode, CFG.gc_nursery_mb, CFG.gc_soft_limit_mb, CFG.gc_hard_limit_mb,
           CFG.addons_mode, CFG.addons_texture_max_px, CFG.addons_texture_cache_mb, CFG.roslyn_enabled);
    if (CFG.render_target_fps > 0)
        printf("[host] cfg: 目标帧率=%d fps adaptive=%d\n", CFG.render_target_fps, CFG.render_adaptive);
    else
        printf("[host] cfg: 目标帧率=不限制 adaptive=%d\n", CFG.render_adaptive);
}

// 必须在 mono_jit_init 之前调用才生效
static void apply_gc_env(void) {
    // [Nebula iOS 第十轮] 强制串行（非并发）major GC：
    // 默认 marksweep-conc 在并发标记阶段会把老年代页 mprotect 成只读，依赖
    // SIGSEGV 写屏障（managed 代码写 RO 页 → SIGSEGV → mono 处理器记录 card）。
    // 但 StikDebug 的 ptrace 会先抢走 SIGSEGV，mono 处理器收不到 → RO 页写入
    // 持续 fault → SIGKILL（无 .ips）。实锤死点 _sgen_los_pin_objects 正是
    // finish_concurrent_mode 并发收尾路径。major=marksweep 用纯串行回收，无
    // 并发标记、无 RO 页保护、无 SIGSEGV 写屏障，彻底规避调试器冲突。
    char buf[128];
    int off = snprintf(buf, sizeof(buf), "major=marksweep");
    if (CFG.gc_nursery_mb > 0)
        off += snprintf(buf + off, sizeof(buf) - off, ",nursery-size=%dm", CFG.gc_nursery_mb);
    setenv("MONO_GC_PARAMS", buf, 1);
    printf("[host] MONO_GC_PARAMS=%s\n", buf);
    // 单一编译器（JIT）版：不再有解释器分支，统一走 JIT（见下方 mono_setmmapjit）。
    // [R64b] GC 线程挂起模式（本机构建仅支持 coop）：
    //  本内嵌 mono 构建只编译了 coop 挂起后端：preempt(抢占) 后端缺失，
    //  mono_threads_suspend_policy_init 会 g_error -> abort，直接死在 mono_jit_init
    //  最早期（真机实证：native 栈 mono_threads_suspend_policy_init -> abort）。
    //  故 iOS/macOS 默认一律 coop。coop 当初仅为绕 StikDebug 的 ptrace 信号抢占而设，
    //  现侧载方案(SideStore/LiveContainer+本地 Anisette) 已无 StikDebug，且上方
    //  major=marksweep 串行 GC 已关掉并发标记 RO 页 SIGSEGV 写屏障，coop 可正常工作。
    //  coop 下 MonoMod 的 _HookSelftest 在 BepInEx 初始化最脆弱时刻触发"目标方法首编
    //  +隐式 GC"会 sgen SIGSEGV，已在 MonoMod.RuntimeDetour.dll 里把该方法打成 no-op
    //  （仅为自检、不影响真实 detour）。保留 NEBULA_SUSPEND 环境变量可强制 preempt
    //  做对照实验（iOS 上会 init 失败，仅用于定位）。
    const char *susp_dflt = "coop";
    const char *susp = getenv("NEBULA_SUSPEND");
    if (!susp || !*susp) susp = susp_dflt;
    setenv("MONO_THREADS_SUSPEND", susp, 1);
    printf("[host] MONO_THREADS_SUSPEND=%s（%s）\n", susp,
           strcmp(susp, "coop") == 0 ? "协作挂起(本机 mono 仅支持 coop)" : "抢占挂起(本机后端缺失, iOS 会 init 失败)");
}

// ---------------------------------------------------------------- RSS + 看门狗

static long rss_mb(void) {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (long)(info.resident_size / (1024 * 1024));
    return -1;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) { fclose(f); return -1; }
    fclose(f);
    return (long)(resident * sysconf(_SC_PAGESIZE) / (1024 * 1024));
#endif
}

static volatile int watchdog_run = 1;
static volatile int g_mono_ready = 0;   // mono_jit_init 成功后才置 1，看门狗据此决定能否 GC
static FILE     *g_logfp   = NULL;      // 日志文件句柄（前移：供前置的 watchdog_thread 使用）
static struct timespec g_wd_t0;         // 看门狗启动时刻，心跳打相对时间戳（与 nebula_mono_mem.log 对表）

static void *watchdog_thread(void *arg) {
    (void)arg;
    // [R65] 看门狗改为【纯 RSS 监控 + 文件记录】，不再参与 mono GC。
    //
    // 根因（R64e 真机崩溃定位）：本线程是裸 pthread，常驻 usleep（非 coop GC 安全点）。
    // 旧版在 g_mono_ready 后置位后 mono_thread_attach 把自己登记成 GC 参与线程；
    // boot 线程在 boot_bepinex 里 JIT 编译 MonoMod 代理方法（ldstr 取字符串常量）触发
    // sgen 分配 / 小GC 时，coop 的 stop-the-world 永远无法让常驻 usleep 的看门狗就范，
    // 扫描其栈越界 → 堆损坏 → 后续任意 sgen 分配在 sgen_try_alloc_obj_nolock 里
    // str x19,[x21,#0x40] SIGSEGV（R34 注释警告的同类问题，只是从"看门狗自己 collect 崩"
    // 变成"看门狗参与 GC 导致主线程分配崩"）。
    //
    // 实测崩时 RSS≈145MB，远低于软/硬顶(1100/1500MB)，看门狗从未真正触发过回收——
    // 它的 mono_thread_attach 纯属副作用。删掉 attach/collect 后：boot 线程自身的
    // phase1/phase2 mono_gc_collect 仍保留（此时扫描面只剩 boot 主线程自己，安全），
    // 托管代码运行期 mono 自带的代际 GC 也照常工作，jetsam 防护不缺失。
    clock_gettime(CLOCK_MONOTONIC, &g_wd_t0);
    while (watchdog_run) {
        long mb = rss_mb();
        if (mb > 0 && g_logfp) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long el = (now.tv_sec - g_wd_t0.tv_sec) * 1000 + (now.tv_nsec - g_wd_t0.tv_nsec) / 1000000;
            // [R65] 只记录 RSS（写文件 + fflush），不再调 mono_gc_collect：本线程不登记
            // 为 GC 参与线程，boot 线程触发的任何 GC 扫描面里就没有这个"永不在安全点"的线程。
            fprintf(g_logfp, "[host][mem][t+%ldms] RSS=%ldMB soft=%d hard=%d ready=%d\n",
                    el, mb, CFG.gc_soft_limit_mb, CFG.gc_hard_limit_mb, g_mono_ready);
            fflush(g_logfp);
        }
        long ms = CFG.gc_watchdog_interval_ms > 0 ? CFG.gc_watchdog_interval_ms : 500;
        usleep((useconds_t)(ms * 1000));
    }
    return NULL;
}

static void start_watchdog(void) {
    static int started = 0;
    if (started) return;   // 幂等：boot_thread 已启动，nebula_mono_init 再调不重复起线程
    started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_thread, NULL) == 0) pthread_detach(t);
}

// ---------------------------------------------------------------- 路径与 boot

static char *module_dir(void) {
    Dl_info info;
    if (dladdr((void *)&module_dir, &info) && info.dli_fname) {
        char buf[PATH_MAX];
        realpath(info.dli_fname, buf);
        char *p = strrchr(buf, '/');
        if (p) *p = 0;
        return strdup(buf);
    }
    return strdup(".");
}

// 组装 MONO_PATH：仅保留用户内容目录作为【文件兜底】（BCL/BepInEx.core 已在 blob 里内存加载）
static void setup_mono_path(const char *dir, const char *root) {
    (void)dir;  // dir(=dylib 目录) 不再含 bcl，仅作占位
    char core[PATH_MAX], interop[PATH_MAX], plugins[PATH_MAX];
    char nebula[PATH_MAX], patchers[PATH_MAX], libs[PATH_MAX];
    snprintf(core,     sizeof(core),     "%s/BepInEx/core", root);
    snprintf(interop,  sizeof(interop),  "%s/BepInEx/interop", root);
    snprintf(plugins,  sizeof(plugins),  "%s/BepInEx/plugins", root);
    snprintf(nebula,   sizeof(nebula),   "%s/BepInEx/nebula", root);
    snprintf(patchers, sizeof(patchers), "%s/BepInEx/patchers", root);
    snprintf(libs,     sizeof(libs),     "%s/NebulaLibs", root);

    const char *all[] = { core, interop, plugins, nebula, patchers, libs };
    char mp[8192]; mp[0] = '\0';
    for (int i = 0; i < 6; i++) {
        if (strlen(mp) + strlen(all[i]) + 2 >= sizeof(mp)) break;
        if (mp[0]) strcat(mp, ":");
        strcat(mp, all[i]);
    }
    setenv("MONO_PATH", mp, 1);
    printf("[host] MONO_PATH(文件兜底)=%s\n", mp);
}

// 前向声明：托管异常转可读文本（定义见下方 boot_bepinex 之后）
static void nebula_log_exception(MonoObject *exc);
static void nebula_fix_platform(MonoDomain *domain);
static void nebula_log_paths(MonoDomain *domain);
// 前向声明（R43）：查证 Preloader 是否真的成功（Doorstop 会吞异常，见下方定义处注释）
static int nebula_preloader_failed(const char *root);
// 前向声明（R44）：把 LogOutput.log 里 interop 相关的行抓进 host 日志
static void nebula_grep_file(const char *path, const char *const keys[], int nkeys, int maxHits, long offset);

// 强制把 MonoMod 的 PlatformHelper.Current 覆写为 iOS。
// 根因（读 BepInEx / MonoMod 源码确认）：嵌入式 Mono 把 iOS 报成 MacOSX；
// PlatformUtils.SetPlatform() 据此把 PlatformHelper.Current 设成 MacOS，而 iOS 检测分支
// （要求 current 先为 Unix）被跳过。于是 Paths.SetExecutablePath 走 MacOS 分支的
// ParentDirectory(exe,4) 上跳 4 级，GameRootPath 落到 /private/var/containers 之类，
// AmongUs.app/Data 自然找不到 → 抛 DirectoryNotFoundException。直接覆写 Current=iOS，
// 让 SetExecutablePath 走非 MacOS 分支（GameRootPath = exe 所在目录 = AmongUs.app）。
//
// 注：以下 mono API 在部分 iOS mono 构建的头里未导出声明，这里补 extern（函数本体在 libmono 中存在）。
// 两套头（iOS 自定义 mono vs macOS mono 6.14.1）对内部结构体/原型的可见性不一致，这里统一用
// 前向声明 + 与 macOS reflection.h 一致的原型，使其在两种编译环境下都不冲突、不隐式声明。
typedef struct _MonoMethodDesc MonoMethodDesc;
typedef struct _MonoReflectionType MonoReflectionType;
extern MonoType *mono_class_get_type(MonoClass *klass);
// mono_type_get_object：macOS reflection.h 声明返回 MonoReflectionType*；iOS 头里未声明。
// 统一按 MonoReflectionType* 声明（与 macOS 一致，iOS 作为唯一声明），调用处再向上转型 MonoObject*。
extern MonoReflectionType *mono_type_get_object(MonoDomain *domain, MonoType *type);
extern MonoMethodDesc *mono_method_desc_new(const char *name, int include_namespace);
extern MonoMethod *mono_method_desc_search_in_class(MonoMethodDesc *desc, MonoClass *klass);
extern void mono_method_desc_free(MonoMethodDesc *desc);
extern char *mono_string_to_utf8(MonoString *s);

static void nebula_fix_platform(MonoDomain *domain) {
    if (!domain) return;
    const blob_entry *mm_e = blob_find("MonoMod.Utils");
    if (!mm_e) { fprintf(stderr, "[host] 找不到 MonoMod.Utils blob，无法纠正平台\n"); return; }
    MonoAssembly *mma = blob_load_assembly(mm_e);
    if (!mma) { fprintf(stderr, "[host] 加载 MonoMod.Utils 失败\n"); return; }
    MonoImage *mm = mono_assembly_get_image(mma);
    MonoClass *ph = mono_class_from_name(mm, "MonoMod.Utils", "PlatformHelper");
    if (!ph) { fprintf(stderr, "[host] 找不到 PlatformHelper\n"); return; }

    // 取 Platform 枚举的 iOS 值：Enum.Parse(typeof(Platform), "iOS")
    MonoClass *penum = mono_class_from_name(mm, "MonoMod.Utils", "Platform");
    if (!penum) { fprintf(stderr, "[host] 找不到 Platform 枚举\n"); return; }
    MonoClass *enumCls = mono_class_from_name(mono_get_corlib(), "System", "Enum");
    if (!enumCls) { fprintf(stderr, "[host] 找不到 System.Enum\n"); return; }
    MonoMethodDesc *desc = mono_method_desc_new("System.Enum:Parse(System.Type,System.String)", TRUE);
    MonoMethod *parse = desc ? mono_method_desc_search_in_class(desc, enumCls) : NULL;
    if (desc) mono_method_desc_free(desc);
    if (!parse) { fprintf(stderr, "[host] 找不到 Enum.Parse(Type,string)\n"); return; }

    MonoType *ptype = mono_class_get_type(penum);
    MonoReflectionType *rt = mono_type_get_object(domain, ptype);
    MonoString *ios = mono_string_new(domain, "iOS");
    void *pargs[2] = { (MonoObject *)rt, ios };
    MonoObject *exc = NULL;
    MonoObject *iosVal = mono_runtime_invoke(parse, NULL, pargs, &exc);
    if (exc || !iosVal) {
        nebula_log_exception(exc);
        fprintf(stderr, "[host] Enum.Parse(Platform,iOS) 失败\n");
        return;
    }

    // PlatformHelper.Current = iosVal（属性 setter，mono 不强制可见性）
    MonoProperty *curP = mono_class_get_property_from_name(ph, "Current");
    MonoMethod *setCur = curP ? mono_property_get_set_method(curP) : NULL;
    if (!setCur) { fprintf(stderr, "[host] 找不到 PlatformHelper.Current setter\n"); return; }
    void *sargs[1] = { iosVal };
    mono_runtime_invoke(setCur, NULL, sargs, &exc);
    if (exc) { nebula_log_exception(exc); fprintf(stderr, "[host] 设置 PlatformHelper.Current=iOS 失败\n"); }
    else     { printf("[host] 已强制 PlatformHelper.Current=iOS（绕过 MacOS 误判）\n"); }
    fflush(NULL);
}

// 诊断：读取并打印 BepInEx.Paths.GameRootPath / GameDataPath。
// 用于判定 SetExecutablePath 之后 Paths 的真实落点：
//   - GameRootPath 若变成 /private/var/containers 之类 → 说明仍中 MacOS 分支（平台覆盖被 SetPlatform 重置）
//   - GameRootPath=AmongUs.app 但 GameDataPath=null → 说明 Data 在 .NET 下不可见（路径/沙盒问题）
static void nebula_log_paths(MonoDomain *domain) {
    if (!domain) return;
    const blob_entry *core_e = blob_find("BepInEx.Core");
    if (!core_e) return;
    MonoAssembly *core_as = blob_load_assembly(core_e);
    if (!core_as) return;
    MonoImage *img = mono_assembly_get_image(core_as);
    MonoClass *paths = mono_class_from_name(img, "BepInEx", "Paths");
    if (!paths) return;

    MonoProperty *pGR = mono_class_get_property_from_name(paths, "GameRootPath");
    MonoProperty *pGD = mono_class_get_property_from_name(paths, "GameDataPath");
    MonoMethod *gGR = pGR ? mono_property_get_get_method(pGR) : NULL;
    MonoMethod *gGD = pGD ? mono_property_get_get_method(pGD) : NULL;
    MonoObject *exc = NULL;
    if (gGR) {
        MonoObject *gr = mono_runtime_invoke(gGR, NULL, NULL, &exc);
        if (gr) { char *c = mono_string_to_utf8((MonoString *)gr);
                  printf("[host] Paths.GameRootPath = %s\n", c ? c : "(null)"); if (c) free(c); }
        else fprintf(stderr, "[host] 读 GameRootPath 失败（未初始化？）\n");
        exc = NULL;
    }
    if (gGD) {
        MonoObject *gd = mono_runtime_invoke(gGD, NULL, NULL, &exc);
        if (gd) { char *c = mono_string_to_utf8((MonoString *)gd);
                  printf("[host] Paths.GameDataPath = %s\n", c ? c : "(null)"); if (c) free(c); }
        else fprintf(stderr, "[host] 读 GameDataPath 失败（未初始化？）\n");
    }
    fflush(NULL);
}

// ---------------------------------------------------------------- R47：iOS IL2CPP 布局校正
// interop 生成的两个输入路径，BepInEx 6 全是按 PC/安卓那一套硬写的，在 iOS 必然指错：
//
//  1) Il2CppInteropManager.get_GameAssemblyPath()
//        = Path.Combine(Paths.GameRootPath, "GameAssembly." + PlatformHelper.LibrarySuffix)
//     Windows 上是 GameAssembly.dll、Linux/Android 上是 GameAssembly.so —— 但 iOS 的
//     IL2CPP 是【静态链进 Frameworks/UnityFramework.framework/UnityFramework】的，
//     整个 app bundle 里根本没有 GameAssembly 这个文件。Cpp2IL 拿到不存在的路径，
//     InitializeLibCpp2Il 直接抛 FileNotFound，异常被 GenerateInteropAssemblies 的
//     catch 吞掉，最后表现成下游 Il2Cppmscorlib 加载失败，完全看不出是路径错了。
//     好在那个 getter 优先读环境变量 BEPINEX_GAME_ASSEMBLY_PATH —— 我们在 Start() 之前
//     探出来 setenv 即可，一行代码、完全不用改 IL。
//
//  2) [IL2CPP] GlobalMetadataPath 默认 = "{GameDataPath}/il2cpp_data/Metadata/global-metadata.dat"
//     这是安卓布局。iOS 的落点各家 Unity 版本不同（Data/Metadata 与
//     Data/Managed/Metadata 都见过），这里运行时扫出来，写进 BepInEx.cfg 纠正。
//
// 两者都做成"找到才改、找不到就什么都不做"，绝不因为探测失败把原来能跑的路堵死。
static int neb_is_file(const char *p) {
    struct stat st;
    return (stat(p, &st) == 0 && S_ISREG(st.st_mode));
}

// mkdir 只建一层，父级不存在就会失败（iOS 上 app bundle 与容器目录父级情况不一），这里递归建。
static int neb_mkdir_p(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    char tmp[PATH_MAX];
    if (strlen(path) >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash && slash != tmp) {
        *slash = 0;
        if (neb_mkdir_p(tmp) != 0) return -1;
    }
    return (mkdir(path, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

// 有界深度递归找文件名（只找文件，不找目录），命中返回 1 并把绝对路径写进 out
static int neb_find_file_rec(const char *dir, const char *fname, int depth, char *out, size_t osz) {
    if (depth < 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    char path[PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_REG) {
            if (!strcasecmp(e->d_name, fname)) {
                snprintf(out, osz, "%s", path);
                closedir(d);
                return 1;
            }
        } else if (e->d_type == DT_DIR) {
            if (neb_find_file_rec(path, fname, depth - 1, out, osz)) { closedir(d); return 1; }
        }
    }
    closedir(d);
    return 0;
}

// 往 BepInEx.cfg 里补/改一个 key（section 不存在则新建；key 已存在则原样保留，
// 避免覆盖用户手改的配置）
// [R48] replace_if_bad：原有值如果是空、或它指向的文件根本不存在（典型是 BepInEx 自己
// 落盘的默认值，比如 GlobalMetadataPath 的安卓布局路径 / UnityBaseLibrariesSource 的
// 联网 URL —— 后者在本机 zlib 封送坏掉的前提下必崩），就强制覆盖；否则保留用户手改。
static void neb_cfg_set(const char *cfgpath, const char *sec, const char *key, const char *val,
                        int replace_if_bad) {
    FILE *in = fopen(cfgpath, "r");
    char line[4096];
    int replace = 1;            // 没有 cfg 就是首次，直接写
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char t[4096]; snprintf(t, sizeof(t), "%s", line);
            char *eq = strchr(t, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = t; while (*k == ' ' || *k == '\t') k++;
            char *e = eq - 1; while (e > k && (*e == ' ' || *e == '\t')) e--;
            *(e + 1) = 0;
            if (strcasecmp(k, key)) continue;
            // 取原值判断是否"坏"
            char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
            char *nl = strpbrk(v, "\r\n"); if (nl) *nl = 0;
            int bad = 1;
            if (!replace_if_bad) bad = 0;
            else if (*v && !strcasecmp(v, val)) bad = 0;   // 已经是目标值，无需再动
            else if (*v) bad = !neb_is_file(v);            // 非空且是真实存在的文件才算"好"
            if (bad) replace = 1;
            else {
                replace = 0;
                printf("[host][r48] cfg 保留可用的 %s = %s -> %s\n", key, v, cfgpath);
            }
            break;
        }
        rewind(in);
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfgpath);
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    if (!replace) {
        printf("[host][r48] cfg 不改动 %s: %s\n", key, cfgpath);
        if (in) fclose(in);
        fclose(out); remove(tmp); return;
    }
    if (!in) {
        fprintf(out, "[%s]\n%s = %s\n", sec, key, val);
    } else {
        int in_sec = 0, wrote = 0;
        char sec_hdr[256]; snprintf(sec_hdr, sizeof(sec_hdr), "[%s]", sec);
        while (fgets(line, sizeof(line), in)) {
            char t[4096]; snprintf(t, sizeof(t), "%s", line);
            char *eq = strchr(t, '=');
            if (eq && !wrote) {
                char *before = t; while (*before == ' ' || *before == '\t') before++;
                if (before[0] != '[') {          // 段头行不参与 key 匹配
                    *eq = 0;
                    char *k = t; while (*k == ' ' || *k == '\t') k++;
                    char *e = eq - 1; while (e > k && (*e == ' ' || *e == '\t')) e--;
                    *(e + 1) = 0;
                    if (!strcasecmp(k, key)) {   // 原地改写这一行的值
                        fprintf(out, "%s = %s\n", key, val);
                        wrote = 1;
                        continue;
                    }
                }
            }
            char *p = t; while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '[') {
                if (in_sec && !wrote) { fprintf(out, "%s = %s\n", key, val); wrote = 1; }
                in_sec = (!strncasecmp(p, sec_hdr, strlen(sec_hdr)));
            }
            fprintf(out, "%s", line);
        }
        if (in_sec && !wrote) fprintf(out, "%s = %s\n", key, val);
        else if (!in_sec) fprintf(out, "\n[%s]\n%s = %s\n", sec, key, val);
    }
    if (in) fclose(in);
    fclose(out);
    rename(tmp, cfgpath);
    printf("[host][r48] cfg 已写入 %s.%s = %s -> %s\n", sec, key, val, cfgpath);
}

// [R48] 把 blob 里某个前缀下的文件原样释放到 dest（缺哪个写哪个，已存在且大小一致则跳过）。
// 用来投递 Unity 基础库：unity-libs/*.dll 必须【先落盘】成了散文件，BepInEx 才会拿它们
// 当 Cecil 参考程序集（AddCecilPlatformAssemblies 只读文件系统，不认内存程序集）。
static int nebula_blob_extract_dir(const char *prefix, const char *dest) {
    if (!g_n) return 0;
    struct stat st;
    if (stat(dest, &st) != 0 && neb_mkdir_p(dest) != 0) return 0;
    size_t plen = strlen(prefix);
    int written = 0, skipped = 0;
    for (int i = 0; i < g_n; i++) {
        const char *nm = g_entries[i].name;
        if (strncmp(nm, prefix, plen)) continue;
        const char *bn = strrchr(nm, '/'); bn = bn ? bn + 1 : nm;
        if (!*bn) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", dest, bn);
        if (stat(p, &st) == 0 && st.st_size == (off_t)g_entries[i].size) { skipped++; continue; }
        FILE *f = fopen(p, "wb");
        if (!f) continue;
        if (fwrite(g_entries[i].data, 1, g_entries[i].size, f) != g_entries[i].size) {
            fclose(f); remove(p); continue;
        }
        fclose(f);
        written++;
    }
    if (written || skipped)
        printf("[host][r48] 释放 %s -> %s （新写 %d，已存在跳过 %d）\n", prefix, dest, written, skipped);
    return written;
}

/* [R60] 取本 dylib 自身的绝对路径。
   dllmap 的 target 指向它：dlopen(自身) 会返回已加载镜像的 handle，随后 dlsym
   即可命中静态链进来的 DobbyHook / SystemNative_* 符号。这样所有原生库都住在
   同一个 Mach-O 里，成品就一个 libnebula_standard.dylib。 */
static char g_r60_self_path[PATH_MAX] = {0};
static const char *nebula_r60_self_path(void) {
#ifdef __APPLE__
    if (g_r60_self_path[0]) return g_r60_self_path;
    Dl_info info;
    if (dladdr((void *)blob_load, &info) && info.dli_fname) {
        snprintf(g_r60_self_path, sizeof(g_r60_self_path), "%s", info.dli_fname);
        return g_r60_self_path;
    }
#endif
    return NULL;
}

/* [R60] 把 dylib 内嵌的 interop/ 与 native/ 资源释放到磁盘（blob_load 之后立刻调用）。

   interop：R59 只做到"随包发布"，用户仍要手动解压 zip 放进 BepInEx/interop/。
    一旦目录为空，BepInEx 的 Il2CppInteropManager 会无视
    UpdateInteropAssemblies=false 而【现场生成】（R58 真机整夜卡死/黑屏）——
    这正是用户反馈的"不手动放 interop 他还会生成"。R60 直接从 dylib 内嵌
    释放 131 个预生成 iOS 桥接 dll，目录永不为空 → 用户零操作、设备端永不生成。

   native：CoreLib P/Invoke shim 的 fallback。主路径是静态链进主 dylib；
    若 CI 的 Mono prefix 没带 libSystem.*.a，则宿主释放这里的 dylib 字节到
    可写目录（Documents/nebula_native/）再 dllmap 过去 —— 即用户说的
    「表面上一个 dylib，启动自动在内存里分裂回来」。 */
static void nebula_r60_extract_bundle(const char *root) {
    if (!g_n) return;
    char dest[PATH_MAX];
    const char *home = getenv("HOME");

    // 投到所有可能的 BepInEx 基准目录：游戏根（用户放包处）+ Documents（BepInEx Paths 实际用）
    const char *bases[2]; int nb = 0;
    if (root && root[0]) bases[nb++] = root;
    if (home && home[0]) { static char doc[PATH_MAX]; snprintf(doc, sizeof(doc), "%s/Documents", home); bases[nb++] = doc; }
    for (int i = 0; i < nb; i++) {
        snprintf(dest, sizeof(dest), "%s/BepInEx/interop", bases[i]);
        int n = nebula_blob_extract_dir("interop/", dest);
        printf("[host][r60] 预生成 interop -> %s（本次新写 %d，已存在则跳过）\n", dest, n);
    }
    // native shim 只投可写目录（app bundle 只读，写不进去也无害）
    if (home && home[0]) {
        snprintf(dest, sizeof(dest), "%s/Documents/nebula_native", home);
        int n = nebula_blob_extract_dir("native/", dest);
        printf("[host][r60] native shim fallback -> %s（本次新写 %d）\n", dest, n);
    }
}

// 探 IL2CPP 二进制 + global-metadata.dat，并落地纠偏
/* [R53] 只探测、绝不调用 il2cpp C API。
   宿主 boot() 是阻塞式 constructor：主线程泵着 CFRunLoop 死等 boot_thread 结束，
   Unity 主线程此时还没跑 il2cpp_init。任何在此刻发生的 il2cpp_domain_get()
   都会在 native 里解引用空 thread/domain 直接 SIGSEGV —— R52 设备日志实证：
       at Il2CppInterop.Runtime.IL2CPP:il2cpp_domain_get <0x00007>
       at Il2CppInterop.Runtime.IL2CPP:.cctor <0x0006f>
       at ...IL2CPPLogSource:.ctor <0x00113>
       at ...IL2CPPChainloader:InitializeLoggers <0x000bf>
   所以这里只 dlsym 看导出是否可见，不做 domain / thread 状态探测。
   R53 的修复在托管侧：IL2CPPLogSource..ctor 不再注册 il2cpp log callback，
   让 IL2CPP.cctor 的首次触发推迟到 Unity 主线程第一次 il2cpp_runtime_invoke
   （那时 VM 必然已 init，ourImagesMap 才能正常填充）。*/
static void nebula_r53_probe_il2cpp(const char *uf) {
    void *dg  = dlsym(RTLD_DEFAULT, "il2cpp_domain_get");
    void *ta  = dlsym(RTLD_DEFAULT, "il2cpp_thread_attach");
    void *ini = dlsym(RTLD_DEFAULT, "il2cpp_init");
    void *ri  = dlsym(RTLD_DEFAULT, "il2cpp_runtime_invoke");
    void *h = NULL;
    if (!ri) {   // 全局命名空间没看到就按 BepInEx 同款方式 dlopen 后再查
        h = dlopen(uf, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            if (!dg)  dg  = dlsym(h, "il2cpp_domain_get");
            if (!ta)  ta  = dlsym(h, "il2cpp_thread_attach");
            if (!ini) ini = dlsym(h, "il2cpp_init");
            if (!ri)  ri  = dlsym(h, "il2cpp_runtime_invoke");
        }
    }
    printf("[host][r53] IL2CPP C API 导出探测(只查不调): domain_get=%p thread_attach=%p init=%p runtime_invoke=%p handle=%p\n",
           dg, ta, ini, ri, h);
    printf("[host][r53] 版本策略: preloader 阶段不注册 il2cpp log callback，IL2CPP.cctor 首次触发推迟到 Unity 主线程首次 il2cpp_runtime_invoke(VM 已 init)\n");
    /* 不 dlclose：UnityFramework 由 app 持有，多一次引用无害；误卸载反而灾难 */
}

static void nebula_fix_il2cpp_paths(const char *root) {
    char app[PATH_MAX]; app[0] = 0;
#if defined(__APPLE__)
    char exep[PATH_MAX]; uint32_t sz = (uint32_t)sizeof(exep);
    if (_NSGetExecutablePath(exep, &sz) == 0) {
        char *slash = strrchr(exep, '/');
        if (slash) *slash = 0;
        snprintf(app, sizeof(app), "%s", exep);   // iPhone App.Bundle 的根（含 Data/ 与 Frameworks/）
    }
#endif
    printf("[host][r47] app bundle=%s | root=%s\n", app[0] ? app : "(未知)", root ? root : "(null)");

    const char *bases[3]; int nb = 0;
    if (app[0])  bases[nb++] = app;
    if (root && (!app[0] || strcmp(root, app))) bases[nb++] = root;
    {
        const char *home = getenv("HOME");
        if (home && home[0]) { static char doc[PATH_MAX]; snprintf(doc, sizeof(doc), "%s/Documents", home); bases[nb++] = doc; }
    }

    // ---- 1) 承载 IL2CPP 的二进制 ----
    static const char *const bin_rel[] = {
        "/Frameworks/UnityFramework.framework/UnityFramework",
        "/GameAssembly.dylib",
        "/GameAssembly.dll",
        "/GameAssembly.so",
    };
    char hit[PATH_MAX]; int found_bin = 0;
    for (int i = 0; i < nb && !found_bin; i++) {
        for (unsigned j = 0; j < sizeof(bin_rel) / sizeof(bin_rel[0]); j++) {
            snprintf(hit, sizeof(hit), "%s%s", bases[i], bin_rel[j]);
            if (neb_is_file(hit)) {
                struct stat st; long long sz = stat(hit, &st) == 0 ? (long long)st.st_size : -1;
                printf("[host][r47] IL2CPP 二进制命中: %s (%lld bytes)\n", hit, sz);
                setenv("BEPINEX_GAME_ASSEMBLY_PATH", hit, 1);
                printf("[host][r47]   已 setenv BEPINEX_GAME_ASSEMBLY_PATH（BepInEx 那个 getter 优先读它）\n");
                found_bin = 1;
                break;
            }
        }
    }
    if (!found_bin) {
        printf("[host][r47] !! 没找到 IL2CPP 二进制（UnityFramework / GameAssembly 都没有），不做纠偏\n");
    } else {
        // [R53] 只打印导出符号地址，绝不调用（VM 此刻尚未 init）
        nebula_r53_probe_il2cpp(hit);
    }

    // ---- 2) global-metadata.dat ----
    static const char *const md_rel[] = {
        "/Data/Managed/Metadata/global-metadata.dat",
        "/Data/Metadata/global-metadata.dat",
        "/Data/il2cpp_data/Metadata/global-metadata.dat",
        "/Data/Raw/Metadata/global-metadata.dat",
        "/Metadata/global-metadata.dat",
    };
    char md[PATH_MAX]; int found_md = 0;
    for (int i = 0; i < nb && !found_md; i++)
        for (unsigned j = 0; j < sizeof(md_rel) / sizeof(md_rel[0]); j++) {
            snprintf(md, sizeof(md), "%s%s", bases[i], md_rel[j]);
            if (neb_is_file(md)) { found_md = 1; break; }
        }
    if (!found_md) {   // 兜底：在 Data/ 下递归扫（Unity 各版本落点不一致，最多下钻 6 层）
        for (int i = 0; i < nb && !found_md; i++) {
            char dpath[PATH_MAX];
            snprintf(dpath, sizeof(dpath), "%s/Data", bases[i]);
            if (neb_find_file_rec(dpath, "global-metadata.dat", 6, md, sizeof(md))) found_md = 1;
        }
    }
    if (found_md) {
        struct stat st; long long sz = stat(md, &st) == 0 ? (long long)st.st_size : -1;
        printf("[host][r47] global-metadata.dat 命中: %s (%lld bytes)\n", md, sz);
        // 写进每一份可能生效的 BepInEx.cfg（BepInExRootPath 在 iOS 上可能被重定向到容器目录）
        for (int i = 0; i < nb; i++) {
            char cfgd[PATH_MAX], cfgp[PATH_MAX + 64];
            snprintf(cfgd, sizeof(cfgd), "%s/BepInEx/config", bases[i]);
            snprintf(cfgp, sizeof(cfgp), "%s/BepInEx.cfg", cfgd);
            // 目录不存在就建（幂等，无害）；失败说明这里不可写，跳过
            if (neb_mkdir_p(cfgd) != 0) continue;
            // replace_if_bad=1：默认值（安卓布局那一串）根本不存在，必须被覆盖
            neb_cfg_set(cfgp, "IL2CPP", "GlobalMetadataPath", md, 1);
        }
    } else {
        printf("[host][r47] !! 没找到 global-metadata.dat，保留 BepInEx 默认路径\n");
    }

    // ---- 3) unity-libs：Il2CppInterop 生成器当参考程序集用的 Unity 托管基础库 ----
    // R48 背景（preloader.log 实锤）：DownloadUnityAssemblies() 联网拉 zip 成功，但解 zip 时
    //   System.IO.Compression.ZLibException: The underlying compression routine could not be loaded
    //   ---> System.ApplicationException: Cannot marshal nonblittlable types without marshal-ilgen
    // 这套 Mono 的 zlib 封送需要运行时生成 IL（第三方 zlib 的 version 字符串参数非 blittable），
    // 本环境不可用 —— 所以必须把这条路彻底绕开。做法：
    //   DownloadUnityAssemblies() 开头若发现 UnityBaseLibrariesSource 为空就直接 return，
    //   而"删光 unity-libs/*.dll 再解 zip"在这行之后 —— 把 source 置空，既跳过联网、
    //   也保住你已经放好的 dll（否则下次启动会被它先清空，再解压失败）。
    for (int i = 0; i < nb; i++) {
        char ul[PATH_MAX], cfgd[PATH_MAX], cfgp[PATH_MAX + 64];
        snprintf(ul,   sizeof(ul),   "%s/BepInEx/unity-libs", bases[i]);
        snprintf(cfgd, sizeof(cfgd), "%s/BepInEx/config",     bases[i]);
        snprintf(cfgp, sizeof(cfgp), "%s/BepInEx.cfg",        cfgd);
        struct stat st;
        int ul_ok = (stat(ul, &st) == 0 && S_ISDIR(st.st_mode));

        // 3a) 缺的话先从 dylib 里带的 unity-libs blob 补上（幂等：大小一致就跳过）
        if (nebula_blob_extract_dir("unity-libs/", ul) > 0) ul_ok = 1;
        if (!ul_ok) continue;

        int ndll = 0, nzip = 0;
        DIR *d = opendir(ul);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                size_t L = strlen(e->d_name);
                if (L > 4 && !strcasecmp(e->d_name + L - 4, ".dll")) ndll++;
                if (L > 4 && !strcasecmp(e->d_name + L - 4, ".zip")) nzip++;
            }
            closedir(d);
        }
        printf("[host][r48] unity-libs 现状: %s （dll=%d zip=%d）\n", ul, ndll, nzip);
        // 3b) 有可用基础库就强制让 BepInEx 跳过 download+unzip 那一段
        if (ndll > 0) {
            if (neb_mkdir_p(cfgd) != 0) continue;
            // replace_if_bad=1：只要原值不是"真实存在的文件"就覆盖（URL 天然不满足 → 被置空）
            neb_cfg_set(cfgp, "IL2CPP", "UnityBaseLibrariesSource", "", 1);
            printf("[host][r48]   unity-libs 已有 %d 个 dll -> 令 BepInEx 跳过联网下载/解压\n", ndll);
        }
        // 3c) [R50] 关掉 Cpp2IL 的 xref 扫描 —— interop 生成最大的耗时大头。
        // ScanMethodRefs 默认取 Environment.Is64BitProcess（真机恒 true）：开启后
        // Cpp2IL 要对 96MB 的 UnityFramework 做全量方法交叉引用分析与 xref 图构建，
        // 在 iPad 的 Mono JIT 下实测能把生成拖到 20 分钟以上、进程还活着却像卡死。
        // 关掉只损失 CallerCount 特性与"死方法"判定，类型/方法本体照样全生成。
        if (neb_mkdir_p(cfgd) != 0) continue;
        neb_cfg_set(cfgp, "IL2CPP", "ScanMethodRefs", "false", 1);

        // 3d) [R60] 设备端【永不生成】interop —— iOS 上跑 Cpp2IL 分析那颗 ~96MB 的
        //     UnityFramework 二进制会卡死/超慢（R58 真机实测整夜跑不完，t+31700s 仍未结束）。
        //     R59 只做到"随包发布"，用户仍要手动放；且一旦 BepInEx/interop 为空，
        //     Il2CppInteropManager 会无视 UpdateInteropAssemblies=false 触发现场生成
        //     （用户实测"不手动放他还会生成"）。R60：interop 已由 dylib 内嵌，
        //     boot 早期 nebula_r60_extract_bundle() 自动释放，这里再兜底补一次，
        //     保证目录恒非空 → 用户零操作、设备端永不生成。
        //     绝不备份/移除/重生成（R58 的"备份 + 强制重生成"与 R52 的"删哈希"一并废除）。
        {
            char ip[PATH_MAX];
            snprintf(ip, sizeof(ip), "%s/BepInEx/interop", bases[i]);
            /* 幂等兜底：已存在且大小一致就跳过，不覆盖不删除 */
            nebula_blob_extract_dir("interop/", ip);
            struct stat ist;
            int nip = 0;
            if (stat(ip, &ist) == 0 && S_ISDIR(ist.st_mode)) {
                DIR *dd = opendir(ip);
                if (dd) {
                    struct dirent *e;
                    while ((e = readdir(dd)) != NULL) {
                        size_t L = strlen(e->d_name);
                        if (L > 4 && !strcasecmp(e->d_name + L - 4, ".dll")) nip++;
                    }
                    closedir(dd);
                }
            }
            printf("[host][r60] interop 现状: %s （dll=%d）\n", ip, nip);

            if (neb_mkdir_p(cfgd) != 0) continue;
            if (nip > 0) {
                // 预生成 interop 已就位：直接复用，绝不在设备端重新生成
                neb_cfg_set(cfgp, "IL2CPP", "UpdateInteropAssemblies", "false", 1);
                printf("[host][r60] interop 已就位(%d dll)，设 UpdateInteropAssemblies=false 直接复用（设备端不生成、无需手动放）\n", nip);
            } else {
                // 极端情况（blob 没带 interop）：仍不生成（会卡死），明确报错，游戏原版可玩
                printf("[host][r60] 警告: BepInEx/interop 仍为空 —— 本 dylib 未内嵌 interop，NOS 无法加载\n");
                neb_cfg_set(cfgp, "IL2CPP", "UpdateInteropAssemblies", "false", 1);
            }
        }
    }
    fflush(NULL);
}

// 自动算对（iOS 被 PlatformUtils.SetPlatform 识别为 Platform.iOS → 走非 MacOS 分支）。
// 关键发现（读 BepInEx 6 源码确认）：
//   - BepInExRootPath 由 DOORSTOP_INVOKE_DLL_PATH 经 Utility.ParentDirectory(.,2) 决定，必须落在
//     【可写容器目录】，否则 iOS 只读 bundle 会让 BepInEx 写日志/配置失败；此前抛
//     UnauthorizedAccessException，且被 Start() 的 catch 写日志再次掩盖真实错误。
//   - GameRootPath 由 DOORSTOP_PROCESS_PATH 的所在目录决定（= root / AmongUs.app）。
//   - Start() 的 catch 会把真实异常写到 BEPINEX_PRELOADER_LOG；设为绝对可写路径，避免它落到
//     只读 bundle 抛 UnauthorizedAccessException 把真错误彻底掩盖。
// 我们【不再】主动调用 SetExecutablePath（那会在 SetPlatform() 之前误中 MacOS 分支），而是把参数
// 通过环境变量交给 PreloaderMain 内部，由它用正确的 iOS 分支计算 Paths。
static int nebula_init_bepinex_paths(MonoDomain *domain, const char *root) {
    (void)domain;
    if (!root) return 0;

    const char *home = getenv("HOME");
    if (!home || !home[0]) home = root;   // 兜底：iOS 一定有 HOME

    char bepinroot[PATH_MAX], invoke[PATH_MAX], prelog[PATH_MAX], exepath[PATH_MAX];
    snprintf(bepinroot, sizeof(bepinroot), "%s/Documents/BepInEx", home);
    snprintf(invoke,    sizeof(invoke),    "%s/core/BepInEx.Preloader.dll", bepinroot);
    snprintf(prelog,    sizeof(prelog),    "%s/Documents/preloader.log", home);
    snprintf(exepath,   sizeof(exepath),   "%s/AmongUs", root);   // GameRootPath = root(AmongUs.app)

    // 创建可写 BepInEx 目录结构：BepInEx 会在其下写 config / plugins / log / cache 等
    mkdir(bepinroot, 0755);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/core", bepinroot);     mkdir(tmp, 0755);
    snprintf(tmp, sizeof(tmp), "%s/plugins", bepinroot);  mkdir(tmp, 0755);
    snprintf(tmp, sizeof(tmp), "%s/config", bepinroot);   mkdir(tmp, 0755);
    snprintf(tmp, sizeof(tmp), "%s/patchers", bepinroot); mkdir(tmp, 0755);
    snprintf(tmp, sizeof(tmp), "%s/cache", bepinroot);    mkdir(tmp, 0755);

    // 1) DOORSTOP_INVOKE_DLL_PATH → ParentDirectory(.,2) 即 BepInExRootPath（可写，绕过只读 bundle）
    setenv("DOORSTOP_INVOKE_DLL_PATH", invoke, 1);
    // 2) DOORSTOP_PROCESS_PATH → GameRootPath = 其所在目录 = root（AmongUs.app，Data 真实存在）
    setenv("DOORSTOP_PROCESS_PATH", exepath, 1);
    // 3) BEPINEX_PRELOADER_LOG → 绝对可写路径：Start() 的 catch 把真实异常写到这里，
    //    避免落到只读 bundle 抛 UnauthorizedAccessException 把真错误彻底掩盖
    setenv("BEPINEX_PRELOADER_LOG", prelog, 1);

    printf("[host] BepInEx 环境就绪: BepInExRootPath=%s  GameRootPath=%s  prelog=%s\n",
           bepinroot, root, prelog);
    fflush(NULL);
    return 1;
}

// 诊断（R37）：preloader 日志尾部监听线程。
// 背景：Doorstop.Start() 在真机上不会同步返回（日志里从没出现过「Start() 已返回」），
// 导致调用点之后那一整段诊断（含异常类型打印、BepInEx 目录枚举）永远执行不到；
// 而 BepInEx 自己的日志只写文件，文件一旦没生成我们就彻底失明。
// 做法：在调 Start() 之前把这个后台线程拉起来，周期性把 Documents/preloader.log 的
// 增量内容原样转发进 host 日志 —— 这样不管 Start() 卡不卡、文件晚多久才出现，
// BepInEx 的真实报错都能看得见。
static volatile int g_prelog_tail_running = 0;

static void *nebula_prelog_tail_thread(void *arg) {
    (void)arg;
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (home) snprintf(path, sizeof(path), "%s/Documents/preloader.log", home);
    else      snprintf(path, sizeof(path), "/var/mobile/Documents/preloader.log");

    long last = 0;
    int announced = 0;
    // 监听 120 秒，足够覆盖 preloader 启动窗口（每 0.5s 轮询一次）
    for (int i = 0; i < 240 && g_prelog_tail_running; i++) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (!announced) {
                printf("[host][prelog] 等待日志文件出现: %s（暂未创建）\n", path);
                fflush(NULL);
                announced = 1;
            }
            usleep(500 * 1000);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz > last) {
            if (fseek(f, last, SEEK_SET) == 0) {
                char buf[8192];
                while (sz > last) {
                    long want = sz - last;
                    if (want > (long)sizeof(buf) - 1) want = (long)sizeof(buf) - 1;
                    size_t n = fread(buf, 1, (size_t)want, f);
                    if (n <= 0) break;
                    buf[n] = '\0';
                    char *p = buf;
                    while (p && *p) {
                        char *nl = strchr(p, '\n');
                        if (nl) *nl = '\0';
                        printf("[host][prelog] %s\n", p);
                        if (!nl) break;
                        p = nl + 1;
                    }
                    last += (long)n;
                }
                fflush(NULL);
            }
        }
        fclose(f);
        usleep(500 * 1000);
    }
    return NULL;
}

// 诊断（R35）：递归枚举目录，定义在文件下方，先前向声明
static void nebula_list_dir(const char *path, int depth);

// [R45] LogOutput.log 是追加写的，而设备上那份可能是别的机器上拷来的旧产物（本次实测：
// 45KB 内容是 PC 上的日志，iPad 根本没写）。只看本次运行新增的增量，别再把历史内容
// 当成本次输出，那是误导。
static long nebula_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static const char *boot_logoutput_path(void) {
    static char p[PATH_MAX];
    const char *home = getenv("HOME");
    if (home) snprintf(p, sizeof(p), "%s/Documents/BepInEx/LogOutput.log", home);
    else snprintf(p, sizeof(p), "/LogOutput.log");
    return p;
}

/* [R54] 长期日志监听。
   R53 教训：BepInEx 的插件加载发生在 Doorstop.Entrypoint.Start() 返回、
   Unity 切场景（Internal_ActiveSceneChanged）之后；而原来的 preloader tail
   只活 120 秒、LogOutput.log 也只在 Start() 之后 grep 一次关键字。于是
   "Error loading [NebulaLoader]: <异常堆栈>" 这类决定性信息永远看不到。
   这里把 LogOutput.log 的【本次增量全量】持续转发进 host 日志（不再只 grep
   关键字），并顺带监听 managed 侧可选的 nebula_diag.log。
   起点用线程启动时的文件大小，天然跳过 PC 拷来的历史残留。 */
static volatile int g_r54_tail_running = 0;

static long nebula_r54_tail_one(const char *path, long last, const char *prefix, int *budget) {
    if (!path || !*path || *budget <= 0) return last;
    FILE *f = fopen(path, "rb");
    if (!f) return last;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return last; }
    long sz = ftell(f);
    // [R55] 文件被 BepInEx 重写/截断后，从旧 offset 读会漏掉前段新内容
    // （R54 真机正是因此漏掉 [r54] 初始化插桩）。sz < last 即判定文件已重写，offset 归零从头读。
    if (sz < last) {
        if (*budget > 0)
            printf("%s [host][r55] 检测到 %s 被重写(sz=%ld < last=%ld)，offset 归零从头读\n",
                   prefix, path, sz, last);
        last = 0;
    }
    if (sz > last && fseek(f, last, SEEK_SET) == 0) {
        char buf[8192];
        while (sz > last && *budget > 0) {
            long want = sz - last;
            if (want > (long)sizeof(buf) - 1) want = (long)sizeof(buf) - 1;
            size_t n = fread(buf, 1, (size_t)want, f);
            if (n <= 0) break;
            buf[n] = '\0';
            char *p = buf;
            while (p && *p && *budget > 0) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                printf("%s %s\n", prefix, p);
                (*budget)--;
                if (!nl) break;
                p = nl + 1;
            }
            last += (long)n;
        }
        fflush(NULL);
    }
    fclose(f);
    return last;
}

static void *nebula_r54_tail_thread(void *arg) {
    (void)arg;
    const char *home = getenv("HOME");
    char diag[PATH_MAX];
    if (home) snprintf(diag, sizeof(diag), "%s/Documents/nebula_diag.log", home);
    else diag[0] = '\0';

    const char *lp = boot_logoutput_path();
    long lastLog = nebula_file_size(lp);
    if (lastLog < 0) lastLog = 0;
    long lastDiag = diag[0] ? nebula_file_size(diag) : 0;
    if (lastDiag < 0) lastDiag = 0;
    int budget = 800;   // 防止 BepInEx Info 全开时把 host 日志刷爆

    printf("[host][r55] 长期日志监听启动(重写检测已开): LogOutput.log 起点=%ld B；diag=%s\n",
           lastLog, diag[0] ? diag : "(未使用)");
    printf("[host][r55] 修复要点: TryLoad 直接喂 UnityFramework 绝对路径(get_GameAssemblyPath())，绕过 DllImportResolver 命中失败的坑；tail 增加文件重写(截断)检测，不再漏掉初始化插桩\n");
    fflush(NULL);

    // 0.25s * 7200 = 30 分钟，覆盖切场景后的插件加载窗口
    for (int i = 0; i < 7200 && g_r54_tail_running && budget > 0; i++) {
        lastLog = nebula_r54_tail_one(lp, lastLog, "[host][r54][log]", &budget);
        if (diag[0])
            lastDiag = nebula_r54_tail_one(diag, lastDiag, "[host][r54][diag]", &budget);
        usleep(250 * 1000);
    }
    return NULL;
}

/* [R56] 宿主主动触发插件加载所需的两个全局句柄。
   R55 真机 preloader.log = System.DllNotFoundException: dobby（设备 core/dobby.dll
   是 Windows x64 PE，iOS arm64 无法 dlopen），native inline detour 装不上，
   OnInvokeMethod 永不触发 → 不扫 plugins → 进原版。详见下方 nebula_r56_* 注释。 */
static MonoDomain *g_r56_domain     = NULL;   // Mono 根域（boot_thread 里 init，主线程需 attach）
static MonoImage  *g_r56_il2cpp_img = NULL;   // BepInEx.Unity.IL2CPP 的 image

// 正式路径：把控制权交给 BepInEx（优先从 blob 内存加载，否则文件兜底）
static int boot_bepinex(MonoDomain *domain, const char *root) {
    // 记录 LogOutput.log 的起始大小，Start() 之后只 grep 这一段增量
    const long log_offset = nebula_file_size(boot_logoutput_path());
    MonoAssembly *as = NULL;
    const blob_entry *e = blob_find("BepInEx.Unity.IL2CPP");
    if (e) {
        as = blob_load_assembly(e);
        if (as) printf("[host] 从 blob 内存加载 BepInEx 入口\n");
    }
    if (!as) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/BepInEx/core/BepInEx.Unity.IL2CPP.dll", root);
        FILE *probe = fopen(path, "rb");
        if (!probe) {
            printf("[host] 未找到 BInEx 入口（内存与文件均无）\n");
            return 0;
        }
        fclose(probe);
        printf("[host] 从文件加载 BepInEx 入口: %s\n", path);
        as = mono_domain_assembly_open(domain, path);
    }
    if (!as) { fprintf(stderr, "[host] 打开 BepInEx.Unity.IL2CPP 失败\n"); return 0; }

    MonoImage *img = mono_assembly_get_image(as);
    /* [R56] 保存 image：dobby detour 装不上时，宿主要在 Unity 主线程从这里取
       IL2CPPChainloader.NebulaManualExecute() 主动扫 plugins（见 boot() 末尾派发）。 */
    g_r56_il2cpp_img = img;
    MonoClass *klass = mono_class_from_name(img, "Doorstop", "Entrypoint");
    if (!klass) { fprintf(stderr, "[host] 找不到 Doorstop.Entrypoint\n"); return 0; }

    MonoMethod *start = mono_class_get_method_from_name(klass, "Start", 0);
    if (!start) { fprintf(stderr, "[host] 找不到 Doorstop.Entrypoint.Start()\n"); return 0; }

    // 切换 CWD 到 app 目录（兜底：任何相对路径写入都落在可写容器内），并手动初始化 BepInEx.Paths
    // [R47] iOS 布局校正：交互前先把 GameAssembly/metadata 的真实位置纠正
    // （见 nebula_fix_il2cpp_paths 上方注释 —— 不改这两个路径，interop 必然生成失败）
    nebula_fix_il2cpp_paths(root);
    chdir(root);
    nebula_init_bepinex_paths(domain, root);
    // 关键：在 BepInEx 接管前注入 NEBULA_IOS=1，触发 vendored BepInEx 的 Patch A
    // （Preloader.Core.PlatformUtils.SetPlatform 在 set_Current 前若检测到该 env=1，
    //  强制 PlatformHelper.Current = iOS(0x249)），彻底绕过嵌入式 Mono 把 iOS 报成
    //  MacOSX 导致 SetExecutablePath 走 MacOS 分支上跳 4 级、GameRootPath 错位的坑。
    setenv("NEBULA_IOS", "1", 1);
    // 诊断（R35）：强制 BepInEx 全级别日志写文件 + 控制台，便于定位 NOS 初始化失败原因
    setenv("BEPINEX_LOG_LEVEL", "All", 1);
    setenv("BEPINEX_PRELOADER_LOG_WRITE", "true", 1);
    setenv("BEPINEX_PRELOADER_LOG_CONSOLE", "true", 1);
    setenv("DOORSTOP_LOG_LEVEL", "All", 1);
    // 残留冗余：Enum.Parse 覆写 Current 在真机已失效（找不到 Enum.Parse(Type,string)）
    // 且会被 SetPlatform 重置；保留为无害兜底，主机制改为上面的 env 触发 Patch A。
    nebula_fix_platform(domain);

    // 诊断（R37）：先把 preloader 日志监听线程拉起来，再调 Start()
    // （Start() 常年不返回，调用点之后的诊断永远到不了，必须提前起后台 tail）
    g_prelog_tail_running = 1;
    {
        pthread_t tt;
        if (pthread_create(&tt, NULL, nebula_prelog_tail_thread, NULL) == 0) pthread_detach(tt);
    }
    // [R54] 插件加载发生在 Start() 返回、Unity 切场景之后，原来的 120s preloader tail
    // 与一次性 LogOutput.log grep 都覆盖不到；再起一个 30 分钟的长期 tail 抓异常堆栈。
    g_r54_tail_running = 1;
    {
        pthread_t rt;
        if (pthread_create(&rt, NULL, nebula_r54_tail_thread, NULL) == 0) pthread_detach(rt);
    }

    printf("[host] 调用 Doorstop.Entrypoint.Start() —— 之后交给 BepInEx chainloader\n");
    MonoObject *exc = NULL;
    mono_runtime_invoke(start, NULL, NULL, &exc);
    nebula_log_paths(domain);   // 诊断：打印 Paths 真实落点，判定平台/MacOS/Data 问题
    // 诊断（R35）：枚举 BepInEx 目录，确认 Preloader/Chainloader 是否生成产物
    // （preloader.log / LogOutput.log / plugins 缓存），定位 NOS 初始化失败原因
    {
        const char *home = getenv("HOME");
        char bepdir[PATH_MAX];
        snprintf(bepdir, sizeof(bepdir), "%s/Documents/BepInEx", home ? home : root);
        printf("[host][diag] === BepInEx 目录枚举（Start 之后）===\n");
        nebula_list_dir(bepdir, 0);
        fflush(NULL);
    }
    // [R44 诊断] LogOutput.log 是判断 interop 有没有真生成的唯一权威来源：
    //   - 跳过生成时会留 "Interop assemblies are possibly out of date"
    //   - 真生成时会有 Cpp2IL / Generator / "Regenerating" 等一长串
    // 设备上的文件取回来成本高，这里把关键行直接抓进 host 日志。
    {
        char lglo[PATH_MAX];
        const char *hom2 = getenv("HOME");
        snprintf(lglo, sizeof(lglo), "%s/Documents/BepInEx/LogOutput.log", hom2 ? hom2 : root);
        static const char *const keys[] = {
            "Interop", "UpdateInteropAssemblies", "Regenerating", "GameAssembly",
            "Cpp2IL", "Generating", "Generator", "possibly out of date",
            "Failed", "Exception", "ERROR", "warn"
        };
        printf("[host][diag] === LogOutput.log 本次增量（起点 %ld B，之前的是历史残留）===\n", log_offset);
        fflush(NULL);
        nebula_grep_file(lglo, keys, (int)(sizeof(keys) / sizeof(keys[0])), 200, log_offset);
    }
    if (exc) { fprintf(stderr, "[host] !! Start() 抛出异常\n"); nebula_log_exception(exc); return 0; }

    // [R43 关键] Doorstop.Entrypoint.Start() 把 Preloader 的所有异常都 catch 住，
    // 只写一份 preloader_*.log 就正常返回（IL 实证：catch 块里 File.WriteAllText 后
    // 既不 rethrow 也不 Exit，除非设了 BEPINEX_FAIL_FAST）。
    // 所以 mono_runtime_invoke 拿不到异常 ≠ BepInEx 成功 —— 只看异常会 100% 误判，
    // 真机表现就是「boot_bepinex -> 1、游戏照常进、NOS 永远不加载」。
    // 这里反查 preloader_*.log：它一出现，就说明 Preloader 半路死了。
    if (nebula_preloader_failed(root)) {
        fprintf(stderr, "[host] !! 发现 preloader_*.log：Doorstop 吞掉了 Preloader 异常，BepInEx 实际未成功\n");
        fprintf(stderr, "[host]    异常全文见上方 [host][prelog] 行（或设备上的 preloader_*.log）\n");
        fflush(NULL);
        return 0;
    }
    printf("[host] Start() 已返回，RSS=%ldMB\n", rss_mb());
    return 1;
}

// 把托管异常转成可读文本打到 host 日志（避免只看得到"抛出异常"却不知原因）
// 只走原生 API 打印异常类型：托管 ToString(mono_object_to_string) 在异常态下
// 需要 JIT 编译，iOS 上实测会把进程带崩并丢掉最后几行日志（看不到异常内容），故弃用。
static void nebula_log_exception(MonoObject *exc) {
    if (!exc) return;
    MonoClass *c = mono_object_get_class(exc);
    const char *ns = c ? mono_class_get_namespace(c) : NULL;
    const char *nm = c ? mono_class_get_name(c) : "?";
    fprintf(stderr, "[host]   异常类型: %s.%s\n", ns ? ns : "", nm ? nm : "?");
    fflush(NULL);
}

/* [R44 诊断] 打印文本日志里命中任一关键字的行。LogOutput.log 在设备上、取回来成本高，
   而它是判断 interop 到底有没有生成的唯一权威来源（里面会有
   "Interop assemblies are possibly out of date" / Cpp2IL / Generator 的行）。
   maxHits 上限防止把 45KB 日志整个灌进来。 */
static void nebula_grep_file(const char *path, const char *const keys[], int nkeys, int maxHits, long offset) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("[host][grep] 打不开 %s\n", path); fflush(NULL); return; }
    /* 只看本次运行新增的部分：offset>0 就跳到启动时的文件末尾 */
    if (offset > 0) fseek(f, offset, SEEK_SET);
    char buf[8192];
    int hits = 0;
    while (fgets(buf, sizeof(buf), f)) {
        int hit = 0;
        for (int i = 0; i < nkeys && !hit; i++)
            if (strstr(buf, keys[i]) != NULL) hit = 1;
        if (!hit) continue;
        size_t L = strlen(buf);
        while (L > 0 && (buf[L - 1] == '\n' || buf[L - 1] == '\r')) buf[--L] = '\0';
        printf("[host][grep] %s\n", buf);
        fflush(NULL);
        if (++hits >= maxHits) break;
    }
    fclose(f);
}

/* [R43] Doorstop 把 Preloader 的异常全吞了（catch 里只写 preloader_*.log 就正常返回），
   所以 mono_runtime_invoke 没抛异常根本不代表 BepInEx 起来了。
   只要 BepInEx 根目录里出现任何 preloader_*.log，就判定 Preloader 半路死掉。
   app 根与 Documents 两处都查（BepInEx 根目录在 iOS 上被重定向到可写容器）。 */
static int nebula_preloader_failed(const char *root) {
    const char *home = getenv("HOME");
    const char *dirs[3];
    static char b0[PATH_MAX], b1[PATH_MAX], b2[PATH_MAX];
    int n = 0;
    snprintf(b0, sizeof(b0), "%s/BepInEx", root);
    dirs[n++] = b0;
    if (home) {
        snprintf(b1, sizeof(b1), "%s/Documents/BepInEx", home);
        dirs[n++] = b1;
        // [R44 修正] preloader.log 真实落点在 Documents 根目录（BepInExRootPath 的
        // 上一级），不是 Documents/BepInEx —— R43 漏了这个目录，于是明明 Preloader
        // 崩了却判不出失败，日志里照样打出 boot_bepinex -> 1。
        snprintf(b2, sizeof(b2), "%s/Documents", home);
        dirs[n++] = b2;
    }

    for (int i = 0; i < n; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "preloader_", 10) == 0) {
                printf("[host][prelog] 命中失败标志: %s/%s\n", dirs[i], de->d_name);
                fflush(NULL);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    return 0;
}

// 诊断（R35）：递归枚举目录打印到 host 日志，确认 BepInEx 运行后生成了哪些产物
// （preloader.log / LogOutput.log / plugins 缓存 / NOS 自身写的错误日志），
// 用于定位 NOS 初始化失败原因。深度限制 3 层，防符号链接环或超大目录爆炸。
static void nebula_list_dir(const char *path, int depth) {
    if (depth > 3) return;
    DIR *d = opendir(path);
    if (!d) { printf("[host][dir] 无法打开 %s\n", path); fflush(NULL); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("[host][dir] D %s/\n", full);
            fflush(NULL);
            nebula_list_dir(full, depth + 1);
        } else {
            printf("[host][dir] F %s (%lld bytes)\n", full, (long long)(stat(full, &st) == 0 ? st.st_size : -1));
            fflush(NULL);
        }
    }
    closedir(d);
}

// 尝试调用某个静态无参方法，成功返回 1
static int try_invoke(MonoImage *img, const char *ns, const char *cls, const char *method) {
    MonoClass *k = mono_class_from_name(img, ns, cls);
    if (!k) return 0;
    MonoMethod *m = mono_class_get_method_from_name(k, method, 0);
    if (!m) return 0;
    MonoObject *exc = NULL;
    printf("[host]   调用 %s%s%s.%s()\n", ns ? ns : "", ns && *ns ? "." : "", cls, method);
    mono_runtime_invoke(m, NULL, NULL, &exc);
    if (exc) { fprintf(stderr, "[host]   !! 抛异常\n"); nebula_log_exception(exc); return 0; }
    return 1;
}

// 通用 C# 模组加载：扫 Mods/ 目录，逐个 Assembly.Load 并尝试常见入口。
// 目的是不只支持星云舰/BepInEx —— 别的 C# AU 模组只要扔进去，入口能被认出来就能跑。
static int boot_mods_dir(MonoDomain *domain, const char *root) {
    char mdir[PATH_MAX];
    const char *m = getenv("NEBULA_MODS_DIR");
    if (m && *m) snprintf(mdir, sizeof(mdir), "%s", m);
    else         snprintf(mdir, sizeof(mdir), "%s/Mods", root);

    DIR *d = opendir(mdir);
    if (!d) { printf("[host] 无 Mods 目录: %s\n", mdir); return 0; }
    printf("[host] 扫描 Mods 目录: %s\n", mdir);

    struct dirent *e;
    int loaded = 0;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (len < 5 || strcasecmp(n + len - 4, ".dll") != 0) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", mdir, n);
        printf("[host] 加载模组: %s\n", n);

        MonoAssembly *as = mono_domain_assembly_open(domain, path);
        if (!as) { fprintf(stderr, "[host]   打开失败\n"); continue; }
        loaded++;

        MonoImage *img = mono_assembly_get_image(as);
        if (!img) continue;

        // 常见入口，按顺序试：命中任意一个就算启动成功
        char cls[256];
        snprintf(cls, sizeof(cls), "%.*s", (int)(len - 4), n);   // 去掉 .dll 当类名
        if (try_invoke(img, "Doorstop", "Entrypoint", "Start")) continue;
        if (try_invoke(img, "", "Entrypoint", "Start"))          continue;
        if (try_invoke(img, "", "Mod", "Start"))                 continue;
        if (try_invoke(img, "", cls, "Start"))                    continue;
        if (try_invoke(img, "", cls, "Main"))                    continue;
        printf("[host]   （没找到已知入口，仅加载不启动）\n");
    }
    closedir(d);
    printf("[host] Mods 加载完毕，共 %d 个程序集\n", loaded);
    return loaded > 0;
}

// 兜底/桌面验证路径：跑 Engine.dll 这个 POC（Roslyn 编译脚本并执行）
static int boot_engine_poc(MonoDomain *domain, const char *dir) {
    char engine[PATH_MAX];
    snprintf(engine, sizeof(engine), "%s/Engine.dll", dir);

    MonoAssembly *as = mono_domain_assembly_open(domain, engine);
    if (!as) { fprintf(stderr, "[host] open %s failed\n", engine); return 0; }

    MonoImage *img = mono_assembly_get_image(as);
    MonoClass *klass = mono_class_from_name(img, "Engine", "Engine");
    if (!klass) { fprintf(stderr, "[host] class Engine.Engine not found\n"); return 0; }

    MonoMethod *main = mono_class_get_method_from_name(klass, "Main", 0);
    if (!main) { fprintf(stderr, "[host] Engine.Main not found\n"); return 0; }
    mono_runtime_invoke(main, NULL, NULL, NULL);
    return 1;
}

/* case_insensitive_fs.c：iOS 文件系统大小写不敏感回退（需传入游戏根目录） */
void nebula_cifs_init(const char *root);

/* boot_overlay.m：启动占位文本（防 Mono+BepInEx 初始化期间黑屏误判死机）。
   由宿主 dylib 同一编译单元导出，托管侧也可用 [DllImport("__Internal")] 调用
   nebula_signal_ready() 在 Nebula.dll + Harmony 就绪时立刻销毁文字。
   下方 weak 空实现：iOS 上 boot_overlay.m 链接了同名【非 weak】真实现，链接器
   优先用真实现；macOS / 其它不含 boot_overlay.m 的构建只有这里 weak 空实现，
   避免链接期 undefined symbol（boot_thread 调了这几个函数）。 */
void nebula_show_boot_overlay(void);
void nebula_hide_boot_overlay(void);
void nebula_signal_ready(void);
__attribute__((weak)) void nebula_show_boot_overlay(void) {}
__attribute__((weak)) void nebula_hide_boot_overlay(void) {}
__attribute__((weak)) void nebula_signal_ready(void) { nebula_hide_boot_overlay(); }

/* iOS mono 产物里未必带 libSystem.Globalization.Native.a，而 mono 的
   native-library.c 会引用 _GlobalizationResolveDllImport（签名见 dotnet/runtime 的
   src/native/libs/System.Globalization.Native/entrypoints.c）。
   这里给 weak 定义兜底：宿主已设 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1，不需要 ICU，
   返回 NULL 即可；若静态库里有真实实现，链接器会优先用真实的。 */
__attribute__((weak))
const void *GlobalizationResolveDllImport(const char *name)
{
    (void)name;
    return NULL;
}

/* ---- 日志：双通道 ----
   (1) 系统日志 os_log（subsystem=com.nebula.host）：真机不依赖沙盒文件路径，
       用 idevicesyslog 直接抓，最可靠——文件日志在沙盒里用户不易取。
   (2) 文件：兜底，依次试多个可写位置（见 log_try）。
   每条都 fflush，进程硬崩(SIGSEGV/SIGABRT)也能留下最后一行；
   同时把原文存进 g_last_buf，崩点信号处理器能读出“最后执行到哪一步”。 */
#include <stdarg.h>
static os_log_t g_neb_log = NULL;
static char      g_last_buf[1024];
static const char *g_last_step = "(尚未执行任何步骤)";

static void hlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_buf, sizeof(g_last_buf), fmt, ap);
    va_end(ap);
    g_last_step = g_last_buf;
    if (g_neb_log) os_log(g_neb_log, "%{public}s", g_last_buf);
    if (g_logfp)   { fprintf(g_logfp, "%s\n", g_last_buf); fflush(g_logfp); }
    printf("%s\n", g_last_buf); fflush(stdout);
}

/* 崩溃信号处理器：崩时把“最后执行到的步骤”打进系统日志 + 文件，再交还默认行为
   生成崩溃报告。async-signal-safe：只读全局 buffer + 调 os_log（Apple 宣称安全）。 */
static void neb_crash(int sig)
{
    if (g_neb_log)
        os_log(g_neb_log, "!! 崩溃信号 %d，最后执行到: %{public}s",
               sig, g_last_step ? g_last_step : "(null)");
    if (g_logfp) { fprintf(g_logfp, "!! signal %d @ %s | RSS=%ldMB\n", sig, g_last_step, rss_mb()); fflush(g_logfp); }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* —— [Nebula iOS 第十二轮] 死点取证 ——
 * 现状：mono_jit_init 内部死亡、无 .ips、日志只到注入钩子的最后一行。
 * 两大盲区：
 *   1) mono 内部的致命输出（g_error 断言 "* Assertion at mini-*.c:xx"、
 *      "Cannot find assembly" 等）全走 stderr —— stderr 从未进过日志文件，
 *      mono 临死说的话我们一句都没听到。
 *   2) 旧 neb_crash 在 mono_jit_init【之后】才装，jit_init 内部崩溃时
 *      根本没有处理器，且无 PC/LR 现场信息。
 * 对策：
 *   A. stderr/stdout 重定向进管道，专职线程逐行回写 nebula_host.txt（fflush），
 *      mono 的遗言（断言消息）第一次可见；
 *   B. SA_SIGINFO 崩溃处理器（在 mono_jit_init 之前安装）：打出 信号号/si_addr/
 *      PC/LR/SP + dladdr 符号化；PC 无模块归属即 JIT 生成代码，直接实锤
 *      "死在执行 JIT 产物" 还是 "死在 mono 运行时自身代码"。
 * SIGKILL 依然不可捕获（内核 codesigning monitor），届时日志无 trap 行 +
 * mono 无遗言 = 内核击杀实锤，转攻签名/内核侧。 */

static void neb_jit_trap(int sig, siginfo_t *si, void *ucv)
{
    (void)ucv;
    // Darwin 上 SIGSEGV/SIGBUS/SIGILL 的 si_addr 即故障指令 PC（够定位），
    // 无需 ucontext.h（iOS SDK 要求 _XOPEN_SOURCE，会带崩编译）。
    uintptr_t pc = (uintptr_t)(si ? si->si_addr : NULL);
    char pdesc[256];
    snprintf(pdesc, sizeof(pdesc), "%s", pc ? "(符号化失败)" : "(无)");
    Dl_info di;
    if (pc && dladdr((void *)pc, &di) && di.dli_fbase) {
        const char *fn = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        snprintf(pdesc, sizeof(pdesc), "%s!%s+0x%lx", fn,
                 di.dli_sname ? di.dli_sname : "?",
                 (unsigned long)(pc - (uintptr_t)di.dli_fbase));
    } else if (pc) {
        snprintf(pdesc, sizeof(pdesc), "0x%lx 无模块归属 → JIT 生成代码区", (unsigned long)pc);
    }
    if (g_logfp) {
        fprintf(g_logfp,
                "!! [trap] signal=%d si_code=%d si_addr=%p\n"
                "   故障PC=0x%lx %s\n   最后步骤: %s\n   RSS=%ldMB\n",
                sig, si ? si->si_code : 0, si ? si->si_addr : NULL,
                (unsigned long)pc, pdesc,
                g_last_step ? g_last_step : "(null)", rss_mb());
        fflush(g_logfp);
    }
    if (g_neb_log)
        os_log(g_neb_log, "!! trap sig=%d pc=%s", sig, pdesc);
    /* 还原默认处置再重发：让系统照常生成崩溃报告（若它还愿意生成的话） */
    struct sigaction d;
    memset(&d, 0, sizeof(d));
    d.sa_handler = SIG_DFL;
    sigaction(sig, &d, NULL);
    raise(sig);
}

static void neb_install_trap(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = neb_jit_trap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT,
#ifdef SIGEMT
                   SIGEMT,
#endif
#ifdef SIGSYS
                   SIGSYS,
#endif
    };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}

/* —— [第十五轮] thread 级 mach 异常端口：先于 BSD signal 抓取真实 fault 地址 ——
 * 背景：mono 在 mono_jit_init 内部装了自家的 SIGSEGV handler，覆盖了我们第十二轮的
 * SA_SIGINFO trap，导致 si_addr（真实故障地址）抓不到。BSD signal 改不了这个顺序，
 * 但 mach 异常端口在 BSD signal 之前投递，且我们只在【当前 boot 线程】上装
 * thread-level 端口（不碰 StikDebug 的 task-level 端口，JIT 放行不受影响）。
 * 收到 EXC_BAD_ACCESS 后打印 fault_addr / fault_pc 再回复 KERN_FAILURE，让内核
 * 把信号照常投递给 mono 的 handler（mono 的崩溃报告照常生成）。 */
static mach_port_t g_neb_exc_port = MACH_PORT_NULL;

typedef struct {
    mach_msg_header_t         head;
    mach_msg_body_t           body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t              ndr;
    exception_type_t          exc;
    mach_msg_type_number_t    code_count;
    int64_t                   code[2];
} neb_exc_msg_t;

static void *neb_exc_loop(void *arg)
{
    mach_port_t port = (mach_port_t)(uintptr_t)arg;
    for (;;) {
        neb_exc_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.head.msgh_size = sizeof(msg);
        kern_return_t kr = mach_msg(&msg.head, MACH_RCV_MSG, 0, sizeof(msg), port,
                                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) continue;
        uintptr_t fa = 0, fp = 0;
        if (msg.exc == EXC_BAD_ACCESS && msg.code_count >= 2) {
            fa = (uintptr_t)msg.code[1];   // EXC_BAD_ACCESS: code[1] = 故障虚拟地址
            fp = (uintptr_t)msg.code[0];
        } else if (msg.code_count >= 1) {
            fp = (uintptr_t)msg.code[0];
        }
        if (g_logfp) {
            char fdesc[256], pdesc[256];
            snprintf(fdesc, sizeof(fdesc), "%s", fa ? "(未映射/无效地址)" : "(无)");
            snprintf(pdesc, sizeof(pdesc), "%s", fp ? "(符号化失败)" : "(无)");
            Dl_info di;
            if (fp && dladdr((void *)fp, &di) && di.dli_fbase) {
                const char *fn = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
                snprintf(pdesc, sizeof(pdesc), "%s!%s+0x%lx", fn,
                         di.dli_sname ? di.dli_sname : "?",
                         (unsigned long)(fp - (uintptr_t)di.dli_fbase));
            } else if (fp) {
                snprintf(pdesc, sizeof(pdesc), "0x%lx 无模块归属(可能=JIT生成代码地址)", (unsigned long)fp);
            }
            fprintf(g_logfp,
                    "!! [mach-exc] type=%d fault_addr=0x%lx(%s) fault_pc=0x%lx %s\n"
                    "   最后步骤: %s   RSS=%ldMB\n",
                    (int)msg.exc, (unsigned long)fa, fdesc,
                    (unsigned long)fp, pdesc,
                    g_last_step ? g_last_step : "(null)", rss_mb());
            fflush(g_logfp);
        }
        if (g_neb_log)
            os_log(g_neb_log, "!! mach-exc type=%d fault_addr=0x%lx", (int)msg.exc, (unsigned long)fa);
        /* 回复 KERN_FAILURE：内核把异常照常转为 signal，mono 的 handler 仍能捕获并打印报告 */
        struct { mach_msg_header_t head; NDR_record_t ndr; kern_return_t ret; } rep;
        memset(&rep, 0, sizeof(rep));
        rep.head.msgh_size = sizeof(rep);
        rep.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(msg.head.msgh_bits), 0);
        rep.head.msgh_remote_port = msg.head.msgh_remote_port;
        rep.head.msgh_id = msg.head.msgh_id + 100;
        rep.ndr = msg.ndr;
        rep.ret = KERN_FAILURE;
        mach_msg(&rep.head, MACH_SEND_MSG, sizeof(rep), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
}

static void neb_install_mach_trap(void)
{
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_neb_exc_port);
    if (kr != KERN_SUCCESS) { g_neb_exc_port = MACH_PORT_NULL; return; }
    kr = mach_port_insert_right(mach_task_self(), g_neb_exc_port, g_neb_exc_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) { g_neb_exc_port = MACH_PORT_NULL; return; }
    /* 只在本线程（boot 线程，崩点所在）装 thread 级端口，避免覆盖 StikDebug 的 task 端口 */
    kr = thread_set_exception_ports(mach_thread_self(),
                                    EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION,
                                    g_neb_exc_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, 0);
    if (kr != KERN_SUCCESS) { g_neb_exc_port = MACH_PORT_NULL; return; }
    pthread_t t;
    if (pthread_create(&t, NULL, neb_exc_loop, (void *)(uintptr_t)g_neb_exc_port) == 0)
        pthread_detach(t);
}

/* —— [第十二轮] stderr/stdout 捕获：mono 的遗言第一次进日志 —— */
static int g_stdio_pipe[2] = { -1, -1 };

static void *neb_stdio_reader(void *arg)
{
    (void)arg;
    static char line[4096];
    size_t len = 0;
    ssize_t n;
    char buf[4096];
    while ((n = read(g_stdio_pipe[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || len >= sizeof(line) - 2) {
                line[len] = 0;
                if (len > 0 && g_logfp) {
                    fprintf(g_logfp, "[mono-out] %s\n", line);
                    fflush(g_logfp);
                }
                len = 0;
            } else {
                line[len++] = c;
            }
        }
    }
    return NULL;
}

static void neb_capture_stdio(void)
{
    if (g_stdio_pipe[0] != -1) return;
    if (pipe(g_stdio_pipe) != 0) return;
    fflush(stdout);
    fflush(stderr);
    dup2(g_stdio_pipe[1], STDOUT_FILENO);
    dup2(g_stdio_pipe[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, NULL, neb_stdio_reader, NULL) == 0) pthread_detach(t);
}

/* 依次尝试若干可写位置建日志；游戏目录未必可写，不能只试一个 */
static int log_try(const char *p){
    if (!p || !*p || g_logfp) return 0;
    FILE *f = fopen(p, "w");
    if (!f) return 0;
    g_logfp = f;
    hlog("[host] log -> %s", p);
    return 1;
}

// —— JIT 能力检测 + 等待 ——
// iOS 严格 W^X：mono 初始化必然生成 trampoline（P/Invoke、委托、接口分派），
// 统一编译器（JIT）版必须可执行 MAP_JIT 内存。进程若没有 JIT 能力
// （无 allow-jit 描述文件、也没被调试器附加），执行 trampoline 页会被内核
// 以 CODESIGNING / Invalid Page 秒杀（SIGKILL，无法被 signal handler 捕获）。
//
// 检测手段：csops(CS_OPS_STATUS) 的 CS_DEBUGGED 位 —— 被调试器附加（StikDebug /
// LiveContainer / SideJITServer 走 get-task-allow 附加）即置位，内核随后允许
// pthread_jit_write_protect_np 翻转并执行 MAP_JIT 页。
//
// 等待的意义：boot 在独立 detached 线程跑，主线程照常。用户可在 app 启动后
// 用 StikDebug 附加启用 JIT，本函数轮询到 CS_DEBUGGED 即放行 mono_jit_init，
// 化解“先开 JIT 还是先启动”的死锁。
static int nebula_have_jit(void) {
#ifndef __APPLE__
    return 1;   // 桌面平台（Linux/Windows）天然可 JIT
#else
#if TARGET_OS_IPHONE
    // 强制开关：用户确认已用 StikDebug 等开好 JIT，但内核 CS_DEBUGGED 位检测
    // 不到时（iOS 17/18 新式无调试器 JIT 通道 / detach 后该位被清 / 检测时机错过），
    // 用它跳过 JIT 闸门。风险自担：若实际并无 JIT，mono_jit_init 会被内核
    // CODESIGNING SIGKILL（游戏同崩，无 .ips）。
    const char *fj = getenv("NEBULA_FORCE_JIT");
    if (fj && *fj) {
        hlog("[host] JIT 强制放行（NEBULA_FORCE_JIT 已设，用户确认已开 JIT）");
        return 1;
    }
    // 判据：进程被调试器（StikDebug 等）附加 → 内核置 CS_DEBUGGED → 授予 JIT 能力。
    // 没这个位，mono 生成 trampoline 时会被内核 CODESIGNING 秒杀。
    int flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) != 0) return 0;
    return (flags & CS_DEBUGGED) ? 1 : 0;
#else
    // macOS：进程默认拥有 JIT 能力（不依赖 CS_DEBUGGED，无需调试器附加），
    // 直接放行。CI 的 macOS runner 无人附加调试器，但 mono 仍能正常 JIT。
    return 1;
#endif
#endif
}

// 返回 1 = JIT 已就绪；返回 0 = 超时仍未就绪（调用方应优雅退出，别硬崩）。
static int nebula_wait_for_jit(void) {
    int wait = 60;   // 秒；可用 NEBULA_JIT_WAIT 覆盖。放宽到 60s，给用户充足时间
                    // 去 StikDebug 启用 JIT（此前 30s 常被错过，导致“明明开了却判没开”）。
    const char *e = getenv("NEBULA_JIT_WAIT");
    if (e && *e) { int v = atoi(e); if (v > 0) wait = v; }

    if (nebula_have_jit()) {
        hlog("[host] JIT 已就绪（进程拥有 JIT 能力）");
        return 1;
    }
    hlog("[host] !! JIT 尚未就绪：当前内核 CS_DEBUGGED 位未置位，进程暂无 JIT 能力。");
    hlog("[host] !! 现在就用 StikDebug 给 AmongUs 启用 JIT（debugserver 附加 → 置 CS_DEBUGGED）。");
    hlog("[host] !! 若已开却仍判无，可在启动前设 NEBULA_FORCE_JIT=1 强制放行（风险自担）。");
    hlog("[host] !! 轮询等待 JIT 就绪（最多 %ds，期间 app 保持存活）...", wait);
    for (int i = 1; i <= wait; i++) {
        sleep(1);
        if (nebula_have_jit()) {
            hlog("[host] JIT 已就绪（%ds 后检测到 JIT 能力），继续 mono 初始化", i);
            return 1;
        }
    }
    hlog("[host] !! JIT 在 %ds 内始终未就绪，放弃 mono 初始化（避免无意义 SIGKILL/SIGSEGV）", wait);
    return 0;
}

// —— JIT 冒烟测试（仅 iOS）：mono_jit_init 之前多路线真实预演
//   「分配 → 写入 ret → sys_icache_invalidate → 执行」 ——
// 内核对非法 JIT 操作的惩罚是 SIGKILL（CODESIGNING / Invalid Page），不可捕获、
// 往往不生成 .ips（2026-10-25 真机实测多轮）。只能靠 nebula_host.txt 定位死因。
// 写入的是 arm64 ret（0xD65F03C0）：跳入后立即返回，不会落到页内零字节。
//
// 2026-10-25 定案背景（Apple 官方文档 + 社区实测交叉证实）：
//   · pthread_jit_write_protect_np 在 iOS 上【官方不导出】——Apple 文档明言
//     "isn't available on iOS"；iOS 14.2~14.4 曾短暂误加后移除。真机 dlsym 各路
//     （含 dlopen libsystem_pthread.dylib）均 NULL，日志实证。macOS 专属 API。
//   · plain mmap RWX + mprotect 补 EXEC：mprotect 返回 0 但内核假成功
//     （.ips vmRegion rw-/rwx，执行仍 SIGBUS KERN_PROTECTION_FAILURE）——死路。
//   · 活路：StikDebug 原理 = debugserver 附加 → 内核打 CS_DEBUGGED → 分离后保留。
//     debugserver 写内存用的是 mach_vm_write（调试器原语，不经 mprotect 的
//     W^X 检查）。本进程 CS_DEBUGGED=1 时这条路从未测过。
//
// 四条路线按序自动探测，任一成功即放行 mono_jit_init；全失败优雅退出保游戏可玩：
//   E. MAP_JIT mmap(RW, 不带 EXEC) → 写入 → mprotect R-X → 执行（macOS 标准用法；
//      此前 A/B/D 都带 EXEC 申请可能正是被拒原因，此路从未真测过）
//   F. plain mmap(RW) → 写入 → mprotect R-X → 执行（Saagar Jha W^X 技法：
//      CS_DEBUGGED 关掉 CS_KILL/CS_HARD → 无效页可执行；从未真测过）
//   A. MAP_JIT mmap(R-X) + mach_vm_write 自写（调试态首选，debugserver 同款原语）
//   B. MAP_JIT mmap(R-X) + mprotect 补 WRITE（真实保护位校验，假成功不写入）
//   C. MAP_JIT + pthread_jit_write_protect_np 翻转（解析到才走，官方路径）
//   D. MAP_JIT mmap(RWX) 直接分配（debugged 态内核可能直接给真 RWX）
// 安全网：
//   · 每次冒险操作（写/执行）前用 mach_vm_region 查内核真实保护位，不合格不动作；
//   · 装 SIGBUS/SIGSEGV/SIGILL 处理器 + sigsetjmp，执行段错误可捕获并尝试下一路线
//     （SIGKILL 无法捕获，但日志最后一行仍能定位死因）。
#if TARGET_OS_IPHONE

// 查询 addr 所在页的内核真实保护位（cur/max）。成功返回 0。
// mach_vm_region 查的是内核权威视图 —— mprotect 可能"假成功"（返回 0 但实际没改，
// codesigning monitor 拦截），必须以这里的结果为准再决定是否写入/执行。
static int nebula_page_prot(void *addr, vm_prot_t *cur, vm_prot_t *mx)
{
    vm_address_t a = (vm_address_t)(uintptr_t)addr;
    vm_size_t sz = 0;
    vm_region_basic_info_data_64_t bi;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = vm_region_64(mach_task_self(), &a, &sz,
                                    VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&bi, &cnt, &obj);
    if (kr != KERN_SUCCESS) return -1;
    if (cur) *cur = bi.protection;
    if (mx)  *mx  = bi.max_protection;
    return 0;
}

// 多源解析 pthread_jit_write_protect_np（iOS 官方不导出，多半解析不到，仅
// macOS / 特殊环境可用）：RTLD_DEFAULT → dlopen libsystem_pthread.dylib。
static void (*nebula_jit_toggle(void))(int)
{
    void (*fn)(int) = (void (*)(int))dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np");
    if (fn) return fn;
    void *h = dlopen("/usr/lib/system/libsystem_pthread.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libsystem_pthread.dylib", RTLD_NOW | RTLD_LOCAL);
    if (h) fn = (void (*)(int))dlsym(h, "pthread_jit_write_protect_np");
    return fn;   // 不 dlclose：保持句柄与函数指针持续有效
}

// 诊断：pthread_jit_write_protect_supported_np 是否可解析 / 返回什么。
// 社区实测它比 write_protect_np 更可能存在——若它返回 1 而翻转函数解析不到，
// 说明平台声称支持却没给用户态入口（Apple 半成品状态）。
static int nebula_jit_supported(void)
{
    typedef int (*fn_t)(void);
    fn_t fn = (fn_t)dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_supported_np");
    if (fn) return fn();
    void *h = dlopen("/usr/lib/system/libsystem_pthread.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libsystem_pthread.dylib", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        fn = (fn_t)dlsym(h, "pthread_jit_write_protect_supported_np");
        if (fn) return fn();
    }
    return -1;   // 符号本身也不存在
}

// 冒烟期间的 SIGBUS/SIGSEGV/SIGILL 兜底：写入/执行段错误时跳回冒烟框架尝试
// 下一路线，而不是整个游戏崩掉。SIGKILL 无法捕获（codesigning monitor 直接杀），
// 但日志最后一行能定位是哪条路线哪个步骤死的。
static sigjmp_buf g_smoke_jmp;
static volatile sig_atomic_t g_smoke_sig = 0;
static volatile uintptr_t g_smoke_fault = 0;

static void nebula_smoke_sigaction(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    g_smoke_sig = sig;
    g_smoke_fault = si ? (uintptr_t)si->si_addr : 0;
    siglongjmp(g_smoke_jmp, 1);
}

#endif // TARGET_OS_IPHONE

static int nebula_jit_smoke(void)
{
#if TARGET_OS_IPHONE
    const unsigned int ret_insn = 0xD65F03C0u;   // arm64 ret
    int flags = 0;
    int debugged = (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) == 0 &&
                    (flags & CS_DEBUGGED)) ? 1 : 0;
    hlog("[host] 冒烟: CS_DEBUGGED=%d csflags=0x%x", debugged, flags);
    hlog("[host] 冒烟: pthread_jit_write_protect_supported_np -> %d", nebula_jit_supported());
    void (*wp)(int) = nebula_jit_toggle();
    hlog("[host] 冒烟: pthread_jit_write_protect_np -> %p", (void *)wp);
    // 顺带探测姊妹符号（iOS 26 CFW 实测同样不导出，但 iOS 18.6 上值得记录）：
    hlog("[host] 冒烟: write_with_callback_np -> %p, freeze_np -> %p",
         dlsym(RTLD_DEFAULT, "pthread_jit_write_with_callback_np"),
         dlsym(RTLD_DEFAULT, "pthread_jit_write_freeze_np"));

    // 信号兜底：装 SIGBUS/SIGSEGV/SIGILL 处理器（可捕获的执行/写入段错误），
    // 旧处理器存档，冒烟结束还原
    struct sigaction sa, old_bus, old_segv, old_ill;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = nebula_smoke_sigaction;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    int have_bus  = (sigaction(SIGBUS,  &sa, &old_bus)  == 0);
    int have_segv = (sigaction(SIGSEGV, &sa, &old_segv) == 0);
    int have_ill  = (sigaction(SIGILL,  &sa, &old_ill)  == 0);

    int ok = 0;
    void *p = NULL;
    vm_prot_t cur = 0, mx = 0;

    // ── 路线 E：MAP_JIT mmap(RW, 不带 EXEC) → 写 → mprotect R-X → 执行 ──
    // macOS 上 MAP_JIT 的标准用法就是不带 EXEC 先分配 RW；此前路线 A/B/D 全带
    // EXEC 申请、全部 EPERM，很可能正是被拒原因 —— 这条从未真正测过。
    if (!ok) {
        p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) {
            hlog("[host] 冒烟: 路线E MAP_JIT(RW) mmap 被拒 errno=%d", errno);
        } else if (nebula_page_prot(p, &cur, &mx) != 0 || !(cur & PROT_WRITE)) {
            hlog("[host] 冒烟: 路线E mmap OK %p 但 cur=%x 不可写", p, cur);
        } else {
            hlog("[host] 冒烟: 路线E MAP_JIT(RW) mmap OK %p (cur=%x max=%x), 试写入 ret ...",
                 p, cur, mx);
            *(unsigned int *)p = ret_insn;
            hlog("[host] 冒烟: 路线E 写入 OK, 试 mprotect 翻 R-X ...");
            if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0) {
                hlog("[host] 冒烟: 路线E mprotect R-X 被拒 errno=%d", errno);
            } else if (nebula_page_prot(p, &cur, &mx) != 0 || !(cur & PROT_EXEC)) {
                hlog("[host] 冒烟: 路线E mprotect 假成功（真实 cur=%x 无 EXEC）, 不执行", cur);
            } else {
                sys_icache_invalidate(p, 4096);
                hlog("[host] 冒烟: 路线E 真实 R-X (cur=%x), icache 已刷, 试执行 ...", cur);
                if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                    ((void (*)(void))p)();
                    hlog("[host] 冒烟: 路线E 执行 OK —— MAP_JIT(RW)+mprotect W^X 路线畅通!");
                    ok = 1;
                } else {
                    hlog("[host] 冒烟: 路线E 执行段错误 signal=%d fault=%p",
                         g_smoke_sig, g_smoke_fault);
                }
            }
        }
        if (!ok && p) { munmap(p, 4096); p = NULL; }
    }

    // ── 路线 F：plain mmap(RW) → 写 → mprotect R-X → 执行（Saagar Jha W^X 技法）──
    // CS_DEBUGGED 关掉 CS_KILL/CS_HARD → 含无效（未签名）代码的页允许执行；
    // 只要 mprotect 能在 RW 与 R-X 间翻转，就是一条可用的 W^X JIT。
    if (!ok) {
        p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            hlog("[host] 冒烟: 路线F plain(RW) mmap 被拒 errno=%d", errno);
        } else {
            hlog("[host] 冒烟: 路线F plain(RW) mmap OK %p (cur=%x max=%x), 试写入 ret ...",
                 p, cur, mx);
            *(unsigned int *)p = ret_insn;
            hlog("[host] 冒烟: 路线F 写入 OK, 试 mprotect 翻 R-X ...");
            if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0) {
                hlog("[host] 冒烟: 路线F mprotect R-X 被拒 errno=%d", errno);
            } else if (nebula_page_prot(p, &cur, &mx) != 0 || !(cur & PROT_EXEC)) {
                hlog("[host] 冒烟: 路线F mprotect 假成功（真实 cur=%x 无 EXEC）, 不执行", cur);
            } else {
                sys_icache_invalidate(p, 4096);
                hlog("[host] 冒烟: 路线F 真实 R-X (cur=%x), icache 已刷, 试执行 ...", cur);
                if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                    ((void (*)(void))p)();
                    hlog("[host] 冒烟: 路线F 执行 OK —— plain(RW)+mprotect W^X 路线畅通!");
                    ok = 1;
                    // mono 需要反复 W^X 循环：验证 RX → 回翻 RW → 再写 → 再 RX → 再执行
                    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0 ||
                        nebula_page_prot(p, &cur, &mx) != 0 || !(cur & PROT_WRITE)) {
                        hlog("[host] !! 路线F 回翻 RW 失败 (cur=%x errno=%d) —— mono 反复写代码会崩!",
                             cur, errno);
                        ok = 0;
                    } else {
                        *(unsigned int *)p = ret_insn;   // 再写一次（内容同 ret）
                        if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0 ||
                            nebula_page_prot(p, &cur, &mx) != 0 || !(cur & PROT_EXEC)) {
                            hlog("[host] !! 路线F 二次翻 R-X 失败 (cur=%x errno=%d)", cur, errno);
                            ok = 0;
                        } else {
                            sys_icache_invalidate(p, 4096);
                            if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                                ((void (*)(void))p)();
                                hlog("[host] 冒烟: 路线F 二次循环执行 OK —— W^X 反复翻转可用, 放行 mono_jit_init");
                            } else {
                                hlog("[host] !! 路线F 二次执行段错误 signal=%d fault=%p",
                                     g_smoke_sig, g_smoke_fault);
                                ok = 0;
                            }
                        }
                    }
                } else {
                    hlog("[host] 冒烟: 路线F 执行段错误 signal=%d fault=%p",
                         g_smoke_sig, g_smoke_fault);
                }
            }
        }
        if (!ok && p) { munmap(p, 4096); p = NULL; }
    }

    // ── 路线 G（纯诊断，不影响 ok 放行逻辑）：复刻 mono_valloc 真实序列 ──
    // mono JIT 区 = mmap(剥EXEC) → mprotect(RWX)（第九轮 re-add 同款 prot=7）
    //             → set_write 翻 RW 写 trampoline → 翻 RX 执行。
    // 若 G 执行死（SIGKILL 无日志）→ "RWX 起步页执行被杀"实锤，re-add 策略要改；
    // 若 G 执行 OK → mono 同款序列在真机畅通，死因在 mono 侧其他路径。
    {
        void *g = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g == MAP_FAILED) {
            hlog("[host] 冒烟: 路线G mmap 被拒 errno=%d", errno);
        } else if (mprotect(g, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            hlog("[host] 冒烟: 路线G mprotect RWX 被拒 errno=%d", errno);
            munmap(g, 4096);
        } else {
            hlog("[host] 冒烟: 路线G re-add RWX OK %p (mono 同款), 翻 RW 写 ...", g);
            if (mprotect(g, 4096, PROT_READ | PROT_WRITE) != 0) {
                hlog("[host] 冒烟: 路线G 翻 RW 被拒 errno=%d", errno);
            } else {
                *(unsigned int *)g = ret_insn;
                if (mprotect(g, 4096, PROT_READ | PROT_EXEC) != 0) {
                    hlog("[host] 冒烟: 路线G 翻 RX 被拒 errno=%d", errno);
                } else {
                    sys_icache_invalidate(g, 4096);
                    hlog("[host] 冒烟: 路线G 翻 RX OK, 试执行（若死在这=RWX起步页执行被杀）...");
                    if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                        ((void (*)(void))g)();
                        hlog("[host] 冒烟: 路线G 执行 OK —— mono 同款序列(RWX起步)畅通!");
                    } else {
                        hlog("[host] 冒烟: 路线G 执行段错误 signal=%d fault=%p",
                             g_smoke_sig, g_smoke_fault);
                    }
                }
            }
            munmap(g, 4096);
        }
    }

    // ── 路线 A：MAP_JIT mmap(R-X) + mach_vm_write 自写（调试态首选）──
    // debugserver 给被调试进程写内存就用 mach_vm_write；本进程 CS_DEBUGGED=1，
    // 同款原语对自家 MAP_JIT 页写入，绕开 mprotect 被压死的 W^X 检查。
    // 页面保持 R-X（可执行），写入走调试器通道，天然符合 W^X。
    if (!ok) {
        p = mmap(NULL, 4096, PROT_READ | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) {
            hlog("[host] 冒烟: 路线A MAP_JIT mmap 被拒 errno=%d（进程无 JIT 能力）", errno);
        } else {
            nebula_page_prot(p, &cur, &mx);
            hlog("[host] 冒烟: 路线A MAP_JIT mmap OK %p (cur=%x max=%x), 试 vm_write ...",
                 p, cur, mx);
            kern_return_t kr = vm_write(mach_task_self(),
                                        (vm_address_t)(uintptr_t)p,
                                        (vm_offset_t)(uintptr_t)&ret_insn,
                                        (mach_msg_type_number_t)sizeof(ret_insn));
            if (kr != KERN_SUCCESS) {
                hlog("[host] 冒烟: 路线A vm_write 失败 kr=%d", kr);
            } else if (*(volatile unsigned int *)p != ret_insn) {
                hlog("[host] 冒烟: 路线A vm_write 报成功但读回不符 —— 假写入");
            } else {
                hlog("[host] 冒烟: 路线A vm_write 写入 OK（读回一致）, 刷 icache, 试执行 ...");
                sys_icache_invalidate(p, 4096);
                if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                    ((void (*)(void))p)();
                    hlog("[host] 冒烟: 路线A 执行 OK —— MAP_JIT+vm_write 路线畅通!");
                    ok = 1;
                } else {
                    hlog("[host] 冒烟: 路线A 执行段错误 signal=%d fault=%p（页不可执行）",
                         g_smoke_sig, g_smoke_fault);
                }
            }
            if (!ok) munmap(p, 4096);
        }
    }

    // ── 路线 B：MAP_JIT mmap(R-X) + mprotect 补 WRITE（校验真实保护位再写）──
    if (!ok) {
        p = mmap(NULL, 4096, PROT_READ | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) {
            hlog("[host] 冒烟: 路线B MAP_JIT mmap 被拒 errno=%d", errno);
        } else {
            hlog("[host] 冒烟: 路线B MAP_JIT mmap OK %p, 试 mprotect +WRITE ...", p);
            if (mprotect(p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
                hlog("[host] 冒烟: 路线B mprotect 被拒 errno=%d", errno);
            } else if (nebula_page_prot(p, &cur, &mx) != 0 || !(cur & PROT_WRITE)) {
                hlog("[host] 冒烟: 路线B mprotect 假成功（真实 cur=%x max=%x）—— 内核拒绝写, 不冒险",
                     cur, mx);
            } else {
                hlog("[host] 冒烟: 路线B mprotect 真实生效 cur=%x, 试写入 ret ...", cur);
                *(unsigned int *)p = ret_insn;
                if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0) {
                    hlog("[host] 冒烟: 路线B 翻回 R-X 被拒 errno=%d", errno);
                } else if (nebula_page_prot(p, &cur, &mx) != 0 || !(cur & PROT_EXEC)) {
                    hlog("[host] 冒烟: 路线B 翻回后真实 cur=%x 无 EXEC, 不执行", cur);
                } else {
                    sys_icache_invalidate(p, 4096);
                    hlog("[host] 冒烟: 路线B icache 已刷, 试执行 ...");
                    if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                        ((void (*)(void))p)();
                        hlog("[host] 冒烟: 路线B 执行 OK —— MAP_JIT+mprotect W^X 循环可用!");
                        ok = 1;
                    } else {
                        hlog("[host] 冒烟: 路线B 执行段错误 signal=%d fault=%p",
                             g_smoke_sig, g_smoke_fault);
                    }
                }
            }
            if (!ok) munmap(p, 4096);
        }
    }

    // ── 路线 C：MAP_JIT + 翻转函数（官方路径，解析到才走）──
    if (!ok && wp) {
        p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) {
            hlog("[host] 冒烟: 路线C MAP_JIT mmap 被拒 errno=%d", errno);
        } else {
            hlog("[host] 冒烟: 路线C MAP_JIT mmap OK %p, 翻转(0)可写 ...", p);
            wp(0);
            hlog("[host] 冒烟: 路线C 已翻可写, 试写入 ret ...");
            *(unsigned int *)p = ret_insn;
            hlog("[host] 冒烟: 路线C 写入 OK, 翻转(1)回可执行 ...");
            wp(1);
            sys_icache_invalidate(p, 4096);
            hlog("[host] 冒烟: 路线C icache 已刷, 试执行 ...");
            if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                ((void (*)(void))p)();
                hlog("[host] 冒烟: 路线C 执行 OK —— MAP_JIT+翻转函数 可用!");
                ok = 1;
            } else {
                hlog("[host] 冒烟: 路线C 执行段错误 signal=%d fault=%p",
                     g_smoke_sig, g_smoke_fault);
            }
            wp(0);   // 还原全局可写态（mono 后续自行管理）
            if (!ok) munmap(p, 4096);
        }
    }

    // ── 路线 D：MAP_JIT + RWX 直接分配（debugged 态内核可能直接给真 RWX）──
    if (!ok) {
        p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) {
            hlog("[host] 冒烟: 路线D MAP_JIT+RWX mmap 被拒 errno=%d", errno);
        } else if (nebula_page_prot(p, &cur, &mx) != 0) {
            hlog("[host] 冒烟: 路线D mmap OK %p 但 prot 查询失败", p);
        } else {
            hlog("[host] 冒烟: 路线D MAP_JIT+RWX mmap OK %p (cur=%x max=%x)", p, cur, mx);
            if ((cur & PROT_WRITE) && (cur & PROT_EXEC)) {
                *(unsigned int *)p = ret_insn;
                sys_icache_invalidate(p, 4096);
                hlog("[host] 冒烟: 路线D 页面真实 RWX, 写入 OK, 试执行 ...");
                if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                    ((void (*)(void))p)();
                    hlog("[host] 冒烟: 路线D 执行 OK —— MAP_JIT+RWX 直接分配可用!");
                    ok = 1;
                } else {
                    hlog("[host] 冒烟: 路线D 执行段错误 signal=%d fault=%p",
                         g_smoke_sig, g_smoke_fault);
                }
            } else {
                hlog("[host] 冒烟: 路线D 页面非真实 RWX (cur=%x)", cur);
            }
            if (!ok) munmap(p, 4096);
        }
    }

    // 还原信号处理器
    if (have_bus)  sigaction(SIGBUS,  &old_bus,  NULL);
    if (have_segv) sigaction(SIGSEGV, &old_segv, NULL);
    if (have_ill)  sigaction(SIGILL,  &old_ill,  NULL);

    if (ok) {
        hlog("[host] 冒烟: JIT 链路畅通, 放行 mono_jit_init");
        return 1;
    }
    hlog("[host] !! 冒烟失败: 四条 JIT 路线（vm_write / mprotect+WRITE / 翻转函数 / RWX直配）均不可用");
    hlog("[host] !! 放弃 mono 初始化（保游戏可玩）。日志上方各路线的失败点即内核给出的精确边界。");
    return 0;
#else
    return 1;   // 桌面/macOS：JIT 天然可用，无需冒烟
#endif
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
/* [Nebula iOS] 探测 allow-jit 是否真生效：尝试一次 MAP_JIT mmap。
   官方文档：allow-jit 仅放行带 MAP_JIT 标志的 mmap；无它则 MAP_JIT mmap 报 EPERM。
   iOS 免费证书签的 provisioning profile 不授权该 entitlement → 系统忽略 → 仍 EPERM。
   探测成功 => entitlement 真生效，主逻辑应改走 MAP_JIT；失败 => 证书/签名问题，plain hack 救不了。*/
static void nebula_probe_map_jit(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_JIT | MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) {
        hlog("[host][probe] MAP_JIT mmap OK addr=%p —— allow-jit 已生效，应走 MAP_JIT 路径", p);
        munmap(p, 4096);
    } else {
        hlog("[host][probe] MAP_JIT mmap FAILED errno=%d —— allow-jit 未生效(免费证书/未嵌对)；plain R-X 在 iOS 硬件 APRR 下也不能执行", errno);
    }
}
#endif

/* [R60] 判断某个符号是否已经住进本 dylib（静态链成功）。
   先查全局命名空间；查不到再 dlopen 自身路径拿 handle 精确查一次。 */
static int nebula_r60_has_symbol(const char *sym) {
    if (dlsym(RTLD_DEFAULT, sym)) return 1;
    const char *self = nebula_r60_self_path();
    if (!self) return 0;
    void *h = dlopen(self, RTLD_NOW);
    if (!h) return 0;
    return dlsym(h, sym) != NULL;
}

/* [R60] NOS 改装包随包带的 BepInEx/core/dobby.dll 是 Windows x64 PE (153088B)，
   iOS arm64 dlopen 不了 → DllNotFoundException: dobby。既然 R60 已把 iOS arm64
   Dobby 静态并进主 dylib，就把这个 PE 挪开（改名，不删，便于回滚），杜绝误加载。 */
static void nebula_r60_quarantine_win_dobby(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char p[PATH_MAX], bak[PATH_MAX];
    snprintf(p, sizeof(p), "%s/Documents/BepInEx/core/dobby.dll", home);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    unsigned char m[2] = {0, 0};
    if (fread(m, 1, 2, f) != 2) { fclose(f); return; }
    fclose(f);
    if (m[0] != 'M' || m[1] != 'Z') return;   // 不是 PE，别动
    snprintf(bak, sizeof(bak), "%s.win.bak", p);
    remove(bak);
    if (rename(p, bak) == 0)
        printf("[host][r60] 已挪走 Windows x64 PE 的 core/dobby.dll -> .win.bak（改用主 dylib 内 arm64 Dobby）\n");
}

/* [R63] iOS arm64 detour 内存翻转 —— 双映射(dual-map) + 隔离 trampoline。
   根因（R62-4/5 两版真机都崩，本版解决）：
     · RWX（R62-5）：iOS APRR/PPL 下 W+X 页不能取指 -> 执行 trampoline 即 SIGSEGV。
     · 纯 RW（R62-4）：mono 把托管 MakeWritable 的 JIT 代码和 detour 目标编译进同一页，
       翻成 RW 后该页 JIT 代码无法取指 -> SIGSEGV。
   APRR 只许 W^X（同 VA 不能同时可写+可执行），而页粒度又让"目标页"和"JIT helper 页"相撞。
   双映射绕开二者：
     · 代码页(detour 目标，mono JIT 页)：不翻原页(保持 RX，JIT helper 安全)，
       vm_remap 出 RW 别名(同物理页，iOS SDK 无 mach_vm.h 故用经典 vm_* API)；
       执行仍从原 RX 取指。
     · trampoline：MemAlloc 改用 mmap 隔离页(RW)，写完后 make_executable 翻 RW->RX(W^X)，
       与 mono 自身 JIT 同款，且页隔离避免踩相邻堆。
   判别：MemAlloc 的 mmap 页记入 g_nebula_tramp；make_writable 对其不动、对其余(代码页)建别名；
   make_executable 对别名页只刷 icache、对 trampoline 翻 RW->RX。 */
#define NEBULA_ALIAS_MAX 64
static struct { uintptr_t orig; uintptr_t alias; } g_nebula_alias[NEBULA_ALIAS_MAX];
static int g_nebula_alias_n = 0;

/* MemAlloc 的 mmap 隔离页基址+大小，用于判别 trampoline（需翻 RX）vs 代码页（双映射） */
#define NEBULA_TRAMP_MAX 64
static struct { uintptr_t base; size_t size; } g_nebula_tramp[NEBULA_TRAMP_MAX];
static int g_nebula_tramp_n = 0;

static long nebula_ps(void) { long ps = sysconf(_SC_PAGESIZE); return ps > 0 ? ps : 16384; }

static int nebula_is_tramp(uintptr_t base) {
    for (int i = 0; i < g_nebula_tramp_n; i++)
        if (g_nebula_tramp[i].base == base) return 1;
    return 0;
}

/* 查/建 某代码页 的 RW 别名（与原页同物理）；无则回原基址 */
static uintptr_t nebula_jit_alias_base(uintptr_t base) {
    long ps = nebula_ps();
    base &= ~((uintptr_t)ps - 1);
    for (int i = 0; i < g_nebula_alias_n; i++)
        if (g_nebula_alias[i].orig == base) return g_nebula_alias[i].alias;
    if (g_nebula_alias_n >= NEBULA_ALIAS_MAX) return base;
    vm_address_t ali = 0;
    vm_size_t sz = (vm_size_t)ps;
    vm_prot_t curp = 0, maxp = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &ali, sz, 0,
        VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)base,
        FALSE, &curp, &maxp, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        hlog("[host][r63] !! dual-map remap 失败 kr=%d @%p（回退原页，可能崩）", (int)kr, (void *)base);
        return base;
    }
    /* 别名页强制 RW（remap 默认可能沿用源 RX），写跳转用 */
    vm_protect(mach_task_self(), ali, sz, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    g_nebula_alias[g_nebula_alias_n].orig = base;
    g_nebula_alias[g_nebula_alias_n].alias = (uintptr_t)ali;
    g_nebula_alias_n++;
    hlog("[host][r63] dual-map + @%p -> %p (RW 别名, 同物理页)", (void *)base, (void *)ali);
    return (uintptr_t)ali;
}

/* 给定任意地址，返回应写入的 VA（代码页走 RW 别名，其余原样） */
__attribute__((visibility("default")))
void *nebula_jit_alias(void *addr) {
    long ps = nebula_ps();
    uintptr_t ua = (uintptr_t)addr;
    uintptr_t base = ua & ~((uintptr_t)ps - 1);
    for (int i = 0; i < g_nebula_alias_n; i++)
        if (g_nebula_alias[i].orig == base)
            return (void *)(g_nebula_alias[i].alias + (ua - base));
    return addr;
}

__attribute__((visibility("default")))
int nebula_jit_make_writable(void *addr, uint32_t size) {
    long ps = nebula_ps();
    uintptr_t ua = (uintptr_t)addr;
    uintptr_t base = ua & ~((uintptr_t)ps - 1);
    if (nebula_is_tramp(base)) {
        hlog("[host][r63] make_writable: trampoline 页(已 RW, 不动) @%p", (void *)base);
        return 0;
    }
    /* 代码页：建 RW 别名，原页保持 RX（JIT helper 安全、APRR 合规） */
    uintptr_t ali = nebula_jit_alias_base(base);
    hlog("[host][r63] make_writable: 代码页 @%p 别名=%p", (void *)base, (void *)ali);
    return 0;
}

__attribute__((visibility("default")))
int nebula_jit_make_executable(void *addr, uint32_t size) {
    long ps = nebula_ps();
    uintptr_t ua = (uintptr_t)addr;
    uintptr_t base = ua & ~((uintptr_t)ps - 1);
    /* 代码页(已双映射)：只刷 icache，绝不翻原页（保持 RX，JIT helper 安全） */
    for (int i = 0; i < g_nebula_alias_n; i++) {
        if (g_nebula_alias[i].orig == base) {
            sys_icache_invalidate((void *)g_nebula_alias[i].alias, ps);
            sys_icache_invalidate((void *)base, ps);
            hlog("[host][r63] make_executable: 代码页 flush(不翻) @%p", (void *)base);
            return 0;
        }
    }
    /* 其余(trampoline)：W^X 翻 RW->RX，再刷 icache */
    int r = vm_protect(mach_task_self(), base, (vm_size_t)ps, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    sys_icache_invalidate((void *)base, ps);
    hlog("[host][r63] make_executable: 翻 RW->RX @%p -> %d", (void *)base, r);
    return r;
}

/* [R63] trampoline 用 mmap 隔离页(RW)，避免与相邻堆同页被翻 RX 踩踏；并记录基址/大小用于判别 */
__attribute__((visibility("default")))
void *nebula_jit_mem_alloc(uint32_t size) {
    long ps = nebula_ps();
    size_t sz = (size_t)((size + (size_t)ps - 1) & ~((size_t)ps - 1));
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { hlog("[host][r63] !! mem_alloc mmap 失败 errno=%d", errno); return NULL; }
    uintptr_t base = (uintptr_t)p & ~((uintptr_t)ps - 1);
    if (g_nebula_tramp_n < NEBULA_TRAMP_MAX) {
        g_nebula_tramp[g_nebula_tramp_n].base = base;
        g_nebula_tramp[g_nebula_tramp_n].size = sz;
        g_nebula_tramp_n++;
    }
    hlog("[host][r63] mem_alloc %u -> %p (mmap RW 隔离页)", size, p);
    return p;
}

__attribute__((visibility("default")))
int nebula_jit_mem_free(void *ptr) {
    if (!ptr) return 0;
    uintptr_t base = (uintptr_t)ptr & ~((uintptr_t)nebula_ps() - 1);
    for (int i = 0; i < g_nebula_tramp_n; i++) {
        if (g_nebula_tramp[i].base == base) {
            size_t freed = g_nebula_tramp[i].size;
            munmap((void *)g_nebula_tramp[i].base, freed);
            /* 从记录中摘除（用末项覆盖，保持顺序无关） */
            g_nebula_tramp[i] = g_nebula_tramp[--g_nebula_tramp_n];
            hlog("[host][r63] mem_free %p (munmap %zu)", ptr, freed);
            return 0;
        }
    }
    hlog("[host][r63] mem_free %p 不在 mmap 记录, 跳过", ptr);
    return 0;
}

/* [R64c] CoreLib P/Invoke shim 补全：主 dylib 静态链入的 System.Native 缺
   SystemNative_IsATty 符号；libSystem.Native 被 dllmap 到主 dylib 自身后，
   Console.IsOutputRedirected -> Interop.Sys.IsATty 解析失败抛
   EntryPointNotFoundException（BepInEx 初始化期 DetourRuntimeILPlatform..ctor
   即触发），并污染 MonoMod detour runtime 初始化态。此处导出同名 stub 供
   mono 解析命中。iOS 无交互式终端，stdout/stderr 已重定向到日志文件，返回 0
   （非 tty）使 Console 走重定向路径正常输出，不再抛异常。 */
__attribute__((visibility("default"), weak))
int SystemNative_IsATty(int fd) {
    (void)fd;
    return 0;   /* 非 tty：Console 认为输出已重定向，写文件/空设备不崩 */
}

/* [Nebula iOS 第二十轮] 用 mono_dllmap_insert 把 .NET 8 CoreLib 的 P/Invoke
   原生库名（libSystem.Native 等）重映射到 iOS 版 shim。
   [R60] 改造：dobby + 5 个 libSystem.* 全部【静态链进主 dylib】，dllmap 一律
   指向主 dylib 自身路径 → dlopen(自身) + dlsym 命中导出符号 → 成品只有一个 dylib。
   若 CI 没带 libSystem.*.a（静态链失败），则回退到文件：app bundle 里现成的
   shim，或启动时从 blob 释放到 Documents/nebula_native/ 的那份。 */
static void nebula_setup_native_dllmaps(const char *appdir)
{
    static const char *shims[] = {
        "libSystem.Native.dylib",
        "libSystem.Globalization.Native.dylib",
        "libSystem.IO.Compression.Native.dylib",
        "libSystem.Net.Security.Native.dylib",
        "libSystem.Security.Cryptography.Native.Apple.dylib",
        NULL
    };

    const char *self = nebula_r60_self_path();

    /* ---- 1) dobby：修「播完 Innersloth 后崩」的根因 ---- */
    nebula_r60_quarantine_win_dobby();
    if (nebula_r60_has_symbol("DobbyHook") && self) {
        static const char *dobby_names[] = { "dobby", "libdobby", "dobby.dll", NULL };
        for (int i = 0; dobby_names[i]; i++)
            mono_dllmap_insert(NULL, dobby_names[i], NULL, self, NULL);
        hlog("[host][r60] dllmap: dobby -> %s（iOS arm64 Dobby 已静态并入主 dylib，chainloader 不再 DllNotFound）", self);
    } else {
        hlog("[host][r60] !! 主 dylib 未导出 DobbyHook —— BepInEx 仍会 DllNotFoundException: dobby");
    }

    /* ---- 1.5) nebula_jit：R62 detour 内存翻转原生函数（vm_protect/mprotect + icache）---- */
    if (self) {
        mono_dllmap_insert(NULL, "nebula_jit", NULL, self, NULL);
        hlog("[host][r62] dllmap: nebula_jit -> %s（detour 内存翻转原生函数）", self);
    }

    /* ---- 2) CoreLib 的 5 个 P/Invoke shim ---- */
    int static_shim = 0;
    {
        static const char *probes[] = {
            "SystemNative_GetEnv", "SystemNative_GetPid", "SystemNative_Open",
            "SystemNative_Stat", "GlobalizationNative_GetLocaleName", NULL
        };
        for (int i = 0; probes[i]; i++)
            if (nebula_r60_has_symbol(probes[i])) { static_shim = 1; break; }
    }
    if (static_shim && self) {
        for (int i = 0; shims[i]; i++) {
            size_t L = strlen(shims[i]);
            char name[256];
            memcpy(name, shims[i], L - 6); name[L - 6] = '\0';  // 去 ".dylib"
            mono_dllmap_insert(NULL, name, NULL, self, NULL);
            hlog("[host][r60] dllmap: %s -> 主 dylib 自身（静态链，单文件）", name);
        }
        return;
    }
    hlog("[host][r60] 主 dylib 未含 SystemNative_*，CoreLib shim 走文件 fallback");

    /* fallback：找磁盘上的 shim（原有逻辑 + R60 的 blob 释放目录） */
    char nativedir[PATH_MAX] = {0};
    const char *home = getenv("HOME");
    if (home && home[0]) snprintf(nativedir, sizeof(nativedir), "%s/Documents/nebula_native", home);
    const char *dirs[] = { "", "native", "Frameworks", "Frameworks/native", NULL };
    char path[PATH_MAX];
    for (int i = 0; shims[i]; i++) {
        int found = 0;
        if (nativedir[0]) {
            snprintf(path, sizeof(path), "%s/%s", nativedir, shims[i]);
            if (access(path, R_OK) == 0) found = 1;
        }
        for (int d = 0; dirs[d] && !found; d++) {
            if (dirs[d][0])
                snprintf(path, sizeof(path), "%s/%s/%s", appdir, dirs[d], shims[i]);
            else
                snprintf(path, sizeof(path), "%s/%s", appdir, shims[i]);
            if (access(path, R_OK) == 0) found = 1;
        }
        if (!found) {
            hlog("[host] native shim 未找到(跳过 dllmap): %s", shims[i]);
            continue;
        }
        size_t L = strlen(shims[i]);
        char name[256];
        memcpy(name, shims[i], L - 6); name[L - 6] = '\0';
        mono_dllmap_insert(NULL, name, NULL, path, NULL);
        hlog("[host] dllmap: %s -> %s", name, path);
    }
}

/* boot 是否已跑完（无论成功还是中途失败退出） */
static volatile int g_boot_complete = 0;

static void *boot_thread_impl(void *arg) {
    (void)arg;
    hlog("[host] boot thread started");
    // 全球化：默认 invariant 模式，避免 iOS 上缺 icudt.dat 崩溃（游戏模组走 UTF-8，不受影响）。
    // 若要完整文化格式化，放 icudt.dat 到 dylib 同目录并删此行。
    setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 1);

    {   // 尽早开日志（早到能覆盖后面每一步），并依次试多个可写位置
        char p[PATH_MAX];
        const char *home = getenv("HOME");
        if (home) { snprintf(p, sizeof(p), "%s/Documents/nebula_host.txt", home); log_try(p); }
        const char *gd = getenv("NEBULA_GAME_DIR");
        if (gd)   { snprintf(p, sizeof(p), "%s/nebula_host.txt", gd);             log_try(p); }
        log_try("/var/mobile/nebula_host.txt");
        hlog("[host] === boot start ===");
    }

#if defined(__APPLE__) && TARGET_OS_IPHONE
    nebula_probe_map_jit();   // 尽早确认 allow-jit 是否真生效，决定后续 JIT 路径
#endif

    start_watchdog();   // 尽早启动内存看门狗，覆盖 mono_jit_init 全程（含其内存高峰）

    char *dir = module_dir();
    hlog("[host] module dir = %s", dir);

    // 游戏根目录（BepInEx 所在处）。iOS 上由注入方指定，默认用 dylib 同目录。
    const char *root = getenv("NEBULA_GAME_DIR");
    if (!root || !*root) root = dir;
    hlog("[host] game root = %s", root);

    if (!g_logfp) {   // 前面候选都没成功，再试游戏根目录 / dylib 同目录
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/nebula_host.txt", root); log_try(p);
        snprintf(p, sizeof(p), "%s/nebula_host.txt", dir);  log_try(p);
    }
    if (!g_logfp) hlog("[host] !! 日志无法创建：所有候选路径都不可写");

    // [第十二轮] 尽早装死点取证：崩溃 trap（含 PC/LR 符号化）+ stderr/stdout 捕获。
    // 必须在 JIT 冒烟之前 —— 冒烟会暂存并还原信号处理器，trap 装在前，
    // 冒烟结束后自动恢复回来，mono_jit_init 全程都有现场取证。
#if defined(__APPLE__) && TARGET_OS_IPHONE
    neb_install_trap();
    neb_capture_stdio();
    hlog("[host] step: trap+stdio 捕获已装（mono 遗言将现形）");
    neb_install_mach_trap();   // [第十五轮] thread 级 mach 端口：先于 BSD signal 抓真实 fault 地址
#endif

    // [R60] 启动画面：无条件强制播放 "AUI Mod Loader by Aiden" 满 3 秒。
    //   ① 不再是「正在生成interop」占位 —— R60 设备端永不生成 interop，
    //      那个文案会误导；② 用户明确要求每次启动强制播放 3s 品牌页；
    //   ③ 最短展示时长由 boot_overlay.m 内部把关，Nebula 就绪信号也不得提前销毁。
    //   窗口可能还没起来，内部带重试；30s 安全超时兜底销毁（绝不卡屏）。
    hlog("[host][r60] 启动画面：强制播放 3s AUI Mod Loader by Aiden");
    nebula_show_boot_overlay();

    // 大小写回退必须在知道游戏根目录后装（只对它生效），且默认关闭
    hlog("[host] step: cifs init");
    nebula_cifs_init(root);

    hlog("[host] step: load_cfg");
    load_cfg(dir);
    hlog("[host] step: apply_gc_env");
    apply_gc_env();
    hlog("[host] step: blob_load");
    blob_load();
    hlog("[host] step: blob ok, assemblies=%d", g_n);
    // [R60] 立刻把内嵌的预生成 interop / native shim 释放到磁盘。
    //   必须在 Doorstop.Entrypoint.Start() 之前：BepInEx 的 Il2CppInteropManager
    //   见到空的 BepInEx/interop 会无视 UpdateInteropAssemblies=false 现场生成
    //   （R58 真机整夜卡死）。释放后目录恒有 131 个 dll → 设备端永不生成，
    //   用户也不必再手动放 zip。
    nebula_r60_extract_bundle(root);

    // [第十三轮关键修复] 预加载钩子必须在 mono_jit_init【之前】装好！
    // mono 在 mono_init 阶段就要通过预加载钩子加载 corlib（System.Private.CoreLib），
    // 此前钩子装在 mono_jit_init 之后，导致 corlib 无从加载 →
    // mono_assembly_load_corlib 触发 g_assert_not_reached → SIGABRT（崩点已实锤）。
    // 现在提前装，mono 初始化加载 corlib 时即可从 __nebulabcl 内存命中。
    if (g_n > 0) mono_install_assembly_preload_hook(nebula_preload, NULL);
    hlog("[host] step: 预加载钩子已装（%d 个程序集可被 mono 内存命中）", g_n);

    hlog("[host] step: setup_mono_path");
    setup_mono_path(dir, root);

    const char *prefix = getenv("MONO_PREFIX");
    if (prefix) {
        char cfg[PATH_MAX];
        snprintf(cfg, sizeof(cfg), "%s/etc", prefix);
        mono_set_dirs(prefix, cfg);
    }
    hlog("[host] step: mono_config_parse");
    mono_config_parse(NULL);

    // 单一编译器（JIT）版本：废除解释器，standard / pro 合并为一份，始终走 JIT 编译执行。
    // 实测旧两种模式都要在 mono_trampolines_init 生成 generic trampoline（写 ARM64 代码到
    // 可执行内存），解释器并未省下 JIT 能力，反而更慢、还多一套构建分支 —— 故废除。
    // MAP_JIT 分配保留（配合 mono 侧 dlsym 探测翻转：真机官方 JIT 走 W^X 翻转；
    // StikDebug 解析不到则跳过，RWX 页直接写）。
    mono_setmmapjit(1);
    hlog("[host] 编译器(JIT)模式：强制 MAP_JIT 分配（trampoline 生成需可执行内存）");

    // [Nebula iOS 第十七轮] 强制纯 JIT、关闭 AOT：本工程 BCL 已改用纯 IL 包
    // （microsoft.netcore.app.runtime.mono.osx-arm64，零 AOT 模块），不打包任何 AOT
    // 预编译镜像。iOS 构建可能把 mono_aot_only 默认置 1（仅 AOT），会拒绝 JIT 并报
    // "method not AOT-compiled"。这里在 mono_jit_init 前显式置 0，放行 JIT，让所有
    // 程序集（含 System.Private.CoreLib）由 JIT 从 IL 编译、走全指针引用（已为 ASLR
    // 打过补丁），从根上消除 AOT rva 重定基缺失导致的 SIGSEGV。
    mono_jit_set_aot_only(0);
    hlog("[host] 强制 mono_aot_only=0（纯 JIT，关闭 AOT；BCL 为纯 IL 零 AOT 模块）");

    // [Nebula iOS 第二十轮] 注入原生 shim dllmap：让 CoreLib 的 P/Invoke 找到 iOS 版
    // libSystem.*.dylib（getenv/文件IO/加密/压缩/TLS），避免 DllNotFoundException。
    nebula_setup_native_dllmaps(dir);

    hlog("[host] step: mono_jit_init ...");

    // 先做 JIT 能力闸门：没 JIT 必崩在 mono_jit_init（标准/pro 都一样）。
    // 有则放行；没有则等调试器附加，仍没有就优雅退出（保留游戏可玩、不硬崩）。
    if (!nebula_wait_for_jit()) {
        hlog("[host] step: 跳过 mono_jit_init（无 JIT 能力）。游戏本体不受影响，仅模组功能不可用。");
        nebula_hide_boot_overlay();   // 无 JIT：销毁占位文字，让原版游戏正常可玩
        free(dir);
        return NULL;
    }

    hlog("[host] step: jit_smoke");
    if (!nebula_jit_smoke()) {
        hlog("[host] step: 跳过 mono 初始化（JIT 冒烟未通过）。游戏本体不受影响，仅模组功能不可用。");
        nebula_hide_boot_overlay();
        free(dir);
        return NULL;
    }

    hlog("[host] step: mono_jit_init 进入, RSS=%ldMB", rss_mb());
    {   // [第十二轮] 进 init 前最后一次确认调试态：若 StikDebug 已分离导致
        // CS_DEBUGGED 掉位，匿名可执行页的执行权会随之消失（冒烟通过≠init 时仍有效）
        int f2 = 0;
        if (csops(getpid(), CS_OPS_STATUS, &f2, sizeof(f2)) == 0)
            hlog("[host] init 前 csflags=0x%x CS_DEBUGGED=%d", f2, (f2 & CS_DEBUGGED) ? 1 : 0);
    }
    MonoDomain *domain = mono_jit_init("NebulaHost");
    if (!domain) { hlog("[host] !! mono_jit_init failed"); free(dir); return NULL; }
    g_mono_ready = 1;   // 看门狗自此起可安全 GC
    // [R63] boot 线程是 mono 主线程，但 coop 挂起要求线程显式 mono_thread_attach 登记
    // safepoint 状态；否则本线程上隐式 GC（JIT 编译期 mono_ldstr 等分配触发）sgen 分配器
    // 状态不一致 -> sgen_try_alloc_obj_nolock 里 str x19,[x21,#0x40] 段错误。
    // 看门狗线程已在 watchdog 内 attach；这里补 boot 线程自身（幂等，mono_jit_init 后调用）。
    extern void mono_thread_attach(MonoDomain *d);
    mono_thread_attach(domain);
    hlog("[host] step: mono_jit_init OK, RSS=%ldMB", rss_mb());

    hlog("[host] step: Mono 就绪, RSS=%ldMB", rss_mb());
    start_watchdog();

    // 分阶段：先把虚拟机本身跑稳并回收一次，再进入模组加载，
    // 不让 VM 初始化和全部模组/资源在同一时刻一起压进来。
    hlog("[host] phase1: VM 就绪，先 GC 回收一次, RSS=%ldMB", rss_mb());
    mono_gc_collect(mono_gc_max_generation());
    hlog("[host] phase1: GC 完成, RSS=%ldMB", rss_mb());

    // NEBULA_BOOT: bepinex | mods | engine | auto（默认）
    // auto 的顺序：BepInEx → Mods 目录 → Engine POC
    // 之所以先 BepInEx：BepInEx 起来后它自己会去加载 plugins/ 里的所有插件，
    // 覆盖面最广；它不在场时才退到通用 Mods 扫描。
    const char *bm = getenv("NEBULA_BOOT");
    int auto_mode = !bm || !*bm || !strcmp(bm, "auto");
    int ok = 0;

    if (auto_mode || !strcmp(bm, "bepinex")) {
        // #5 防 Interop 生成 OOM：BepInEx 现场解析原版二进制生成 Il2CppInterop 桥接类，
        // 一次性吃掉大量托管内存。进入前先强制全量 GC 腾出空间，降低 jetsam 风险。
        hlog("[host] phase2 前强制 GC（防 Interop 生成 OOM）, RSS=%ldMB", rss_mb());
        mono_gc_collect(mono_gc_max_generation());
        hlog("[host] phase2: 进入模组加载(BepInEx/星云), RSS=%ldMB", rss_mb());
        hlog("[host] step: boot_bepinex enter");
        ok = boot_bepinex(domain, root);
        hlog("[host] step: boot_bepinex -> %d", ok);
    }
    if (!ok && (auto_mode || !strcmp(bm, "mods"))) {
        hlog("[host] step: boot_mods_dir enter");
        ok = boot_mods_dir(domain, root);
        hlog("[host] step: boot_mods_dir -> %d", ok);
    }
    if (!ok && (auto_mode || !strcmp(bm, "engine"))) {
        hlog("[host] step: boot_engine_poc enter");
        ok = boot_engine_poc(domain, dir);
        hlog("[host] step: boot_engine_poc -> %d", ok);
    }
    if (!ok) hlog("[host] !! 没有任何启动路径成功");

    // [R60] 启动画面收尾：请求销毁即可，boot_overlay.m 内部按「首次显示时间戳」
    //   强制满 3 秒才真正移除（Nebula 就绪信号也不得提前销毁）。这里默认 3000ms
    //   与之对齐；安全超时(默认30s)在 boot_overlay.m 内兜底。
    {
        int hold = 3000;
        const char *he = getenv("NEBULA_BOOT_OVERLAY_HOLD_MS");
        if (he && *he) { int v = atoi(he); if (v >= 0) hold = v; }
        if (hold > 0) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)hold * NSEC_PER_MSEC),
                           dispatch_get_main_queue(), ^{ nebula_hide_boot_overlay(); });
            hlog("[host] 占位文字将在 %dms 后销毁（或 Nebula 就绪即提前销毁）", hold);
        } else {
            nebula_hide_boot_overlay();
            hlog("[host] 占位文字立即销毁（NEBULA_BOOT_OVERLAY_HOLD_MS=0）");
        }
    }

    watchdog_run = 0;

    // [R43 关键] BepInEx 起来后【绝不】mono_jit_cleanup。
    // BepInEx 6 IL2CPP 的插件加载根本不在 Preloader 里发生：IL2CPPChainloader.Initialize()
    // 只做一件事——用 INativeDetour 钩住 il2cpp_runtime_invoke；真正的 chainloader.Start()
    //（扫 plugins/ 加载 NOS、打 Harmony 补丁）要等 Unity 跑起来、首次切场景时
    // IL2CPP 调 il2cpp_runtime_invoke 命中那个方法名才触发（IL 实证，见 Preloader.Run 末尾
    // 只有 Initialize 没有 Start）。也就是插件加载发生在 Unity 之后，Mono 域必须活到那时。
    // 旧实现在这里无条件 cleanup，等于把 BepInEx 连同刚生成的一切一起拆掉 ——
    // 这正是「boot_bepinex -> 1、游戏正常进、NOS 永远不出现」的直接原因。
    if (ok) {
        hlog("[host] BepInEx 已起：保留 Mono 域常驻（不 cleanup —— 插件要等 Unity 切场景才加载）");
        /* [R56] 只有真的加载了 BepInEx.Unity.IL2CPP（有 image）才登记 domain：
           dobby detour 失败不影响 chainloader 实例（Initialize 里 set_Instance 在
           TryLoad/detour 之前已完成，R55 真机已打印 Chainloader initialized）。
           放行 Unity 后由主线程主动调 NebulaManualExecute() 顶替 detour 的延迟触发。 */
        if (g_r56_il2cpp_img) {
            g_r56_domain = domain;
            hlog("[host][r56] 已登记 Mono domain + BepInEx image，准备主线程主动触发 Execute()");
        } else {
            hlog("[host][r56] !! 无 BepInEx image（走的不是 BepInEx 路径），跳过主动触发");
        }
    } else {
        hlog("[host] BepInEx 未成功：清理 Mono 域，交还游戏");
        mono_jit_cleanup(domain);
    }
    free(dir);
    return NULL;
}

/* 包装层：不论 impl 从哪条 return 退出都置完成标志，避免逐条返回点去改 */
static void *boot_thread(void *arg) {
    void *r = boot_thread_impl(arg);
    g_boot_complete = 1;
    return r;
}

/* ============================================================================
 * [R56] 绕开 native inline hook，由宿主在 Unity 主线程主动触发 chainloader
 *
 * 【为什么】R55 真机 preloader.log 全文：
 *   System.DllNotFoundException: dobby
 *     at DobbyDetour.PrepareImpl / INativeDetour.CreateAndApply /
 *        IL2CPPChainloader.Initialize / Preloader.Run / Doorstop.Entrypoint.Start
 *   此前 [r55] TryLoad OK -> GetExport(il2cpp_runtime_invoke) 已经打通，
 *   但设备 BepInEx/core/dobby.dll 是 Windows x64 PE (153088 B)，iOS arm64
 *   dlopen 不了；iOS 侧载环境改 __TEXT 做 inline hook 也不现实。detour 装不上
 *   → il2cpp_runtime_invoke 永不被钩 → OnInvokeMethod 不触发 → plugins 不扫
 *   → 进原版、NOS 不加载。
 *
 * 【能绕开的依据】IL 实证 OnInvokeMethod 只是「一次性延迟触发器」：
 *   命中 Internal_ActiveSceneChanged 后执行 SetupUnityLogging() +
 *   PreloadInteropAssemblies() + Instance.Execute()，然后 Dispose 卸掉自己。
 *   三段全是纯 managed（Preload 只是 Assembly.Load(interop/*.dll)，Execute 扫
 *   plugins 加载 NebulaLoader），都不依赖 dobby。R56 的 BepInEx.Unity.IL2CPP.dll
 *   已新增 public static IL2CPPChainloader.NebulaManualExecute()，把这三段原样
 *   搬出来，并带 catch(Exception) 把完整堆栈写进 LogOutput.log。
 *   Initialize() 里 set_Instance 在 TryLoad/detour 之前就完成了（R55 真机已打印
 *   Chainloader initialized），detour 抛异常被 Doorstop 吞掉不影响 Instance。
 *
 * 【线程与时机】必须用 dispatch_get_main_queue() 在 Unity 主线程调用：
 *   - Il2CppInterop 桥接类首次访问会触发 Il2CppInterop.Runtime.IL2CPP 的 .cctor
 *     → il2cpp_domain_get()。R52 的 native SIGSEGV 就是 preloader 阶段 Unity 尚未
 *     il2cpp_init 就碰它；这里等 boot() 放行、Unity 初始化完再调。
 *   - il2cpp_init 在主线程发生并自动 attach 该线程，插件 Load() 访问游戏类才安全。
 *   - Mono 域是 boot_thread 里 mono_jit_init 的，主线程默认未 attach，故先
 *     mono_thread_attach() 再 mono_runtime_invoke()。
 *
 * 【延迟】默认 4000ms（NEBULA_EXEC_DELAY_MS 可调，0~60000），从 boot() 放行
 *   Unity 起算，覆盖 didFinishLaunching / il2cpp_init / 首个场景加载。
 * ==========================================================================*/
static int64_t nebula_r56_delay_ms(void) {
    const char *e = getenv("NEBULA_EXEC_DELAY_MS");
    if (!e || !*e) return 4000;
    long v = strtol(e, NULL, 10);
    if (v < 0) v = 0;
    if (v > 60000) v = 60000;
    return (int64_t)v;
}

static void nebula_r56_invoke_manual_execute(void) {
    printf("[host][r56] ===== Unity 主线程触发开始 =====\n");
    fflush(NULL);
    if (!g_r56_domain || !g_r56_il2cpp_img) {
        printf("[host][r56] !! domain/image 为空，放弃触发\n");
        fflush(NULL);
        return;
    }

    /* 函数内 extern：iOS 自定义 mono 头可能未声明（与 watchdog_thread 一致） */
    extern void mono_thread_attach(MonoDomain *domain);
    mono_thread_attach(g_r56_domain);
    printf("[host][r56] mono_thread_attach 完成\n");
    fflush(NULL);

    MonoClass *k = mono_class_from_name(g_r56_il2cpp_img,
                                        "BepInEx.Unity.IL2CPP", "IL2CPPChainloader");
    if (!k) {
        printf("[host][r56] !! 找不到 IL2CPPChainloader 类\n");
        fflush(NULL);
        return;
    }
    MonoMethod *m = mono_class_get_method_from_name(k, "NebulaManualExecute", 0);
    if (!m) {
        printf("[host][r56] !! 找不到 NebulaManualExecute() —— 内嵌 DLL 不是 r56 版？\n");
        fflush(NULL);
        return;
    }

    printf("[host][r56] 已定位 NebulaManualExecute()，开始 mono_runtime_invoke（扫 plugins / 加载 NOS）\n");
    fflush(NULL);
    MonoObject *exc = NULL;
    mono_runtime_invoke(m, NULL, NULL, &exc);
    if (exc) {
        fprintf(stderr, "[host][r56] !! managed 抛出异常（类型见下）：\n");
        nebula_log_exception(exc);
    } else {
        printf("[host][r56] mono_runtime_invoke 返回（无托管异常）；插件加载结果看 LogOutput.log 的 [r56] 行\n");
    }
    printf("[host][r56] ===== Unity 主线程触发结束 =====\n");
    fflush(NULL);
}

static void nebula_r56_schedule_execute(void) {
    if (!g_r56_domain || !g_r56_il2cpp_img) {
        printf("[host][r56] BepInEx 入口未就绪(domain=%p img=%p)，跳过主动触发\n",
               (void *)g_r56_domain, (void *)g_r56_il2cpp_img);
        fflush(NULL);
        return;
    }
    int64_t d = nebula_r56_delay_ms();
    printf("[host][r56] 已派发：%lld ms 后在 Unity 主线程调用 IL2CPPChainloader.NebulaManualExecute()\n", (long long)d);
    printf("[host][r57] 修复要点：dobby 装不上已用「宿主主线程主动触发」绕开；R57 进一步跳过 iOS IL2CPP 缺失的 SetupUnityLogging(op_Implicit)，直接 PreloadInteropAssemblies + Execute() 扫 plugins / 加载 NOS\n");
    printf("[host][r58] 下一步：R57 已让 NOS 真正加载并在 GateThread:.cctor 初始化，但崩在 il2cpp 调用（PC 17.3s interop vs iPad 17.3.1o 版本错配）；本版强制在设备端为 17.3.1o 重新生成 interop（外来 interop 已备份到 interop.bak/）\n");
    printf("[host][r56] 延迟可用 NEBULA_EXEC_DELAY_MS 调整（当前 %lld ms）；Execute() 异常堆栈会写进 LogOutput.log 并由长期 tail 转发\n", (long long)d);
    fflush(NULL);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, d * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        nebula_r56_invoke_manual_execute();
    });
}

static void boot(void) {
    // 【阻塞式 boot】默认主线程在这里等到 boot_thread 全部结束才返回 ——
    // 期间游戏保持占位黑屏，Unity / Innersloth / 游戏本体一律不得提前开跑。
    //
    // 背景（本 bug 的根因）：旧实现 pthread_detach 后主线程立刻放行，
    // Unity 与 preloader 变成并行。于是 BepInEx 还在啃 UnityFramework 生成 interop 时，
    // 游戏已经把 Unity 初始化、Coroutine、甚至 IAP 商品列表都跑完了；
    // 等 interop 生成完毕，游戏早已加载完成，NOS 再也找不到插入时机 —— 表现就是"永远加载不了"。
    // PC 上 Doorstop 是同步阻塞的（preloader 跑完才轮到 Unity），这里必须对齐同样的语义。
    //
    // NEBULA_BLOCK_BOOT=0 可退回旧的异步行为（仅供对照实验）。
    const char *blk = getenv("NEBULA_BLOCK_BOOT");
    int block = !(blk && strcmp(blk, "0") == 0);

    pthread_t t;
    if (pthread_create(&t, NULL, boot_thread, NULL) != 0) return;

    if (block) {
        // 关键：绝不能用 pthread_join 纯死等 —— 主线程一卡死，主队列上的占位文字
        // 与省略号动画统统得不到调度（用户看到黑屏无字），而且会被 iOS 看门狗
        // 判定无响应（0x8badf00d）直接杀进程。
        // 改为「等待期间持续泵浦主运行循环」：主线程始终是“正在处理事件”的健康态，
        // 同时因为 boot 还没返回，Unity 依旧没法往下走 —— 两边要求同时满足。
        hlog("[host] boot 阻塞模式：等待 BepInEx/interop 生成完成（期间泵浦主运行循环，占位动画持续刷新）");
        while (!g_boot_complete) {
            /* 每次最多跑 50ms 就让出，循环里刷新 CFRunLoop → GCD 主队列得以执行 */
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        }
        pthread_join(t, NULL);
        hlog("[host] boot 阻塞结束，放行 Unity 继续加载（游戏与 NOS 一同加载）");
        /* [R56] 派发点必须在 pthread_join 之后：此时 boot_thread 已结束、domain/image
           已登记，且 boot() 即将返回放行 Unity。从这一刻起算延迟，block 由主线程
           runloop 在 Unity 初始化完成后执行，天然对齐官方 detour「Unity 起来后触发」
           的语义。非阻塞分支（NEBULA_BLOCK_BOOT=0）只作对照实验，不派发。 */
        /* [R60] dobby 已静态并入主 dylib → chainloader 官方 detour
           （INativeDetour 钩住 il2cpp_runtime_invoke）应当装得上，OnInvokeMethod
           会在 Unity 首次切场景时正常触发 Instance.Execute()，无需 R56 兜底。
           真机日志实证：R56 兜底的 NebulaManualExecute() 在播完 Innersloth 后
           于 mono_jit_runtime_invoke SIGSEGV —— 正是「手动放了 interop、播完
           innersloth 以后也崩」的直接原因，故 dobby 就绪时必须跳过它。
           若设备端 dobby inline hook 因 code signing 失败导致 detour 没装上，
           可设 NEBULA_FORCE_MANUAL_EXECUTE=1 恢复旧兜底做对照。 */
        {
            const char *fm = getenv("NEBULA_FORCE_MANUAL_EXECUTE");
            int force_manual = (fm && !strcmp(fm, "1"));
            if (nebula_r60_has_symbol("DobbyHook") && !force_manual) {
                hlog("[host][r60] dobby 已就绪：跳过 R56 兜底 NebulaManualExecute（该兜底是播完 Innersloth 后 SIGSEGV 源头），交由 chainloader 官方 detour 触发 Execute()");
            } else {
                hlog("[host][r60] dobby 未就绪或强制兜底：仍派发 R56 NebulaManualExecute");
                nebula_r56_schedule_execute();
            }
        }
    } else {
        pthread_detach(t);
    }
}

#ifdef BUILD_AS_LIB
__attribute__((constructor))
static void on_load(void) {
    // 最早能拿到的日志：系统日志 os_log，不依赖沙盒/文件，且先于 boot 建立，
    // 这样即使 boot 之前崩，也能在 idevicesyslog 里看到“ctor entered”。
    g_neb_log = os_log_create("com.nebula.host", "host");
    os_log(g_neb_log, "ctor entered (dylib 已加载, before boot)");
    // 崩时至少能知道崩在哪一步（覆盖 SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGTRAP）
    signal(SIGSEGV, neb_crash);
    signal(SIGABRT, neb_crash);
    signal(SIGBUS,  neb_crash);
    signal(SIGILL,  neb_crash);
    signal(SIGTRAP, neb_crash);
    boot();
}
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    boot();
    sleep(2);   // 桌面测试时等后台线程跑完
    return 0;
}
