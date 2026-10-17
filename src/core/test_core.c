/*
 * test_core.c — host 端本地玩法引擎测试（纯本地逻辑，不依赖网络）
 * 跑法：cd src/core && make test
 */

#include "core.h"
#include <stdio.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static int g_bc = 0;
static void fake_cb(uint32_t c, uint16_t a, uint32_t t, float x, float y){
    (void)c;(void)a;(void)t;(void)x;(void)y; g_bc++;
}

static void setup5(nebula_game_t *g){
    nebula_core_init(g);
    nebula_core_add_player(g, 1, ROLE_SHERIFF);
    nebula_core_add_player(g, 2, ROLE_CREW);
    nebula_core_add_player(g, 3, ROLE_IMPOSTOR);
    nebula_core_add_player(g, 4, ROLE_ENGINEER);
    nebula_core_add_player(g, 5, ROLE_JESTER);
}

int main(void)
{
    printf("=== Nebula core engine host test ===\n");
    nebula_core_set_action_cb(fake_cb);
    nebula_game_t g;

    /* 警长枪内鬼 → 内鬼死，警长活；触发广播 */
    setup5(&g); g_bc = 0;
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 3, 0, 0, 1);
    CHECK(nebula_core_find(&g,3)->alive==0, "警长枪内鬼：内鬼死亡");
    CHECK(nebula_core_find(&g,1)->alive==1, "警长存活");
    CHECK(g_bc==1, "本地击杀触发 1 次广播");

    /* 警长误杀船员 → 自己死 */
    setup5(&g);
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,1)->alive==0, "警长误杀船员：自己死亡");
    CHECK(nebula_core_find(&g,2)->alive==1, "被误杀船员反存活");

    /* 内鬼击杀船员 → 船员死 */
    setup5(&g);
    nebula_core_use_ability(&g, 3, ABILITY_KILL, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,2)->alive==0, "内鬼击杀船员：船员死亡");

    /* 工程师可进通风管；船员不可 */
    setup5(&g);
    nebula_core_use_ability(&g, 4, ABILITY_VENT, 0, 1.0f, 2.0f, 1);
    CHECK(nebula_core_find(&g,4)->in_vent==1, "工程师可进通风管");
    nebula_core_use_ability(&g, 2, ABILITY_VENT, 0, 0, 0, 1);
    CHECK(nebula_core_find(&g,2)->in_vent==0, "船员不可进通风管");

    /* 会议 + 投票 + 驱逐内鬼 → 船员胜 */
    setup5(&g);
    nebula_core_call_meeting(&g, 2, 1);              /* 船员发起会议 */
    CHECK(g.phase==PHASE_MEET, "会议阶段已开启");
    nebula_core_cast_vote(&g, 1, 3, 1);              /* 投内鬼 */
    nebula_core_cast_vote(&g, 2, 3, 1);
    nebula_core_cast_vote(&g, 4, 3, 1);
    int ej = nebula_core_resolve_meeting(&g);
    CHECK(ej>=0 && nebula_core_find(&g,3)->alive==0, "内鬼被驱逐");
    CHECK(g.phase==PHASE_OVER && g.winner==1, "内鬼全灭 → 船员胜（局终）");

    /* 驱逐小丑 → 小丑胜（中立） */
    setup5(&g);
    nebula_core_call_meeting(&g, 2, 1);
    nebula_core_cast_vote(&g, 1, 5, 1);
    nebula_core_cast_vote(&g, 2, 5, 1);
    nebula_core_cast_vote(&g, 3, 5, 1);
    ej = nebula_core_resolve_meeting(&g);
    CHECK(ej>=0 && g.winner==ROLE_JESTER, "小丑被驱逐 → 小丑胜");

    /* 平票 → 无人出局 */
    setup5(&g);
    nebula_core_call_meeting(&g, 2, 1);
    nebula_core_cast_vote(&g, 1, 3, 1);
    nebula_core_cast_vote(&g, 2, 4, 1);   /* 1 vs 1 平票 */
    ej = nebula_core_resolve_meeting(&g);
    CHECK(ej==-1, "平票无人出局");
    CHECK(nebula_core_find(&g,3)->alive==1 && nebula_core_find(&g,4)->alive==1, "平票双方均在");

    /* 修饰：BAIT 被击杀触发会议 */
    setup5(&g);
    nebula_core_add_modifier(&g, 3, MOD_BAIT);
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 3, 0, 0, 1);
    CHECK(g.phase==PHASE_MEET, "BAIT 修饰：被击杀触发会议");

    /* 网络来的动作(local=0)不二次广播 */
    setup5(&g); g_bc = 0;
    nebula_core_use_ability(&g, 1, ABILITY_KILL, 3, 0, 0, 0);
    CHECK(g_bc==0, "网络动作不二次广播（防回环）");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
