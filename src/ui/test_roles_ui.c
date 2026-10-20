/*
 * test_roles_ui.c — 本地验证【职业面板】的数据层
 *
 * 面板本身是 ObjC（只能真机跑），但它读写的数据全在这层 C 代码里，
 * 所以"点 +/- 是否真的改了权重并落盘"可以在 Linux 上先验证。
 * 跑法：cd src/ui && make test
 */

#include "roles_ui.h"
#include "../core/roleweight.h"
#include "../core/roles.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static const char *D = "/tmp/nebula_ui_home";
static char RP[512], FP[512];

static void wr(const char *p, const char *c) {
    FILE *f = fopen(p, "w"); if (f) { fputs(c, f); fclose(f); }
}
static int file_has(const char *p, const char *needle) {
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

int main(void) {
    printf("=== Nebula 职业面板数据层 host test ===\n");
    snprintf(RP, sizeof(RP), "%s/nebula_roles.txt", D);
    snprintf(FP, sizeof(FP), "%s/nebula_freeplay.txt", D);

    /* ① 267 个职业都能在面板里列出来 */
    wr(RP, "sheriff x2\njackal\n");
    nebula_ui_init(D);
    int n = nebula_ui_role_count();
    printf("       职业总数 = %d\n", n);
    CHECK(n > 200, "面板列出全部职业（>200）");
    int named = 0;
    for (int i = 0; i < n; i++) if (nebula_ui_role_name(i) && nebula_ui_role_name(i)[0]) named++;
    CHECK(named == n, "每个职业都有中文名（面板不会显示空行）");

    /* ② 权重读出来了 */
    int sheriff = -1, jackal = -1;
    for (int i = 0; i < n; i++) {
        const char *nm = nebula_ui_role_name(i);
        if (nm && strcmp(nm, "警长") == 0) sheriff = i;
        if (nm && strcmp(nm, "豺狼") == 0) jackal = i;
    }
    CHECK(sheriff >= 0 && jackal >= 0, "找到警长与豺狼");
    CHECK(nebula_ui_role_weight(sheriff) == 2, "配置里的 x2 读成权重 2");
    CHECK(nebula_ui_role_weight(jackal) == 1, "没写数量的读成权重 1");

    /* ③ 面板点 +/- 改权重 */
    nebula_ui_role_bump(jackal, +1);
    CHECK(nebula_ui_role_weight(jackal) == 2, "点 + 权重变 2");
    nebula_ui_role_bump(jackal, -1);
    CHECK(nebula_ui_role_weight(jackal) == 1, "点 - 权重回到 1");
    for (int k = 0; k < 40; k++) nebula_ui_role_bump(jackal, +1);
    CHECK(nebula_ui_role_weight(jackal) == WEIGHT_MAX, "狂点 + 不会超过上限（防手滑点爆）");
    for (int k = 0; k < 40; k++) nebula_ui_role_bump(jackal, -1);
    CHECK(nebula_ui_role_weight(jackal) == 0, "狂点 - 不会变成负数");

    /* ④ 改完立即落盘（下次启动还在） */
    nebula_ui_role_set(sheriff, 3);
    CHECK(file_has(RP, "sheriff x3"), "权重改动立即写入配置");

    /* ⑤ 权重 → 上场序列：x3 应出现 3 次 */
    int ids[512];
    int m = nebula_weight_expand(ids, 512);
    int sheriff_cnt = 0;
    for (int i = 0; i < m; i++) {
        const char *nm = nebula_role_name(ids[i]);
        if (nm && strcmp(nm, "警长") == 0) sheriff_cnt++;
    }
    CHECK(sheriff_cnt == 3, "权重 3 → 上场序列里出现 3 次");

    /* ⑥ 一个都不配时不会开天窗（至少给个船员） */
    nebula_weight_clear();
    int m2 = nebula_weight_expand(ids, 512);
    CHECK(m2 >= 1, "全部清零也不会出现空局");

    /* ⑦ 自由模式开关（面板顶部那个开关） */
    wr(FP, "fill 8\nrotate 30\n");
    nebula_ui_init(D);
    CHECK(nebula_ui_freeplay_on() == 1, "配置文件开启 → 面板开关显示开");
    nebula_ui_set_freeplay(0);
    CHECK(nebula_ui_freeplay_on() == 0, "关一次 → 立即生效");
    CHECK(file_has(FP, "off"), "关闭状态被写回配置（下次启动还关着）");
    nebula_ui_set_freeplay(1);
    CHECK(nebula_ui_freeplay_on() == 1, "再开 → 立即生效");
    CHECK(!file_has(FP, "off"), "开启时配置里没有 off 行");

    /* ⑧ 换职业按钮 */
    nebula_ui_next_role();
    CHECK(1, "换职业按钮调用不崩");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
