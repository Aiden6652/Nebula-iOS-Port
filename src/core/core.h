/*
 * core.h — Nebula-iOS 纯本地玩法内核（不依赖网络）
 *
 * 架构边界（由用户确认）：
 *   - 模组逻辑【全部纯本地】：角色/技能/胜负每台设备独立计算，不碰网络。
 *   - 网络层只负责在【联机】时把"动作"广播给另一台设备，两端各自用
 *     同一套本地逻辑重算结果 → 状态自然收敛。
 *
 * 本文件定义游戏状态、角色、技能执行、胜负判定，以及"本地动作→广播"
 * 的回调钩子（由 dylib 接上网络层）。
 */

#ifndef NEBULA_CORE_H
#define NEBULA_CORE_H

#include <stdint.h>
#include <stddef.h>

/* 角色（最小演示集，后续按星云舰逐步扩充） */
typedef enum {
    ROLE_CREW     = 0,   /* 船员：无主动技能 */
    ROLE_IMPOSTOR = 1,   /* 内鬼：可击杀 */
    ROLE_SHERIFF  = 2,   /* 警长：可枪杀；误杀船员则自己死 */
} nebula_role_t;

#define NEBULA_MAX_PLAYERS 16
#define ABILITY_KILL       1   /* 与协议 RPC_ABILITY_USE 的 ability 字段对齐 */

typedef struct {
    uint32_t      net_id;
    nebula_role_t role;
    int           alive;        /* 1=存活 0=死亡 */
    uint8_t       kill_cd;      /* 击杀冷却（演示字段） */
} nebula_player_t;

typedef struct {
    nebula_player_t players[NEBULA_MAX_PLAYERS];
    int            n_players;
    int            over;        /* 1=本局结束 */
    int            winner;      /* 0=未定 1=船员胜 2=内鬼胜 */
} nebula_game_t;

/*
 * 本地动作回调：当一次"本地发起"的技能被本地执行后触发，
 * dylib 把它接到网络层去广播（让对端用同一逻辑重算）。
 * 参数与 RPC_ABILITY_USE 的 payload 完全一致。
 */
typedef void (*nebula_action_cb_t)(uint32_t caster, uint16_t ability,
                                   uint32_t target, float x, float y);
void nebula_core_set_action_cb(nebula_action_cb_t cb);

/* 初始化一局 */
void nebula_core_init(nebula_game_t *g);

/* 加入一名玩家（建房/同步角色分配时调用） */
void nebula_core_add_player(nebula_game_t *g, uint32_t net_id, nebula_role_t role);

/* 执行技能。
 *   local=1：本地发起（执行后触发 action 回调去广播）
 *   local=0：来自网络（只本地重算，不再广播，避免回环）
 * 返回 0 成功，-1 非法（如施法者已死/目标不存在）。 */
int nebula_core_use_ability(nebula_game_t *g,
                            uint32_t caster, uint16_t ability,
                            uint32_t target, float x, float y,
                            int local);

/* 重算胜负（每次动作后调用） */
void nebula_core_check_win(nebula_game_t *g);

/* 取某玩家指针（不存在返回 NULL） */
nebula_player_t *nebula_core_find(nebula_game_t *g, uint32_t net_id);

#endif /* NEBULA_CORE_H */
