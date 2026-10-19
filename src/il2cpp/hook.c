/*
 * hook.c — 指针替换型 hook 实现（无 Frida / 无 JIT / 不改代码段）
 * 详见 hook.h 头部的原理说明。
 */

#include "hook.h"
#include "bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define MAX_HOOKS 32

typedef struct {
    void             *method;   /* MethodInfo*        */
    nebula_method_fn_t orig;    /* 原入口（用于回调） */
    char              key[96];  /* "Class.Method"     */
} hook_rec_t;

static hook_rec_t g_hooks[MAX_HOOKS];
static int        g_nhooks = 0;
static void     (*g_tick_cb)(void) = NULL;

/* ---- 熔断开关 ----
 * 万一目标设备的调用点真的做了 PAC 认证（会 SIGILL），不必重新编译：
 * 在 IPA 里可执行文件旁边放一个 nebula_no_hook.txt，即停用所有 hook，
 * 自动退回"纯字段轮询"模式（功能降级但必定不崩）。 */
/* 通用开关：可执行文件旁是否存在指定文件 */
int nebula_flag_file(const char *filename) {
#if defined(__APPLE__)
    char path[1024]; uint32_t sz = sizeof(path);
    if (_NSGetExecutablePath(path, &sz) != 0) return 0;
    char *slash = strrchr(path, '/');
    if (!slash) return 0;
    strncpy(slash + 1, filename, sizeof(path) - (size_t)(slash + 1 - path) - 1);
    return (access(path, F_OK) == 0) ? 1 : 0;
#else
    (void)filename;
    return 0;   /* 非 Apple 平台：一律视为不存在 */
#endif
}

int nebula_hook_enabled(void) {
#if defined(__APPLE__)
    /* ★ 默认【不装】hook。
       职业显示靠轮询就够了，而 hook 一旦调用约定处理不当会破坏
       PlayerControl.FixedUpdate —— 表现为"交互失效/按键常亮"。
       确认需要每帧执行逻辑时，主动放 nebula_hook.txt 才启用。 */
    return nebula_flag_file("nebula_hook.txt") ? 1 : 0;
#else
    (void)0;
    return 0;   /* 非 Apple 平台：不装 */
#endif
}

static void hlog(const char *fmt, ...) {
    char buf[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    nebula_log("%s", buf);   /* 统一走 syslog + $HOME/Documents/nebula_log.txt */
}

static void key_of(char *out, size_t n, const char *cls, const char *m) {
    snprintf(out, n, "%s.%s", cls, m);
}

nebula_method_fn_t nebula_hook_orig(const char *class_hint, const char *method_name) {
    char key[96]; key_of(key, sizeof(key), class_hint, method_name);
    for (int i = 0; i < g_nhooks; i++)
        if (strncmp(g_hooks[i].key, key, sizeof(key)) == 0) return g_hooks[i].orig;
    return NULL;
}

int nebula_hook_install(const char *class_hint, const char *method_name,
                        nebula_method_fn_t replacement,
                        nebula_method_fn_t *out_orig)
{
    if (!class_hint || !method_name || !replacement) return -1;

    void *klass = nebula_il2cpp_find_class_by_hint(class_hint);
    if (!klass) { hlog("hook: 找不到类 %s", class_hint); return -1; }

    void *m = nebula_il2cpp_class_get_method(klass, method_name);
    if (!m) { hlog("hook: 类 %s 中找不到方法 %s", class_hint, method_name); return -1; }

    /* MethodInfo 的第 0 个成员即 methodPointer —— 位于可写数据区 */
    nebula_method_fn_t orig = *(nebula_method_fn_t *)m;
    if (!orig) { hlog("hook: %s.%s 原入口为空", class_hint, method_name); return -1; }

    /* ↓↓ 纯数据写：重定向该方法的执行流，未触碰任何代码字节，故不需要 JIT */
    *(nebula_method_fn_t *)m = replacement;

    if (g_nhooks < MAX_HOOKS) {
        hook_rec_t *r = &g_hooks[g_nhooks++];
        r->method = m;
        r->orig   = orig;
        key_of(r->key, sizeof(r->key), class_hint, method_name);
    }
    if (out_orig) *out_orig = orig;

    hlog("hook: 已安装 %s.%s（原入口已保存，可执行自定义逻辑后回调）", class_hint, method_name);
    return 0;
}

int nebula_hook_count(void) { return g_nhooks; }

void nebula_hook_set_tick(void (*tick_cb)(void)) { g_tick_cb = tick_cb; }

/* ---- GameManager.Update 的每帧 hook ---- */
static void *update_hook(void *instance, void *method) {
    static unsigned long frames = 0;
    frames++;
    if (frames % 120 == 0) hlog("Update hook 生效中：已 %lu 帧（逻辑每帧执行）", frames);

    if (g_tick_cb) g_tick_cb();                       /* ★ 每帧跑本地引擎逻辑 */

    nebula_method_fn_t orig = nebula_hook_orig("GameManager", "Update");
    return orig ? orig(instance, method) : NULL;      /* 继续调用原函数 */
}

/* 目标回退链：GameManager 在新版里没有 Update，依次尝试其它真实存在的入口 */
static const struct { const char *cls; const char *mth; } hook_targets[] = {
    { "PlayerControl", "FixedUpdate" },
    { "PlayerControl", "Update" },
    { "ShipStatus",    "Update" },
    { "GameManager",   "Update" },
};

int nebula_hook_install_game_update(void) {
    if (!nebula_hook_enabled()) {
        hlog("hook: 检测到关闭标记，已停用 hook，退回纯轮询模式（不崩但无帧级逻辑）");
        return -2;
    }
    int rc = -1;
    for (size_t i = 0; i < sizeof(hook_targets)/sizeof(hook_targets[0]); i++) {
        rc = nebula_hook_install(hook_targets[i].cls, hook_targets[i].mth, update_hook, NULL);
        if (rc == 0) { hlog("hook: 已挂到 %s.%s", hook_targets[i].cls, hook_targets[i].mth); break; }
    }
    if (rc == 0) hlog("hook: 每帧逻辑已接入游戏 Update（指针替换，无 JIT/无 Frida）");
    return rc;
}
