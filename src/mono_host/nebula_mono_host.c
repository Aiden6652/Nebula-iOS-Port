// Nebula iOS Port — Mono 嵌入宿主
// 职责：在 AU 进程里初始化 Mono 运行时，加载 Engine.dll 并执行。
//
// 关键：构造器里必须异步 boot（pthread），绝不在 constructor/DllMain 同步初始化
//       Mono —— 会与加载器锁死锁（CoreCLR POC 已踩过）。
//
// 内存：跑全量 NOS（星云舰 + 28 个 Addons）在 iOS 上很容易被 jetsam 杀掉，
//       而分发包不能假定用户有 increased-memory-limit 权限。所以这里做了三件事：
//       1) 宿主配置 nebula_host.cfg（nursery 大小 / RSS 软顶 / 运行模式）
//       2) 默认走解释器而不是 JIT（解释器不需要可执行内存权限）
//       3) RSS 看门狗线程，超软顶就强制 GC
//       详见 docs/ios-memory-budget.md
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>

#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/object.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/mono-gc.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

// ---------------------------------------------------------------- 配置

typedef struct {
    char runtime_mode[16];      // interp | jit | auto
    int  gc_nursery_mb;
    int  gc_soft_limit_mb;
    int  gc_watchdog_interval_ms;
    char addons_mode[16];       // all（默认，职业在 addon 里，必须全加载）
    int  addons_texture_max_px; // 贴图降分辨率上限（长边像素）
    int  addons_texture_cache_mb; // 贴图 LRU 缓存上限
    int  addons_audio_lazy;     // 音频按需加载
    int  roslyn_enabled;        // 运行时是否加载 Roslyn 编译 addon 脚本
    int  render_target_fps;     // 目标帧率；0 = 不限制
    int  render_adaptive;       // 掉帧时自动降画质（降贴图分辨率/LRU）
} host_cfg;

// 内置默认值 = Standard 档（纯解释器 + 小 nursery + 320MB 软顶）。
// 配置文件的 standard / pro 两套预设会覆盖它。
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

// 两套预设：standard（纯解释器）/ pro（强制 JIT）。
// 用 NEBULA_PROFILE 选；NEBULA_HOST_CFG 可直接指定文件路径，优先级最高。
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
        if (!probe) {
            // 预设文件不在就退回通用名
            snprintf(path, sizeof(path), "%s/nebula_host.cfg", dir);
        } else {
            fclose(probe);
        }
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[host] 未找到 cfg(profile=%s, 试过 %s)，用内置默认值(standard)\n", profile, path);
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
        else if (!strcmp(k, "gc.watchdog_interval_ms"))  CFG.gc_watchdog_interval_ms = atoi(v);
        else if (!strcmp(k, "addons.mode"))              snprintf(CFG.addons_mode, sizeof(CFG.addons_mode), "%s", v);
        else if (!strcmp(k, "addons.texture_max_px"))    CFG.addons_texture_max_px = atoi(v);
        else if (!strcmp(k, "addons.texture_cache_mb"))   CFG.addons_texture_cache_mb = atoi(v);
        else if (!strcmp(k, "addons.audio_lazy"))        CFG.addons_audio_lazy = atoi(v);
        else if (!strcmp(k, "roslyn.enabled"))           CFG.roslyn_enabled = atoi(v);
        else if (!strcmp(k, "render.target_fps"))        CFG.render_target_fps = atoi(v);
        else if (!strcmp(k, "render.adaptive"))          CFG.render_adaptive = atoi(v);
    }
    fclose(f);

    // 透传给托管侧（Engine/BepInEx 那边直接读环境变量，省得再解析一遍文件）
    char buf[32];
    setenv("NEBULA_ADDONS_MODE", CFG.addons_mode, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.addons_texture_max_px);   setenv("NEBULA_TEXTURE_MAX_PX", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.addons_texture_cache_mb); setenv("NEBULA_TEXTURE_CACHE_MB", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.roslyn_enabled);          setenv("NEBULA_ROSLYN", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.render_target_fps);       setenv("NEBULA_TARGET_FPS", buf, 1);
    snprintf(buf, sizeof(buf), "%d", CFG.render_adaptive);         setenv("NEBULA_ADAPTIVE", buf, 1);

    printf("[host] cfg: mode=%s nursery=%dMB soft_limit=%dMB interval=%dms | addons=%s 贴图≤%dpx 贴图缓存%dMB audio_lazy=%d roslyn=%d\n",
           CFG.runtime_mode, CFG.gc_nursery_mb, CFG.gc_soft_limit_mb, CFG.gc_watchdog_interval_ms,
           CFG.addons_mode, CFG.addons_texture_max_px, CFG.addons_texture_cache_mb,
           CFG.addons_audio_lazy, CFG.roslyn_enabled);
    if (CFG.render_target_fps > 0)
        printf("[host] cfg: 目标帧率=%d fps, adaptive=%d\n", CFG.render_target_fps, CFG.render_adaptive);
    else
        printf("[host] cfg: 目标帧率=不限制, adaptive=%d\n", CFG.render_adaptive);
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
        printf("[host] 运行模式=JIT（要求进程带 dynamic-codesigning，否则会崩）\n");
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
    long ps = sysconf(_SC_PAGESIZE);
    return (long)(resident * ps / (1024 * 1024));
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

// 取当前模块（.so / 可执行）自身所在目录，保证 dylib 注入游戏后也能定位自己的文件。
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

static void *boot_thread(void *arg) {
    (void)arg;
    char *dir = module_dir();
    printf("[host] module dir = %s\n", dir);

    // 读配置 + 在 mono_jit_init 之前把 GC/运行模式环境变量设好
    load_cfg(dir);
    apply_gc_env();

    // 仅当显式设置了 MONO_PREFIX（iOS 自定义 Mono 根）时才覆盖目录；
    // 桌面测试留空，让 Mono 用默认安装路径找 mscorlib 等框架程序集。
    const char *prefix = getenv("MONO_PREFIX");
    if (prefix) {
        char cfg[PATH_MAX];
        snprintf(cfg, sizeof(cfg), "%s/etc", prefix);
        mono_set_dirs(prefix, cfg);
    }
    mono_config_parse(NULL);

    MonoDomain *domain = mono_jit_init("NebulaHost");
    if (!domain) { fprintf(stderr, "[host] mono_jit_init failed\n"); free(dir); return NULL; }

    long base_rss = rss_mb();
    printf("[host] Mono 就绪，基线 RSS = %ldMB\n", base_rss);

    start_watchdog();

    char engine[PATH_MAX];
    snprintf(engine, sizeof(engine), "%s/Engine.dll", dir);

    MonoAssembly *assembly = mono_domain_assembly_open(domain, engine);
    if (!assembly) {
        fprintf(stderr, "[host] open %s failed\n", engine);
        watchdog_run = 0;
        mono_jit_cleanup(domain);
        free(dir);
        return NULL;
    }

    MonoImage *image = mono_assembly_get_image(assembly);
    MonoClass *klass = mono_class_from_name(image, "Engine", "Engine");
    if (!klass) {
        fprintf(stderr, "[host] class Engine.Engine not found\n");
        watchdog_run = 0;
        mono_jit_cleanup(domain);
        free(dir);
        return NULL;
    }

    // 走 Main()：它读 script.cs.txt，用 Roslyn 编译，再由 Mono 执行。
    MonoMethod *main = mono_class_get_method_from_name(klass, "Main", 0);
    if (main) {
        mono_runtime_invoke(main, NULL, NULL, NULL);
        printf("[host] Engine 结束，RSS = %ldMB\n", rss_mb());
    } else {
        fprintf(stderr, "[host] Engine.Main not found\n");
    }

    watchdog_run = 0;
    mono_jit_cleanup(domain);
    free(dir);
    return NULL;
}

// 异步启动：构造器立即返回，后台线程初始化 Mono（避免加载器锁死锁）。
static void boot(void) {
    pthread_t t;
    pthread_create(&t, NULL, boot_thread, NULL);
    pthread_detach(t);
}

#ifdef BUILD_AS_LIB
__attribute__((constructor))
static void on_load(void) { boot(); }
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    boot();
    sleep(2);   // 桌面测试时等后台线程跑完
    return 0;
}
