/*
 * test_core.c — host 端本地内核测试（纯本地逻辑，不依赖网络）
 * 跑法：cd src/core && make test
 */

#include "core.h"
#include <stdio.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static int g_broadcasts = 0;
static void fake_cb(uint32_t c, uint16_t a, uint32_t t, float x, float y){
    (void)c;(void)a;(void)t;(void)x;(void)y; g_broadcasts++;
}

int main(void)
{
    printf("=== Nebula core (local logic) host test ===\n");
    nebula_core_set_action_cb(fake_cb);

    nebula_game_t g;
    nebula_core_init(&g);
    nebula_core_add_player(&g, 1, ROLE_SHERIFF);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);

    /* 警长枪内鬼(3) → 内鬼死，警长活；触发 1 次广播 */
    g_broadcasts = 0;
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 3, 0, 0, 1);
    CHECK(nebula_core_find(&g, 3)->alive == 0, "警长枪内鬼：内鬼死亡");
    CHECK(nebula_core_find(&g, 1)->alive == 1, "警长存活");
    CHECK(g_broadcasts == 1, "本地发起触发 1 次广播");

    /* 重开：警长误杀船员(2) → 警长自己死 */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 1, ROLE_SHERIFF);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g, 1)->alive == 0, "警长误杀船员：自己死亡");
    CHECK(nebula_core_find(&g, 2)->alive == 1, "被误杀的船员反而存活");

    /* 内鬼(3)杀船员(2) → 船员死 */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_use_ability(&g, 3, ABILITY_KILL, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g, 2)->alive == 0, "内鬼击杀船员：船员死亡");

    /* 胜负：内鬼全灭 → 船员胜（警长枪内鬼） */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 1, ROLE_SHERIFF);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 3, 0, 0, 1); /* 警长枪内鬼 */
    CHECK(g.over == 1 && g.winner == 1, "内鬼全灭 → 船员胜");

    /* 胜负：内鬼≥好人 → 内鬼胜（2内鬼 vs 2好人，内鬼杀1好人 → 2 vs 1） */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 1, ROLE_CREW);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_add_player(&g, 4, ROLE_IMPOSTOR);
    nebula_core_use_ability(&g, 3, ABILITY_KILL, 1, 0, 0, 1);
    CHECK(g.over == 1 && g.winner == 2, "内鬼≥好人 → 内鬼胜");

    /* 网络来的动作(local=0)不二次广播 */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 1, ROLE_SHERIFF);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    g_broadcasts = 0;
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 3, 0, 0, 0); /* 来自网络 */
    CHECK(g_broadcasts == 0, "网络动作不触发二次广播（防回环）");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
