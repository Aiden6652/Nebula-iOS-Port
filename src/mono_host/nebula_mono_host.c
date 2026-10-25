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
#include <strings.h>    // strcasecmp

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
#include <setjmp.h>                // sigsetjmp/siglongjmp（冒烟段错误兜底）
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <TargetConditionals.h>   // TARGET_OS_IPHONE / TARGET_OS_MAC
#include <os/log.h>
#include <signal.h>
#include <dispatch/dispatch.h>     // 启动占位文本的延时/主线程销毁
#include <sys/mman.h>              // mmap / MAP_JIT（JIT 冒烟测试）
#include <libkern/OSCacheControl.h> // sys_icache_invalidate（指令缓存刷新）
#include <errno.h>
#include <dlfcn.h>                 // dlsym 探测翻转符号（冒烟）
#include <unistd.h>                // getpid
// sys/codesign.h 在 iOS SDK 中被 Apple 剥离（交叉编译会 'file not found'），
// 故手动声明 csops 及其常量。csops 是 libSystem 真实导出的符号，运行/链接均可用。
extern int csops(int pid, unsigned int ops, void *useraddr, unsigned long usersize);
// mach_vm.h 在 iOS SDK 带 #error unsupported（Apple 剥离），但 mach_vm_write /
// mach_vm_region 是 libsystem_kernel 真实导出的符号（.ips 可见），手动声明。
// 用途：JIT 冒烟的调试态写入原语（debugserver 同款）与真实保护位校验。
extern kern_return_t mach_vm_write(vm_map_t target_task,
                                   mach_vm_address_t address,
                                   vm_offset_t data,
                                   mach_msg_type_number_t dataCnt);
extern kern_return_t mach_vm_region(vm_map_t target_task,
                                    mach_vm_address_t *address,
                                    mach_vm_size_t *size,
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
    char *p = getsectiondata((const struct mach_header_64 *)hdr, "__DATA", "__nebulabcl", &sz);
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
    const char *slash = strrchr(aname, '/');
    const char *s = slash ? slash + 1 : aname;
    char base[512];
    strncpy(base, s, sizeof(base) - 1); base[sizeof(base) - 1] = 0;
    size_t L = strlen(base);
    if (L > 4 && !strcasecmp(base + L - 4, ".dll")) base[L - 4] = 0;
    for (int i = 0; i < g_n; i++) {
        const char *bn = strrchr(g_entries[i].name, '/');
        const char *en = bn ? bn + 1 : g_entries[i].name;
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
    int  gc_soft_limit_mb;      // 软顶：超过即常规 GC（默认 320MB）
    int  gc_hard_limit_mb;      // 硬顶：超过即激进全量 GC + 告警（默认 500MB，见 #12）
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
    .gc_soft_limit_mb = 320,
    .gc_hard_limit_mb = 500,    // #12：RSS 超 500MB 强制激进 GC，防 jetsam 杀
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
    if (CFG.gc_nursery_mb > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "nursery-size=%dm", CFG.gc_nursery_mb);
        setenv("MONO_GC_PARAMS", buf, 1);
        printf("[host] MONO_GC_PARAMS=%s\n", buf);
    }
    // 单一编译器（JIT）版：不再有解释器分支，统一走 JIT（见下方 mono_setmmapjit）。
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

static void *watchdog_thread(void *arg) {
    (void)arg;
    while (watchdog_run) {
        long mb = rss_mb();
        if (mb > 0 && g_logfp) {
            // mono 未就绪前只记录 RSS（GC 尚未初始化，不能调 mono_gc_collect，否则崩）；
            // 就绪后依软/硬顶强制 GC，防 jetsam 杀。内存日志一律写文件（fflush）——
            // 旧版写 stderr 不进 nebula_host.log，导致 OOM 时彻底失明。
            if (g_mono_ready && CFG.gc_hard_limit_mb > 0 && mb > CFG.gc_hard_limit_mb) {
                fprintf(g_logfp, "[host][mem] !! RSS %ldMB 超过硬顶 %dMB，激进全量 GC！\n", mb, CFG.gc_hard_limit_mb);
                fflush(g_logfp);
                mono_gc_collect(mono_gc_max_generation());
                long after = rss_mb();
                fprintf(g_logfp, "[host][mem]     GC 后 RSS=%ldMB\n", after); fflush(g_logfp);
            } else if (g_mono_ready && CFG.gc_soft_limit_mb > 0 && mb > CFG.gc_soft_limit_mb) {
                fprintf(g_logfp, "[host][mem] RSS %ldMB 超过软顶 %dMB，强制 GC\n", mb, CFG.gc_soft_limit_mb);
                fflush(g_logfp);
                mono_gc_collect(mono_gc_max_generation());
            } else {
                fprintf(g_logfp, "[host][mem] RSS=%ldMB soft=%d hard=%d ready=%d\n",
                        mb, CFG.gc_soft_limit_mb, CFG.gc_hard_limit_mb, g_mono_ready);
                fflush(g_logfp);
            }
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

// 正式路径：把控制权交给 BepInEx（优先从 blob 内存加载，否则文件兜底）
static int boot_bepinex(MonoDomain *domain, const char *root) {
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
    MonoClass *klass = mono_class_from_name(img, "Doorstop", "Entrypoint");
    if (!klass) { fprintf(stderr, "[host] 找不到 Doorstop.Entrypoint\n"); return 0; }

    MonoMethod *start = mono_class_get_method_from_name(klass, "Start", 0);
    if (!start) { fprintf(stderr, "[host] 找不到 Doorstop.Entrypoint.Start()\n"); return 0; }

    printf("[host] 调用 Doorstop.Entrypoint.Start() —— 之后交给 BepInEx chainloader\n");
    MonoObject *exc = NULL;
    mono_runtime_invoke(start, NULL, NULL, &exc);
    if (exc) { fprintf(stderr, "[host] !! Start() 抛出异常\n"); return 0; }
    printf("[host] Start() 已返回，RSS=%ldMB\n", rss_mb());
    return 1;
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
    if (exc) { fprintf(stderr, "[host]   !! 抛异常\n"); return 0; }
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
static FILE     *g_logfp   = NULL;
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

/* 依次尝试若干可写位置建日志；游戏目录未必可写，不能只试一个 */
static int log_try(const char *p)
{
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
// 往往不生成 .ips（2026-10-25 真机实测多轮）。只能靠 nebula_host.log 定位死因。
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
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t bi;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz,
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
            hlog("[host] 冒烟: 路线A MAP_JIT mmap OK %p (cur=%x max=%x), 试 mach_vm_write ...",
                 p, cur, mx);
            kern_return_t kr = mach_vm_write(mach_task_self(),
                                             (mach_vm_address_t)(uintptr_t)p,
                                             (vm_offset_t)(uintptr_t)&ret_insn,
                                             (mach_msg_type_number_t)sizeof(ret_insn));
            if (kr != KERN_SUCCESS) {
                hlog("[host] 冒烟: 路线A mach_vm_write 失败 kr=%d", kr);
            } else if (*(volatile unsigned int *)p != ret_insn) {
                hlog("[host] 冒烟: 路线A mach_vm_write 报成功但读回不符 —— 假写入");
            } else {
                hlog("[host] 冒烟: 路线A mach_vm_write 写入 OK（读回一致）, 刷 icache, 试执行 ...");
                sys_icache_invalidate(p, 4096);
                if (sigsetjmp(g_smoke_jmp, 1) == 0) {
                    ((void (*)(void))p)();
                    hlog("[host] 冒烟: 路线A 执行 OK —— MAP_JIT+mach_vm_write 路线畅通!");
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
    hlog("[host] !! 冒烟失败: 四条 JIT 路线（mach_vm_write / mprotect+WRITE / 翻转函数 / RWX直配）均不可用");
    hlog("[host] !! 放弃 mono 初始化（保游戏可玩）。日志上方各路线的失败点即内核给出的精确边界。");
    return 0;
#else
    return 1;   // 桌面/macOS：JIT 天然可用，无需冒烟
#endif
}

static void *boot_thread(void *arg) {
    (void)arg;
    hlog("[host] boot thread started");
    // 全球化：默认 invariant 模式，避免 iOS 上缺 icudt.dat 崩溃（游戏模组走 UTF-8，不受影响）。
    // 若要完整文化格式化，放 icudt.dat 到 dylib 同目录并删此行。
    setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 1);

    {   // 尽早开日志（早到能覆盖后面每一步），并依次试多个可写位置
        char p[PATH_MAX];
        const char *home = getenv("HOME");
        if (home) { snprintf(p, sizeof(p), "%s/Documents/nebula_host.log", home); log_try(p); }
        const char *gd = getenv("NEBULA_GAME_DIR");
        if (gd)   { snprintf(p, sizeof(p), "%s/nebula_host.log", gd);             log_try(p); }
        log_try("/var/mobile/nebula_host.log");
        hlog("[host] === boot start ===");
    }

    start_watchdog();   // 尽早启动内存看门狗，覆盖 mono_jit_init 全程（含其内存高峰）

    char *dir = module_dir();
    hlog("[host] module dir = %s", dir);

    // 游戏根目录（BepInEx 所在处）。iOS 上由注入方指定，默认用 dylib 同目录。
    const char *root = getenv("NEBULA_GAME_DIR");
    if (!root || !*root) root = dir;
    hlog("[host] game root = %s", root);

    if (!g_logfp) {   // 前面候选都没成功，再试游戏根目录 / dylib 同目录
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/nebula_host.log", root); log_try(p);
        snprintf(p, sizeof(p), "%s/nebula_host.log", dir);  log_try(p);
    }
    if (!g_logfp) hlog("[host] !! 日志无法创建：所有候选路径都不可写");

    // 启动占位文本：Mono + BepInEx 初始化期间显示在屏幕正中，防黑屏误判死机。
    // 窗口可能还没起来，内部带重试；Nebula.dll + Harmony 就绪后由托管侧
    // nebula_signal_ready() 销毁，或此处兜底延时/安全超时销毁。
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
    MonoDomain *domain = mono_jit_init("NebulaHost");
    if (!domain) { hlog("[host] !! mono_jit_init failed"); free(dir); return NULL; }
    g_mono_ready = 1;   // 看门狗自此起可安全 GC
    hlog("[host] step: mono_jit_init OK, RSS=%ldMB", rss_mb());

    // 注册预加载钩子：程序集依赖优先从 blob 内存加载（BCL/BepInEx.core 已内嵌）
    if (g_n > 0) mono_install_assembly_preload_hook(nebula_preload, NULL);

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

    // 启动占位文本：BepInEx 已接管（或所有启动路径都失败）—— 把画面交还游戏。
    // 精确时机由托管侧 nebula_signal_ready() 提前触发（Nebula.dll + Harmony 就绪）；
    // 这里给一个兜底延时（NEBULA_BOOT_OVERLAY_HOLD_MS，默认 2000ms），让星云舰
    // 自己的加载动画先就位，再销毁文字；安全超时(默认30s)在 boot_overlay.m 内兜底。
    {
        int hold = 2000;
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
    mono_jit_cleanup(domain);
    free(dir);
    return NULL;
}

static void boot(void) {
    pthread_t t;
    pthread_create(&t, NULL, boot_thread, NULL);
    pthread_detach(t);
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
