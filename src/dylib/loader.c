/*
 * nebula_ios_loader.c
 * 自建 iOS 模组加载器（替代 BepInEx 的简易框架）
 *
 * 说明：
 *  - 本文件为【可编译空壳】，目标为 arm64-apple-ios。
 *  - 真实逻辑（IL2CPP 桥接、Hook 注册、玩法重写）在 dump 到方法地址后填充。
 *  - 日志写到 stderr；设备上可改用 syslog / 文件以便调试。
 */

#include <stdio.h>
#include <stdint.h>

/* ===== 简易日志 ===== */
static void port_log(const char *msg) {
    if (msg) {
        fputs("[Nebula-iOS] ", stderr);
        fputs(msg, stderr);
        fputc('\n', stderr);
    }
}

/* ===== IL2CPP 桥接上下文（占位，运行时由 Frida 解析地址填入） ===== */
typedef struct {
    void *domain;   /* Il2CppDomain */
    void *image;    /* Assembly-CSharp 镜像 */
} il2cpp_ctx_t;

static il2cpp_ctx_t g_ctx = {0};

/* ===== 初始化 IL2CPP 桥接（占位） ===== */
void nebula_port_init(void) {
    port_log("loader constructed");
    /* TODO: 用 frida-il2cpp-bridge 获取 Il2CppDomain / Assembly-CSharp 镜像并填入 g_ctx */
}

/* ===== 注册 native Hook（占位） ===== */
void nebula_port_hook_setup(void) {
    port_log("hook setup placeholder");
    /* TODO: 用 Frida Interceptor 在 dump 出的目标方法原生地址上打补丁
     *       例如 PlayerControl.FixedUpdate / GameManager.Update */
}

/* ===== dylib 入口：dyld 加载阶段自动执行 ===== */
__attribute__((constructor))
static void loader_entry(void) {
    nebula_port_init();
    nebula_port_hook_setup();
    port_log("dylib loaded");
}
