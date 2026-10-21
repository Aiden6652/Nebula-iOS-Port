// Nebula iOS Port — Mono 嵌入宿主
// 职责：在 AU 进程里初始化 Mono 运行时（JIT），加载 Engine.dll 并执行。
// 等价于之前 CoreCLR POC 的宿主，但运行时换成 Mono（BepInEx 跑在 Mono 上）。
//
// 关键：构造器里必须异步 boot（pthread），绝不在 constructor/DllMain 同步初始化
//       Mono —— 会与加载器锁死锁（CoreCLR POC 已踩过）。
#define _GNU_SOURCE
#include <stdlib.h>
#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/object.h>
#include <mono/metadata/mono-config.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

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
    if (!domain) { fprintf(stderr, "[host] mono_jit_init failed\n"); return NULL; }

    char engine[PATH_MAX];
    snprintf(engine, sizeof(engine), "%s/Engine.dll", dir);

    MonoAssembly *assembly = mono_domain_assembly_open(domain, engine);
    if (!assembly) {
        fprintf(stderr, "[host] open %s failed\n", engine);
        mono_jit_cleanup(domain);
        return NULL;
    }

    MonoImage *image = mono_assembly_get_image(assembly);
    MonoClass *klass = mono_class_from_name(image, "Engine", "Engine");
    if (!klass) { fprintf(stderr, "[host] class Engine.Engine not found\n"); mono_jit_cleanup(domain); return NULL; }

    // 走 Main()：它读 script.cs.txt，用 Roslyn 编译，再由 Mono JIT 执行。
    MonoMethod *main = mono_class_get_method_from_name(klass, "Main", 0);
    if (main) {
        mono_runtime_invoke(main, NULL, NULL, NULL);
    } else {
        fprintf(stderr, "[host] Engine.Main not found\n");
    }

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
