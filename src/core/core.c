/*
 * core.c — Nebula-iOS 纯本地玩法引擎实现
 * 规则为星云舰式可玩集，框架稳定，后续按原版逐步扩充。
 */

#include "core.h"
#include <string.h>

static nebula_action_cb_t g_action_cb = NULL;

void nebula_core_set_action_cb(nebula_action_cb_t cb) { g_action_cb = cb; }

void nebula_core_init(nebula_game_t *g)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->meeting_by = -1;
}

void nebula_core_add_player(nebula_game_t *g, uint32_t net_id, nebula_role_t role)
{
    if (!g || g->n_players >= NEBULA_MAX_PLAYERS) return;
    nebula_player_t *p = &g->players[g->n_players++];
    p->net_id = net_id;
    p->role   = role;
    p->alive  = 1;
    p->kill_cd = 0;
    p->vent_cd = 0;
    p->in_vent = 0;
    p->voted  = 0;
    p->vote_target = 0;
    p->modifiers = MOD_NONE;
    p->shielded = 0;
    p->revealed = 0;
    p->recruited = 0;
}

nebula_player_t *nebula_core_find(nebula_game_t *g, uint32_t net_id)
{
    if (!g) return NULL;
    for (int i = 0; i < g->n_players; i++)
        if (g->players[i].net_id == net_id) return &g->players[i];
    return NULL;
}

/* 广播帮助：本地动作 → 编码 ABILITY_USE → 回调 */
static void broadcast(uint32_t caster, uint16_t ability, uint32_t target, float x, float y)
{
    if (g_action_cb) g_action_cb(caster, ability, target, x, y);
}

/* 让 target 承受一次致死效果；有护盾则吸收并返回 0（未致死） */
static int apply_lethal(nebula_player_t *t)
{
    if (!t) return 0;
    if (t->shielded) { t->shielded = 0; return 0; }  /* 护盾吸收 */
    t->alive = 0;
    return 1;                                        /* 已致死 */
}

static void do_kill(nebula_game_t *g, uint32_t caster, uint32_t target)
{
    nebula_player_t *c = nebula_core_find(g, caster);
    nebula_player_t *t = nebula_core_find(g, target);
    if (!c || !t || !c->alive || !t->alive) return;

    /* 易怒者：被攻击时反杀凶手（目标本人安然无恙） */
    if (t->role == ROLE_AVENGER) {
        apply_lethal(c);
        return;
    }

    if (c->role == ROLE_IMPOSTOR
        || c->role == ROLE_JACKAL
        || (c->role == ROLE_SIDEKICK && c->recruited)) {
        apply_lethal(t);
    } else if (c->role == ROLE_SHERIFF) {
        if (t->role == ROLE_IMPOSTOR) apply_lethal(t);
        else c->alive = 0;                 /* 误杀船员，自己死 */
    } else {
        return;                            /* 船员/工程师/占卜师等无击杀 */
    }
    c->kill_cd = 30;

    /* 被击杀者带 BAIT 修饰 → 死后触发会议（演示） */
    if (t->modifiers & MOD_BAIT) nebula_core_call_meeting(g, target, 0);
}

int nebula_core_use_ability(nebula_game_t *g, uint32_t caster, uint16_t ability,
                            uint32_t target, float x, float y, int local)
{
    if (!g || g->phase == PHASE_OVER) return -1;

    switch (ability) {
    case ABILITY_KILL:
        do_kill(g, caster, target);
        break;
    case ABILITY_VENT: {
        nebula_player_t *c = nebula_core_find(g, caster);
        if (!c || !c->alive) return -1;
        /* 仅内鬼/工程师可用；工程师有额外冷却 */
        if (c->role == ROLE_IMPOSTOR || c->role == ROLE_ENGINEER) {
            c->in_vent = !c->in_vent;
            c->vent_cd = (c->role == ROLE_ENGINEER) ? 20 : 5;
        } else return -1;
        break;
    }
    case ABILITY_SABOTAGE:
        /* 仅内鬼；演示：仅标记，不单独改变胜负 */
        if (!(nebula_core_find(g, caster) &&
              nebula_core_find(g, caster)->role == ROLE_IMPOSTOR)) return -1;
        break;
    case ABILITY_SNIPE: {
        /* 狙击手：远程击杀，无反伤 */
        nebula_player_t *c = nebula_core_find(g, caster);
        nebula_player_t *t = nebula_core_find(g, target);
        if (!c || !t || c->role != ROLE_SNIPER || !c->alive || !t->alive) return -1;
        apply_lethal(t);
        c->kill_cd = 45;
        break;
    }
    case ABILITY_SHIELD: {
        /* 保镖：给目标加护盾 */
        nebula_player_t *c = nebula_core_find(g, caster);
        nebula_player_t *t = nebula_core_find(g, target);
        if (!c || !t || c->role != ROLE_BODYGUARD || !t->alive) return -1;
        t->shielded = 1;
        break;
    }
    case ABILITY_BUBBLE: {
        /* 泡泡枪手：远程击杀且隐藏尸体 → 不触发 BAIT 报告 */
        nebula_player_t *c = nebula_core_find(g, caster);
        nebula_player_t *t = nebula_core_find(g, target);
        if (!c || !t || c->role != ROLE_BUBBLEGUNNER || !c->alive || !t->alive) return -1;
        apply_lethal(t);        /* 与 do_kill 不同：刻意不调 call_meeting，故 BAIT 不报告 */
        c->kill_cd = 40;
        break;
    }
    case ABILITY_RECRUIT: {
        /* 豺狼招募：把目标变为跟班（同一阵营） */
        nebula_player_t *c = nebula_core_find(g, caster);
        nebula_player_t *t = nebula_core_find(g, target);
        if (!c || !t || c->role != ROLE_JACKAL || !t->alive) return -1;
        if (t->role == ROLE_JACKAL || (t->role == ROLE_SIDEKICK && t->recruited)) return -1;
        t->role = ROLE_SIDEKICK;
        t->recruited = 1;
        break;
    }
    case ABILITY_REVEAL: {
        /* 占卜师：识破目标阵营 */
        nebula_player_t *c = nebula_core_find(g, caster);
        nebula_player_t *t = nebula_core_find(g, target);
        if (!c || !t || c->role != ROLE_SEER) return -1;
        t->revealed = 1;
        break;
    }
    case ABILITY_TASK: {
        nebula_player_t *c = nebula_core_find(g, caster);
        if (!c || !c->alive) return -1;     /* 任务只由存活者完成 */
        break;
    }
    default:
        return -1;
    }

    if (local) broadcast(caster, ability, target, x, y);
    /* 会议期间（如 BAIT 触发）不立即结算胜负，待会议结束再判 */
    if (g->phase == PHASE_PLAY) nebula_core_check_win(g);
    return 0;
}

void nebula_core_call_meeting(nebula_game_t *g, uint32_t caster, int local)
{
    if (!g || g->phase == PHASE_OVER) return;
    if (!nebula_core_find(g, caster)) return;
    g->phase = PHASE_MEET;
    g->meeting_by = (int)caster;
    for (int i = 0; i < g->n_players; i++) {
        g->players[i].voted = 0;
        g->players[i].vote_target = 0;
    }
    if (local) broadcast(caster, ABILITY_MEETING, caster, 0, 0);
}

void nebula_core_cast_vote(nebula_game_t *g, uint32_t voter, uint32_t target, int local)
{
    if (!g || g->phase != PHASE_MEET) return;
    nebula_player_t *v = nebula_core_find(g, voter);
    if (!v || !v->alive || v->voted) return;
    v->voted = 1;
    v->vote_target = target;
    if (local) broadcast(voter, ABILITY_VOTE, target, 0, 0);
}

int nebula_core_resolve_meeting(nebula_game_t *g)
{
    if (!g || g->phase != PHASE_MEET) return -1;

    /* 统计票数，找最高 */
    int cnt[NEBULA_MAX_PLAYERS] = {0};
    for (int i = 0; i < g->n_players; i++) {
        if (g->players[i].voted)
            for (int j = 0; j < g->n_players; j++)
                if (g->players[j].net_id == g->players[i].vote_target)
                    { cnt[j]++; break; }
    }
    int maxc = 0, ej = -1, tie = 0;
    for (int j = 0; j < g->n_players; j++) {
        if (cnt[j] > maxc) { maxc = cnt[j]; ej = j; tie = 0; }
        else if (cnt[j] == maxc && maxc > 0) tie = 1;
    }

    int ejected = -1;
    if (maxc > 0 && !tie) {
        ejected = ej;
        g->players[ej].alive = 0;
        /* 小丑被驱逐 → 小丑胜 */
        if (g->players[ej].role == ROLE_JESTER) {
            g->phase = PHASE_OVER; g->winner = WIN_JESTER; return ejected;
        }
    }
    g->phase = PHASE_PLAY;
    g->meeting_by = -1;
    nebula_core_check_win(g);
    return ejected;
}

void nebula_core_check_win(nebula_game_t *g)
{
    if (!g || g->phase == PHASE_OVER) return;
    int alive_imp = 0, alive_other = 0, alive_jackal = 0;
    for (int i = 0; i < g->n_players; i++) {
        nebula_player_t *p = &g->players[i];
        if (!p->alive) continue;
        if (p->role == ROLE_IMPOSTOR || p->role == ROLE_SNIPER
            || p->role == ROLE_BUBBLEGUNNER) {
            alive_imp++;                                    /* 内鬼阵营 */
        } else if (p->role == ROLE_JACKAL
                   || (p->role == ROLE_SIDEKICK && p->recruited)) {
            alive_jackal++;                                 /* 豺狼阵营（中立） */
        } else if (p->role == ROLE_JESTER) {
            continue;                                       /* 中立方不计入好人 */
        } else if (p->role == ROLE_MADMATE) {
            continue;                                       /* 狂信者不计入船员，挂改阵营判定 */
        } else {
            alive_other++;                                  /* 好人阵营 */
        }
    }
    /* 豺狼方压过其余方 → 豺狼胜（优先判定，避免被 Crew 提前结算） */
    if (alive_jackal > 0 && alive_jackal >= (alive_imp + alive_other)) {
        g->phase = PHASE_OVER; g->winner = WIN_JACKAL; return;
    }
    if (alive_imp == 0) { g->phase = PHASE_OVER; g->winner = WIN_CREW; return; }
    if (alive_imp >= alive_other && alive_jackal == 0) {
        g->phase = PHASE_OVER; g->winner = WIN_IMPOSTOR; return;
    }
}

void nebula_core_add_modifier(nebula_game_t *g, uint32_t net_id, uint32_t mod)
{
    nebula_player_t *p = nebula_core_find(g, net_id);
    if (p) p->modifiers |= mod;
}

int nebula_core_has_modifier(nebula_player_t *p, uint32_t mod)
{
    return p && (p->modifiers & mod);
}
