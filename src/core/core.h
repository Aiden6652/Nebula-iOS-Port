/*
 * core.h — Nebula-iOS 纯本地玩法引擎（不依赖网络）
 *
 * 架构边界（用户确认）：模组逻辑【全部纯本地】，每台设备独立用相同规则重算；
 * 联机时网络层【只广播动作】，两端各自本地重算 → 状态自然收敛。
 *
 * 本文件是本地玩法的"大脑"：角色、技能、会议/投票、胜负、修饰（Auxiliary）。
 * 后续按星云舰逐步扩充角色与技能即可，框架不变。
 */

#ifndef NEBULA_CORE_H
#define NEBULA_CORE_H

#include <stdint.h>
#include <stddef.h>

/* ===================== 角色 ===================== */
typedef enum {
    ROLE_CREW      = 0,   /* 船员：无击杀，靠任务/投票 */
    ROLE_IMPOSTOR  = 1,   /* 内鬼：可击杀/破坏/通风管 */
    ROLE_SHERIFF   = 2,   /* 警长：可枪杀，误杀船员则自己死 */
    ROLE_ENGINEER  = 3,   /* 工程师：船员，可短暂用通风管 */
    ROLE_JESTER    = 4,   /* 小丑：被投票驱逐即胜（中立） */
    ROLE_SEER      = 5,   /* 占卜师：船员，可识破目标阵营 */
    ROLE_MADMATE   = 6,   /* 狂信者：不计入船员数，与内鬼同胜 */
    ROLE_BODYGUARD = 7,   /* 保镖：船员，可为目标加护盾（免疫一次致死） */
    ROLE_SNIPER    = 8,   /* 狙击手：内鬼阵营，可远程击杀且无反伤 */
    ROLE_AVENGER   = 9,   /* 易怒者：被攻击时反杀凶手 */
    ROLE_BUBBLEGUNNER = 10, /* 泡泡枪手：内鬼阵营，泡泡击杀隐藏尸体（不触发报告） */
    ROLE_JACKAL    = 11,  /* 豺狼：中立阵营，可击杀，豺狼方≥其余方即胜 */
    ROLE_SIDEKICK  = 12,  /* 跟班：被豺狼招募前算船员，招募后入豺狼阵营 */
} nebula_role_t;

/* 胜负方代码 */
#define WIN_NONE     0
#define WIN_CREW     1
#define WIN_IMPOSTOR 2
#define WIN_JACKAL   3
#define WIN_JESTER   4

/* ===================== 技能 / 动作 ID =====================
 * 与协议 RPC_ABILITY_USE 的 ability 字段对齐；一个 RPC 承载所有动作。 */
#define ABILITY_KILL     1   /* 击杀：caster 对 target */
#define ABILITY_VENT     2   /* 进出通风管：caster，坐标 (x,y) */
#define ABILITY_MEETING  3   /* 发起会议：caster */
#define ABILITY_VOTE     4   /* 投票：caster 投给 target */
#define ABILITY_TASK     5   /* 完成任务：caster，target=任务id */
#define ABILITY_SABOTAGE 6   /* 破坏：caster，target=破坏类型 */
#define ABILITY_REVEAL   7   /* 识破：caster 识破 target 阵营 */
#define ABILITY_SHIELD   8   /* 护盾：caster 给 target 加护盾 */
#define ABILITY_SNIPE    9   /* 狙杀：caster 远程击杀 target（无反伤） */
#define ABILITY_BUBBLE   10  /* 泡泡击杀：caster 远程击杀且隐藏尸体（不触发 BAIT 报告） */
#define ABILITY_RECRUIT  11  /* 招募：豺狼把 target 变为自己的跟班 */

/* ===================== 修饰（Auxiliary，位掩码） ===================== */
#define MOD_NONE     0x0000
#define MOD_SPEED    0x0001  /* 加速 */
#define MOD_BAIT     0x0002  /* 被击杀者死后触发会议 */
#define MOD_GIANT    0x0004  /* 体型变大（演示字段） */

/* ===================== 阶段 ===================== */
#define PHASE_PLAY   0
#define PHASE_MEET  1
#define PHASE_OVER  2

#define NEBULA_MAX_PLAYERS 16
#define NEBULA_MAX_MODS    8

typedef struct {
    uint32_t      net_id;
    nebula_role_t role;
    int           alive;
    uint8_t       kill_cd;     /* 击杀冷却 */
    uint8_t       vent_cd;     /* 通风管冷却 */
    int           in_vent;     /* 是否在通风管内 */
    int           voted;       /* 本次会议是否已投 */
    uint32_t      vote_target; /* 投给谁 */
    uint32_t      modifiers;   /* 生效中的修饰位掩码 */
    int           shielded;    /* 护盾：免疫一次致死 */
    int           revealed;    /* 阵营已被识破 */
    int           recruited;   /* 是否已被豺狼招募为跟班 */
} nebula_player_t;

typedef struct {
    nebula_player_t players[NEBULA_MAX_PLAYERS];
    int            n_players;
    int            phase;
    int            winner;       /* 0=未定 1=船员 2=内鬼 4=小丑 */
    int            meeting_by;   /* 发起会议者 net_id，-1 无 */
    uint32_t       mod_defs[NEBULA_MAX_MODS]; /* 修饰效果参数表（演示） */
} nebula_game_t;

/* 本地动作回调：本地发起的动作 → 编码广播（见 src/dylib/loader.c） */
typedef void (*nebula_action_cb_t)(uint32_t caster, uint16_t ability,
                                   uint32_t target, float x, float y);
void nebula_core_set_action_cb(nebula_action_cb_t cb);

void   nebula_core_init(nebula_game_t *g);
void   nebula_core_add_player(nebula_game_t *g, uint32_t net_id, nebula_role_t role);
nebula_player_t *nebula_core_find(nebula_game_t *g, uint32_t net_id);

/* 执行动作。local=1 本地发起（触发广播）；local=0 来自网络（只重算）。 */
int  nebula_core_use_ability(nebula_game_t *g, uint32_t caster, uint16_t ability,
                             uint32_t target, float x, float y, int local);

/* 会议 / 投票 */
void nebula_core_call_meeting(nebula_game_t *g, uint32_t caster, int local);
void nebula_core_cast_vote(nebula_game_t *g, uint32_t voter, uint32_t target, int local);
/* 结算会议：驱逐得票最高者；返回被驱逐者 net_id，-1 表示无人出局 */
int  nebula_core_resolve_meeting(nebula_game_t *g);

void nebula_core_check_win(nebula_game_t *g);

/* 每帧推进：冷却递减等随时间变化的状态。由 Update hook 每帧调用。 */
void nebula_core_tick(nebula_game_t *g);

/* 修饰 */
void nebula_core_add_modifier(nebula_game_t *g, uint32_t net_id, uint32_t mod);
int  nebula_core_has_modifier(nebula_player_t *p, uint32_t mod);

#endif /* NEBULA_CORE_H */
