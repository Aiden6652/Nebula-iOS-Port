/* roles.c — 官方职业表（自动生成） */
#include "roles.h"
#include <string.h>

typedef struct { int id; const char *key; const char *name; nebula_team_t team; const char *color; } role_row_t;

static const role_row_t ROLES[] = {
    { ROLE_AGENT,                    "agent", "代理人", TEAM_CREW, "#FFFFFF" },
    { ROLE_ALIEN,                    "alien", "外星人", TEAM_NEUTRAL, "#98FB98" },
    { ROLE_ARSONIST,                 "arsonist", "纵火犯", TEAM_NEUTRAL, "#FF4500" },
    { ROLE_AVENGER,                  "avenger", "复仇者", TEAM_NEUTRAL, "#808080" },
    { ROLE_BAIT,                     "bait", "诱饵", TEAM_CREW, "#FF69B4" },
    { ROLE_BERSERKER,                "berserker", "易怒者", TEAM_CREW, "#FFFFFF" },
    { ROLE_BLOODY,                   "bloody", "血语者", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_BOUNTY_HUNTER,            "bountyHunter", "赏金猎人", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_BUBBLEGUN,                "bubblegun", "泡泡枪手", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_BUSKER,                   "busker", "灵化使", TEAM_CREW, "#FFB6C1" },
    { ROLE_CAMOUFLAGER,              "camouflager", "隐蔽者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_CANNON,                   "cannon", "空气炮手", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_CHAIN_SHIFTER,            "chainShifter", "交换师", TEAM_NEUTRAL, "#7FFFD4" },
    { ROLE_CLEANER,                  "cleaner", "清理者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_CLOG,                     "clog", "阻碍者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_COLLATOR,                 "collator", "基因学家", TEAM_CREW, "#FFFFFF" },
    { ROLE_COMET,                    "comet", "彗星之子", TEAM_NEUTRAL, "#87CEEB" },
    { ROLE_CONFUSED,                 "confused", "迷茫者", TEAM_NEUTRAL, "#D3D3D3" },
    { ROLE_CREWMATE,                 "crewmate", "船员", TEAM_CREW, "#FFFFFF" },
    { ROLE_DAMNED,                   "damned", "受诅咒者", TEAM_CREW, "#FFFFFF" },
    { ROLE_DAMNED_IMPOSTOR,          "damnedImpostor", "受诅咒者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_DANCER,                   "dancer", "舞蹈家", TEAM_CREW, "#FFFFFF" },
    { ROLE_DESTROYER,                "destroyer", "毁灭者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_DISCLOSURE,               "disclosure", "显眼包", TEAM_CREW, "#FFFFFF" },
    { ROLE_DISTURBER,                "disturber", "干扰者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_DOCTOR,                   "doctor", "医学家", TEAM_CREW, "#00FF7F" },
    { ROLE_ECHO,                     "echo", "感知者", TEAM_CREW, "#DDA0DD" },
    { ROLE_EFFACER,                  "effacer", "擦除者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_EMBER,                    "ember", "余烬", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_EVIL_GUESSER,             "evilGuesser", "邪恶的赌怪", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_EVIL_TRACKER,             "evilTracker", "邪恶的追踪者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_EVIL_TRAPPER,             "evilTrapper", "邪恶的陷阱师", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_EXTRA_MISSION,            "extraMission", "额外任务", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_GRUDGE,                   "grudge", "怨灵", TEAM_NEUTRAL, "#696969" },
    { ROLE_GUESSER,                  "guesser", "赌怪", TEAM_CREW, "#FFFFFF" },
    { ROLE_HADAR,                    "hadar", "矿工", TEAM_CREW, "#C0C0C0" },
    { ROLE_HALLUCINATION,            "hallucination", "幻形", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_ILLUSIONER,               "illusioner", "模仿大师", TEAM_CREW, "#FFFFFF" },
    { ROLE_IMPOSTOR,                 "impostor", "伪装者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_JACKAL,                   "jackal", "豺狼", TEAM_NEUTRAL, "#1D95F9" },
    { ROLE_JAILER,                   "jailer", "狱卒", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_JESTER,                   "jester", "小丑", TEAM_NEUTRAL, "#B0B0B0" },
    { ROLE_JUSTICE,                  "justice", "大神官", TEAM_CREW, "#FFF8DC" },
    { ROLE_LOVER,                    "lover", "恋人", TEAM_NEUTRAL, "#FFC0CB" },
    { ROLE_LUMINE,                   "lumine", "引路魂", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_MADMATE,                  "madmate", "叛徒", TEAM_CREW, "#FFFFFF" },
    { ROLE_MARIONETTE,               "marionette", "木偶师", TEAM_CREW, "#FFFFFF" },
    { ROLE_MAYOR,                    "mayor", "市长", TEAM_CREW, "#FFD700" },
    { ROLE_META_ROLE,                "metaRole", "自由模式附加能力", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_MORPHING,                 "morphing", "化形者", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_NECROMANCER,              "necromancer", "死灵法师", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_NICE_GUESSER,             "niceGuesser", "正义的赌怪", TEAM_CREW, "#FFFF99" },
    { ROLE_NICE_TRAPPER,             "niceTrapper", "正义的陷阱师", TEAM_CREW, "#99FF99" },
    { ROLE_NOISEGHOST,               "noiseghost", "吵闹鬼", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_NOISEMAKER,               "noisemaker", "大嗓门", TEAM_CREW, "#FFFFFF" },
    { ROLE_OBSESSIONAL,              "obsessional", "暗恋者", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_PAINTER,                  "painter", "画皮师", TEAM_CREW, "#FFFFFF" },
    { ROLE_PAPARAZZO,                "paparazzo", "摄影师", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_PHOSPHORUS,               "phosphorus", "提灯人", TEAM_CREW, "#FFFFFF" },
    { ROLE_POLTERGEIST,              "poltergeist", "捣蛋鬼", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_PROVOCATEUR,              "provocateur", "挑衅者", TEAM_CREW, "#FF8C00" },
    { ROLE_PSYCHIC,                  "psychic", "灵媒", TEAM_CREW, "#E6E6FA" },
    { ROLE_RAIDER,                   "raider", "强盗", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_REAPER,                   "reaper", "送葬者", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_SCARLET,                  "scarlet", "魅魔", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_SCARLET_LOVER,            "scarletLover", "魅魔的恋人", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_SECRET,                   "secret", "觉醒者", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_SECRET_IN_GAME,           "secretInGame", "觉醒者", TEAM_NEUTRAL, "#C0C0C0" },
    { ROLE_SEER,                     "seer", "先知", TEAM_CREW, "#B48EFF" },
    { ROLE_SHERIFF,                  "sheriff", "警长", TEAM_CREW, "#FFA500" },
    { ROLE_SIDEKICK,                 "sidekick", "跟班", TEAM_NEUTRAL, "#1D95F9" },
    { ROLE_SNIPER,                   "sniper", "狙击手", TEAM_IMPOSTOR, "#FF1919" },
    { ROLE_STIRRER,                  "stirrer", "幻术师", TEAM_CREW, "#FFFFFF" },
    { ROLE_THURIFER,                 "thurifer", "调香师", TEAM_CREW, "#FFE4B5" },
    { ROLE_TIE_BREAKER,              "tieBreaker", "破平者", TEAM_CREW, "#FFFFFF" },
    { ROLE_UBIQUITOUS,               "ubiquitous", "飞手", TEAM_CREW, "#FFFFFF" },
    { ROLE_VULTURE,                  "vulture", "秃鹫", TEAM_NEUTRAL, "#8B5A2B" },
};

static const int ROLE_N = (int)(sizeof(ROLES)/sizeof(ROLES[0]));

const char *nebula_role_name(int role) {
    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].name;
    return "未知职业";
}

nebula_team_t nebula_role_team(int role) {
    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].team;
    return TEAM_CREW;
}

const char *nebula_role_key(int role) {
    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].key;
    return "";
}

static const char *ENUM_NAMES[] = {
    "ROLE_AGENT",
    "ROLE_ALIEN",
    "ROLE_ARSONIST",
    "ROLE_AVENGER",
    "ROLE_BAIT",
    "ROLE_BERSERKER",
    "ROLE_BLOODY",
    "ROLE_BOUNTY_HUNTER",
    "ROLE_BUBBLEGUN",
    "ROLE_BUSKER",
    "ROLE_CAMOUFLAGER",
    "ROLE_CANNON",
    "ROLE_CHAIN_SHIFTER",
    "ROLE_CLEANER",
    "ROLE_CLOG",
    "ROLE_COLLATOR",
    "ROLE_COMET",
    "ROLE_CONFUSED",
    "ROLE_CREWMATE",
    "ROLE_DAMNED",
    "ROLE_DAMNED_IMPOSTOR",
    "ROLE_DANCER",
    "ROLE_DESTROYER",
    "ROLE_DISCLOSURE",
    "ROLE_DISTURBER",
    "ROLE_DOCTOR",
    "ROLE_ECHO",
    "ROLE_EFFACER",
    "ROLE_EMBER",
    "ROLE_EVIL_GUESSER",
    "ROLE_EVIL_TRACKER",
    "ROLE_EVIL_TRAPPER",
    "ROLE_EXTRA_MISSION",
    "ROLE_GRUDGE",
    "ROLE_GUESSER",
    "ROLE_HADAR",
    "ROLE_HALLUCINATION",
    "ROLE_ILLUSIONER",
    "ROLE_IMPOSTOR",
    "ROLE_JACKAL",
    "ROLE_JAILER",
    "ROLE_JESTER",
    "ROLE_JUSTICE",
    "ROLE_LOVER",
    "ROLE_LUMINE",
    "ROLE_MADMATE",
    "ROLE_MARIONETTE",
    "ROLE_MAYOR",
    "ROLE_META_ROLE",
    "ROLE_MORPHING",
    "ROLE_NECROMANCER",
    "ROLE_NICE_GUESSER",
    "ROLE_NICE_TRAPPER",
    "ROLE_NOISEGHOST",
    "ROLE_NOISEMAKER",
    "ROLE_OBSESSIONAL",
    "ROLE_PAINTER",
    "ROLE_PAPARAZZO",
    "ROLE_PHOSPHORUS",
    "ROLE_POLTERGEIST",
    "ROLE_PROVOCATEUR",
    "ROLE_PSYCHIC",
    "ROLE_RAIDER",
    "ROLE_REAPER",
    "ROLE_SCARLET",
    "ROLE_SCARLET_LOVER",
    "ROLE_SECRET",
    "ROLE_SECRET_IN_GAME",
    "ROLE_SEER",
    "ROLE_SHERIFF",
    "ROLE_SIDEKICK",
    "ROLE_SNIPER",
    "ROLE_STIRRER",
    "ROLE_THURIFER",
    "ROLE_TIE_BREAKER",
    "ROLE_UBIQUITOUS",
    "ROLE_VULTURE",
};

int nebula_role_count(void) { return ROLE_N; }
int nebula_role_id_at(int idx) { return (idx>=0 && idx<ROLE_N) ? ROLES[idx].id : 0; }
int nebula_role_id_by_key(const char *key) {
    if (!key) return -1;
    for (int i=0;i<ROLE_N;i++) if (ROLES[i].key && strcmp(ROLES[i].key, key)==0) return ROLES[i].id;
    return -1;
}

const char *nebula_role_color(int role) {
    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].color;
    return "#FFFFFF";
}

const char *nebula_role_enum_name(int role) {
    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ENUM_NAMES[i];
    return "";
}
