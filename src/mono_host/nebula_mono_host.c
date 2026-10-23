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
//   2) 内存：全量 NOS 在 iOS 上很容易被 jetsam 杀，且分发包不能假定用户有
//      increased-memory-limit。所以有 RSS 软顶看门狗 + 默认走解释器。
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
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <os/log.h>
#include <signal.h>
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
    char runtime_mode[16];      // interp | jit | auto
    int  gc_nursery_mb;
    int  gc_soft_limit_mb;
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
    .runtime_mode = "interp",
    .gc_nursery_mb = 16,
    .gc_soft_limit_mb = 320,
    .gc_watchdog_interval_ms = 2000,
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

// 两套预设：standard（纯解释器）/ pro（强制 JIT）
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

    printf("[host] cfg: mode=%s nursery=%dMB soft_limit=%dMB | addons=%s 贴图≤%dpx 缓存%dMB roslyn=%d\n",
           CFG.runtime_mode, CFG.gc_nursery_mb, CFG.gc_soft_limit_mb,
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
    if (!strcmp(CFG.runtime_mode, "interp")) {
        setenv("MONO_ENV_OPTIONS", "--interp", 1);
        printf("[host] 运行模式=解释器(interp)，不需要可执行内存权限\n");
    } else if (!strcmp(CFG.runtime_mode, "jit")) {
        printf("[host] 运行模式=JIT（要求 dynamic-codesigning，否则会崩）\n");
    }
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

static void *watchdog_thread(void *arg) {
    (void)arg;
    while (watchdog_run) {
        long mb = rss_mb();
        if (mb > 0 && CFG.gc_soft_limit_mb > 0 && mb > CFG.gc_soft_limit_mb) {
            fprintf(stderr, "[host][mem] RSS %ldMB 超过软顶 %dMB，强制 GC\n", mb, CFG.gc_soft_limit_mb);
            mono_gc_collect(mono_gc_max_generation());
        }
        long ms = CFG.gc_watchdog_interval_ms > 0 ? CFG.gc_watchdog_interval_ms : 2000;
        usleep((useconds_t)(ms * 1000));
    }
    return NULL;
}

static void start_watchdog(void) {
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
    if (g_logfp) { fprintf(g_logfp, "!! signal %d @ %s\n", sig, g_last_step); fflush(g_logfp); }
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

    // 崩在这里 = mono 运行时自身初始化问题（sdb trampoline 那类）
    if (strcmp(CFG.runtime_mode, "jit") == 0) {
        // pro/JIT 模式：保留 JIT，且强制 JIT 内存走 MAP_JIT 分配。
        // iOS 严格 W^X：非 MAP_JIT 的可执行内存非法，必须 MAP_JIT + 后续
        // pthread_jit_write_protect_np 翻转（参考 OpenJDK os_bsd_aarch64.cpp）。
        mono_setmmapjit(1);
        hlog("[host] pro/JIT 模式：保留 JIT + 强制 MAP_JIT 分配");
    } else {
        // standard：强制 AOT-only，避开运行时生成 trampoline（.NET MAUI iOS 的运行方式）。
        mono_jit_set_aot_only(TRUE);
        hlog("[host] 强制 mono AOT-only（iOS 禁止运行时生成 trampoline）");
    }

    hlog("[host] step: mono_jit_init ...");
    MonoDomain *domain = mono_jit_init("NebulaHost");
    if (!domain) { hlog("[host] !! mono_jit_init failed"); free(dir); return NULL; }
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
