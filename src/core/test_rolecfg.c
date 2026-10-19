/*
 * test_rolecfg.c — host 端验证职业阵容配置
 * 跑法：cd src/core && make test
 */

#include "rolecfg.h"
#include "roles.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static void wr(const char *p, const char *c) {
    FILE *f = fopen(p, "w"); if (f) { fputs(c, f); fclose(f); }
}

int main(void) {
    printf("=== Nebula rolecfg host test ===\n");
    int ids[ROLECFG_MAX], n = 0;

    /* 默认阵容 */
    n = nebula_rolecfg_default(ids, ROLECFG_MAX, &n);
    CHECK(n > 0, "默认阵容非空");
    CHECK(strcmp(nebula_role_name(ids[0]), "警长") == 0, "默认首个是警长");

    /* key 反查 */
    CHECK(nebula_role_id_by_key("sheriff") >= 0, "key 反查 sheriff");
    CHECK(strcmp(nebula_role_name(nebula_role_id_by_key("jackal")), "豺狼") == 0, "key 反查 jackal=豺狼");
    CHECK(nebula_role_id_by_key("no_such_role") == -1, "未知 key 返回 -1");
    CHECK(nebula_role_id_by_key(NULL) == -1, "NULL key 返回 -1");

    /* 配置文件：带数量与注释 */
    const char *p = "/tmp/nebula_roles_test.txt";
    wr(p, "# 本局阵容\nsheriff x2\njackal\nbogus_role\nsniper x3\n");
    n = nebula_rolecfg_load(p, ids, ROLECFG_MAX, &n);
    CHECK(n == 6, "数量展开正确（2+1+3=6，未知项被跳过）");
    CHECK(strcmp(nebula_role_name(ids[0]), "警长") == 0, "第1个=警长");
    CHECK(strcmp(nebula_role_name(ids[1]), "警长") == 0, "第2个=警长（x2 展开）");
    CHECK(strcmp(nebula_role_name(ids[2]), "豺狼") == 0, "第3个=豺狼");
    CHECK(strcmp(nebula_role_name(ids[3]), "狙击手") == 0, "第4个=狙击手（x3 展开）");

    /* 空文件 → 回退默认 */
    wr(p, "# 只有注释\n");
    n = nebula_rolecfg_load(p, ids, ROLECFG_MAX, &n);
    CHECK(n > 0, "空配置回退默认阵容");

    /* 文件不存在 → 回退默认 */
    n = nebula_rolecfg_load("/tmp/definitely_missing.txt", ids, ROLECFG_MAX, &n);
    CHECK(n > 0, "文件不存在回退默认阵容");

    /* 循环取值 */
    CHECK(nebula_rolecfg_pick(ids, n, n + 1) == ids[1], "pick 超出长度会回绕");
    CHECK(nebula_rolecfg_pick(NULL, 0, 0) == -1, "pick 空数组返回 -1");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
