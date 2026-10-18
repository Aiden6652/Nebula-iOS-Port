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

    /* ===== 新角色：狙击手 ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 6, ROLE_SNIPER);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_use_ability(&g, 6, ABILITY_SNIPE, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,2)->alive==0, "狙杀：目标死亡且无反伤");
    CHECK(nebula_core_find(&g,6)->alive==1, "狙击手存活（无反伤）");

    /* ===== 新角色：保镖护盾 ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 7, ROLE_BODYGUARD);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_use_ability(&g, 7, ABILITY_SHIELD, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,2)->shielded==1, "保镖已给目标加护盾");
    nebula_core_use_ability(&g, 3, ABILITY_KILL, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,2)->alive==1, "护盾吸收第一次致死");
    CHECK(nebula_core_find(&g,2)->shielded==0, "护盾已消耗");

    /* ===== 新角色：占卜师识破 ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 5, ROLE_SEER);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_use_ability(&g, 5, ABILITY_REVEAL, 3, 0, 0, 1);
    CHECK(nebula_core_find(&g,3)->revealed==1, "占卜师识破目标阵营");

    /* ===== 新角色：狂信者不计入船员，与内鬼同胜 ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_add_player(&g, 6, ROLE_MADMATE);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_use_ability(&g, 3, ABILITY_KILL, 2, 0, 0, 1);  /* 内鬼杀掉唯一船员 */
    CHECK(g.phase==PHASE_OVER && g.winner==2, "狂信者不计入船员 → 内鬼胜（狂信者随胜）");

    /* ===== 新角色：易怒者（被攻击反杀凶手） ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_add_player(&g, 9, ROLE_AVENGER);
    nebula_core_use_ability(&g, 3, ABILITY_KILL, 9, 0, 0, 1);
    CHECK(nebula_core_find(&g,9)->alive==1, "易怒者未被杀死（安然无恙）");
    CHECK(nebula_core_find(&g,3)->alive==0, "攻击者被反杀");

    /* ===== 新角色：泡泡枪手（隐藏尸体，不触发 BAIT 报告） ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 10, ROLE_BUBBLEGUNNER);
    nebula_core_add_player(&g, 2, ROLE_CREW);       /* 带 BAIT 的受害者 */
    nebula_core_add_player(&g, 12, ROLE_CREW);      /* 其余船员，保证局未结束 */
    nebula_core_add_player(&g, 13, ROLE_CREW);
    nebula_core_add_modifier(&g, 2, MOD_BAIT);
    nebula_core_use_ability(&g, 10, ABILITY_BUBBLE, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,2)->alive==0, "泡泡击杀致死");
    CHECK(g.phase==PHASE_PLAY, "泡泡隐藏尸体 → 未触发 BAIT 报告（局仍进行）");

    /* ===== 新角色：豺狼 + 跟班 ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 11, ROLE_JACKAL);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_use_ability(&g, 11, ABILITY_RECRUIT, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,2)->role==ROLE_SIDEKICK
          && nebula_core_find(&g,2)->recruited==1, "豺狼招募成功 → 目标成为跟班");
    CHECK(nebula_core_find(&g,2)->alive==1, "招募不致死");
    CHECK(g.phase==PHASE_OVER && g.winner==WIN_JACKAL, "豺狼方压过其余方 → 豺狼胜");

    /* ===== 每帧 tick：冷却递减 ===== */
    nebula_core_init(&g);
    nebula_core_add_player(&g, 3, ROLE_IMPOSTOR);
    nebula_core_add_player(&g, 2, ROLE_CREW);
    nebula_core_add_player(&g, 4, ROLE_CREW);
    nebula_core_add_player(&g, 5, ROLE_CREW);
    nebula_core_use_ability(&g, 3, ABILITY_KILL, 2, 0, 0, 1);
    CHECK(nebula_core_find(&g,3)->kill_cd==30, "击杀后进入冷却 30");
    nebula_core_tick(&g);
    CHECK(nebula_core_find(&g,3)->kill_cd==29, "tick 一次冷却 -1");
    for (int i=0;i<29;i++) nebula_core_tick(&g);
    CHECK(nebula_core_find(&g,3)->kill_cd==0, "冷却递减到 0");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
