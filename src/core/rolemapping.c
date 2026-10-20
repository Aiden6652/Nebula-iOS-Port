/*
 * rolemapping.c — 职业 → 原版 RoleTypes 的映射规则
 *
 * 参考 PC 端 BepInEx 里 RoleManager 的做法：底层角色决定"能不能杀、
 * 能不能跳管"，上层只是显示名与技能逻辑。
 */

#include "rolemapping.h"
#include "roles.h"
#include <string.h>

/* 带刀职业（船员/中立里能杀人的）—— 底层必须伪装成 Impostor
   否则游戏不会给击杀按钮 */
static const char *KNIFE_KEYS[] = {
    /* 船员阵营带刀 */
    "sheriff",          /* 警长 */
    "justice",          /* 大神官/审判系 */
    "justicePlus",      /* 审判长 */
    "niceGuesser",      /* 正义的赌怪 */
    "vigilante",        /* 义警 */
    "veteran",          /* 老兵 */
    "marksman", "marksmanship",
    /* 中立带刀 */
    "jackal",           /* 豺狼 */
    "sidekick",         /* 跟班 */
    "avenger",          /* 复仇者 */
    "arsonist",         /* 纵火犯（部分版本带刀） */
    "plague",           /* 瘟疫 */
    "tyrant",           /* 暴君 */
    "hitman",           /* 杀手 */
    "spectre",          /* 幽灵 */
    "demon", "werewolf",
    NULL
};

/* 有对应原版能力的职业 → 映射过去，能直接用原版机制 */
struct { const char *key; int rt; } SPECIAL[] = {
    {"morphing",      RT_SHAPESHIFTER},  /* 化形者 */
    {"shapeshifter",  RT_SHAPESHIFTER},
    {"engineer",      RT_ENGINEER},      /* 工程师（跳管） */
    {"scientist",    RT_SCIENTIST},      /* 科学家（ vitals） */
    {"guardianangel",RT_GUARDIAN_ANGEL}, /* 守护天使 */
    {"guardianAngel",RT_GUARDIAN_ANGEL},
    {"phantom",      RT_PHANTOM},        /* 幻影（隐身） */
    {"tracker",      RT_TRACKER},        /* 追踪者 */
    {"noisemaker",   RT_NOISEMAKER},     /* 噪音制造者 */
    {"detective",    RT_DETECTIVE},      /* 侦探 */
    {"judge",        RT_JUDGE},          /* 审判官 */
    {"spiritguide",  RT_SPIRITGUIDE},    /* 通灵师 */
    {"viper",        RT_VIPER},          /* 毒蛇 */
    {NULL, 0}
};

static int eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    /* 忽略大小写比较 */
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

/* 主职业 + 附加职业 → 能力【并集】后的底层 RoleTypes */
int nebula_role_underlying_multi(int main_id, const int *subs, int nsub)
{
    const char *mk = nebula_role_key(main_id);
    int team_imp = (nebula_role_team(main_id) == TEAM_IMPOSTOR);
    int base = nebula_role_underlying(main_id, mk, team_imp);

    int knife = nebula_role_has_knife(main_id, mk, team_imp);
    int best_special = -1;

    for (int i = 0; i < nsub; i++) {
        int id = subs[i];
        const char *k = nebula_role_key(id);
        int imp = (nebula_role_team(id) == TEAM_IMPOSTOR);
        if (nebula_role_has_knife(id, k, imp)) knife = 1;
        /* 附加职业对应的原版能力也能用上 */
        for (int j = 0; SPECIAL[j].key; j++) {
            if (k && eq(k, SPECIAL[j].key)) best_special = SPECIAL[j].rt;
        }
    }

    /* 带刀优先（决定了有没有击杀按钮） */
    if (knife) return RT_IMPOSTOR;
    if (best_special >= 0) return best_special;
    return base;
}

int nebula_role_has_knife(int role_id, const char *key, int team_is_impostor)
{
    (void)role_id;
    if (team_is_impostor) return 1;          /* 内鬼阵营一律带刀 */
    if (!key) return 0;
    for (int i = 0; KNIFE_KEYS[i]; i++)
        if (eq(key, KNIFE_KEYS[i])) return 1;
    return 0;
}

int nebula_role_underlying(int role_id, const char *key, int team_is_impostor)
{
    (void)role_id;
    /* ① 有对应原版能力的，直接映射过去（能用原版机制） */
    if (key) {
        for (int i = 0; SPECIAL[i].key; i++)
            if (eq(key, SPECIAL[i].key)) return SPECIAL[i].rt;
    }
    /* ② 带刀的（含整个内鬼阵营、豺狼、警长）→ Impostor */
    if (nebula_role_has_knife(role_id, key, team_is_impostor)) return RT_IMPOSTOR;
    /* ③ 其余功能型 → Crewmate */
    return RT_CREWMATE;
}
