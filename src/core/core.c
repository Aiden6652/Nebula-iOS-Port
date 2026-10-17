/*
 * core.c — Nebula-iOS 纯本地玩法内核实现
 * 与 src/core/core.h 配套；规则为星云舰式最小可玩集，后续逐步对齐原版。
 */

#include "core.h"
#include <string.h>

static nebula_action_cb_t g_action_cb = NULL;

void nebula_core_set_action_cb(nebula_action_cb_t cb) { g_action_cb = cb; }

void nebula_core_init(nebula_game_t *g)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
}

void nebula_core_add_player(nebula_game_t *g, uint32_t net_id, nebula_role_t role)
{
    if (!g || g->n_players >= NEBULA_MAX_PLAYERS) return;
    nebula_player_t *p = &g->players[g->n_players++];
    p->net_id = net_id;
    p->role   = role;
    p->alive  = 1;
    p->kill_cd = 0;
}

nebula_player_t *nebula_core_find(nebula_game_t *g, uint32_t net_id)
{
    if (!g) return NULL;
    for (int i = 0; i < g->n_players; i++)
        if (g->players[i].net_id == net_id) return &g->players[i];
    return NULL;
}

static void do_kill(nebula_game_t *g, uint32_t caster, uint32_t target,
                    float x, float y, int local)
{
    nebula_player_t *c = nebula_core_find(g, caster);
    nebula_player_t *t = nebula_core_find(g, target);
    if (!c || !t || !c->alive || !t->alive) return;   /* 死者不能行动/被再次击杀 */

    if (c->role == ROLE_IMPOSTOR) {
        t->alive = 0;                                  /* 内鬼击杀：目标死 */
    } else if (c->role == ROLE_SHERIFF) {
        if (t->role == ROLE_IMPOSTOR) {
            t->alive = 0;                              /* 警长枪内鬼：内鬼死 */
        } else {
            c->alive = 0;                              /* 警长误杀船员：自己死 */
        }
    } else {
        return;                                        /* 船员无击杀能力，忽略 */
    }
    c->kill_cd = 30;                                   /* 进入冷却（演示） */

    /* 本地发起 → 广播动作，让对端用同一逻辑重算 */
    if (local && g_action_cb) g_action_cb(caster, ABILITY_KILL, target, x, y);
}

int nebula_core_use_ability(nebula_game_t *g,
                            uint32_t caster, uint16_t ability,
                            uint32_t target, float x, float y,
                            int local)
{
    if (!g || g->over) return -1;
    if (ability == ABILITY_KILL) {
        do_kill(g, caster, target, x, y, local);
        nebula_core_check_win(g);
        return 0;
    }
    return -1;   /* 未实现的技能 */
}

void nebula_core_check_win(nebula_game_t *g)
{
    if (!g || g->over) return;
    int alive_imp = 0, alive_other = 0;
    for (int i = 0; i < g->n_players; i++) {
        if (!g->players[i].alive) continue;
        if (g->players[i].role == ROLE_IMPOSTOR) alive_imp++;
        else alive_other++;
    }
    if (alive_imp == 0) { g->over = 1; g->winner = 1; }       /* 内鬼全灭→船员胜 */
    else if (alive_imp >= alive_other) { g->over = 1; g->winner = 2; } /* 内鬼≥好人→内鬼胜 */
}
