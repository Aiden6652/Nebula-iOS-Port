/*
 * test_bridge.c — host 端验证：无 Frida 环境下的优雅降级
 *
 * 在 Linux（没有 Among Us / libil2cpp）上跑，预期：
 *   - nebula_il2cpp_init() 返回 -1，并明确报告"关键 API 缺失"
 *   - 后续所有调用都应安全返回，绝不崩溃、不空指针解引用
 * 这正好验证：万一你的 IPA 剥离了 il2cpp 符号，dylib 不会崩，只是明确报错。
 *
 * 跑法：cd src/il2cpp && make test
 */

#include "bridge.h"
#include "hook.h"
#include <stdio.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static int g_tick_hits = 0;
static void fake_tick(void){ g_tick_hits++; }

int main(void) {
    printf("=== Nebula il2cpp bridge host test (no-frida graceful degrade) ===\n");

    int rc = nebula_il2cpp_init();
    printf("  init 返回 %d（Linux 上无 il2cpp，预期 -1）\n", rc);
    CHECK(rc == -1, "无 il2cpp 符号时 init 明确失败（-1）");
    CHECK(nebula_il2cpp_ready() == 0, "未就绪时 ready()=0");

    /* 未就绪时以下调用都必须安全返回，不得崩溃 */
    CHECK(nebula_il2cpp_find_class_by_hint("PlayerControl") == NULL, "类查找返回 NULL");
    CHECK(nebula_il2cpp_apply_speed(1.5f) == 0, "apply_speed 安全返回 0（未改动任何对象）");
    float f = 0; int i = 0;
    CHECK(nebula_il2cpp_get_float(NULL, "x", &f) == -1, "空对象读 float 返回 -1");
    CHECK(nebula_il2cpp_set_float(NULL, "x", 1.0f) == -1, "空对象写 float 返回 -1");
    CHECK(nebula_il2cpp_get_int(NULL, "x", &i) == -1, "空对象读 int 返回 -1");
    CHECK(nebula_il2cpp_set_int(NULL, "x", 1) == -1, "空对象写 int 返回 -1");
    CHECK(nebula_il2cpp_get_instances(NULL, NULL, 0) == -1, "取实例安全返回 -1");

    const char *hints[] = {"PlayerControl"};
    CHECK(nebula_il2cpp_dump_report(NULL, hints, 1) == -1, "dump 在未就绪时返回 -1");

    /* hook 层：无 il2cpp 时必须安全返回，不得崩溃 */
    CHECK(nebula_hook_count() == 0, "初始未安装任何 hook");
    nebula_hook_set_tick(fake_tick);
    CHECK(nebula_hook_orig("GameManager", "Update") == NULL, "未安装时查原入口返回 NULL");
    CHECK(nebula_hook_install("GameManager", "Update", NULL, NULL) == -1, "参数非法时返回 -1");
    CHECK(nebula_hook_install_game_update() == -1, "无 il2cpp 时安装 hook 返回 -1");
    CHECK(nebula_hook_count() == 0, "失败后未产生脏记录");
    CHECK(g_tick_hits == 0, "未 hook 成功时不误调 tick 回调");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
