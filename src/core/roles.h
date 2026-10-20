/* roles.h — 由官方汉化包自动生成，请勿手改（改请用 tools/ 下的生成器）
 * 数据来源：Addons/【汉化-基础】SCBaseLangPack.zip → Language/SChinese.dat
 * 共 110 个官方职业，含官方中文名与阵营归属。 */
#ifndef NEBULA_ROLES_H
#define NEBULA_ROLES_H

typedef enum {
    ROLE_AGENT,                      /* 代理人 */
    ROLE_ALIEN,                      /* 外星人 */
    ROLE_AMALGAM,                    /* 融合体 */
    ROLE_ARSONIST,                   /* 纵火犯 */
    ROLE_AVENGER,                    /* 复仇者 */
    ROLE_BAIT,                       /* 诱饵 */
    ROLE_BALLOONHOLDER,              /* 气球持有者 */
    ROLE_BERSERKER,                  /* 易怒者 */
    ROLE_BLOODY,                     /* 血语者 */
    ROLE_BOUNTY_HUNTER,              /* 赏金猎人 */
    ROLE_BUBBLEGUN,                  /* 泡泡枪手 */
    ROLE_BUSKER,                     /* 灵化使 */
    ROLE_CAMOUFLAGER,                /* 隐蔽者 */
    ROLE_CANNON,                     /* 空气炮手 */
    ROLE_CHAIN_SHIFTER,              /* 交换师 */
    ROLE_CHAINSHIFTER,               /* 链移者 */
    ROLE_CLEANER,                    /* 清理者 */
    ROLE_CLIMBER,                    /* 攀登者 */
    ROLE_CLOG,                       /* 阻碍者 */
    ROLE_COLLATOR,                   /* 基因学家 */
    ROLE_COMET,                      /* 彗星之子 */
    ROLE_CONFUSED,                   /* 迷茫者 */
    ROLE_CREEPING,                   /* 潜行者 */
    ROLE_CREWMATE,                   /* 船员 */
    ROLE_CUPID,                      /* 丘比特 */
    ROLE_DAMNED,                     /* 受诅咒者 */
    ROLE_DAMNED_IMPOSTOR,            /* 受诅咒者 */
    ROLE_DANCER,                     /* 舞蹈家 */
    ROLE_DESTROYER,                  /* 毁灭者 */
    ROLE_DISCLOSURE,                 /* 显眼包 */
    ROLE_DISTURBER,                  /* 干扰者 */
    ROLE_DOCTOR,                     /* 医学家 */
    ROLE_DOPPELGANGER,               /* 二重身 */
    ROLE_ECHO,                       /* 感知者 */
    ROLE_EFFACER,                    /* 擦除者 */
    ROLE_EMBER,                      /* 余烬 */
    ROLE_EVIL_GUESSER,               /* 邪恶的赌怪 */
    ROLE_EVIL_TRACKER,               /* 邪恶的追踪者 */
    ROLE_EVIL_TRAPPER,               /* 邪恶的陷阱师 */
    ROLE_EVILTRACKER,                /* 邪恶追踪者 */
    ROLE_EXTRA_MISSION,              /* 额外任务 */
    ROLE_FIXER,                      /* 修理工 */
    ROLE_GAMBLER,                    /* 赌徒 */
    ROLE_GIMLET,                     /* 钻头 */
    ROLE_GRAFFITI,                   /* 涂鸦者 */
    ROLE_GRUDGE,                     /* 怨灵 */
    ROLE_GUESSER,                    /* 赌怪 */
    ROLE_HADAR,                      /* 矿工 */
    ROLE_HALLUCINATION,              /* 幻形 */
    ROLE_ILLUSIONER,                 /* 模仿大师 */
    ROLE_IMPOSTOR,                   /* 伪装者 */
    ROLE_INVESTIGATOR,               /* 调查员 */
    ROLE_JACKAL,                     /* 豺狼 */
    ROLE_JAILER,                     /* 狱卒 */
    ROLE_JEKYLLANDHYDE,              /* 双重人格 */
    ROLE_JESTER,                     /* 小丑 */
    ROLE_JUSTICE,                    /* 大神官 */
    ROLE_JUSTICE_PLUS,               /* 审判长 */
    ROLE_LOVER,                      /* 恋人 */
    ROLE_LUMINE,                     /* 引路魂 */
    ROLE_MADMATE,                    /* 叛徒 */
    ROLE_MARIONETTE,                 /* 木偶师 */
    ROLE_MAYOR,                      /* 市长 */
    ROLE_META_ROLE,                  /* 自由模式附加能力 */
    ROLE_MORPHING,                   /* 化形者 */
    ROLE_NAVVY,                      /* 杂役 */
    ROLE_NECROMANCER,                /* 死灵法师 */
    ROLE_NEXUS,                      /* 枢纽 */
    ROLE_NICE_GUESSER,               /* 正义的赌怪 */
    ROLE_NICE_TRAPPER,               /* 正义的陷阱师 */
    ROLE_NICETRACKER,                /* 正义追踪者 */
    ROLE_NIGHTMARE,                  /* 梦魇 */
    ROLE_NOISEGHOST,                 /* 吵闹鬼 */
    ROLE_NOISEMAKER,                 /* 大嗓门 */
    ROLE_OBSESSIONAL,                /* 暗恋者 */
    ROLE_OPPORTUNIST,                /* 机会主义者 */
    ROLE_ORACLE,                     /* 神谕者 */
    ROLE_PAINTER,                    /* 画皮师 */
    ROLE_PAPARAZZO,                  /* 摄影师 */
    ROLE_PHOSPHORUS,                 /* 提灯人 */
    ROLE_PLAGUE,                     /* 瘟疫 */
    ROLE_POLTERGEIST,                /* 捣蛋鬼 */
    ROLE_PROVOCATEUR,                /* 挑衅者 */
    ROLE_PSYCHIC,                    /* 灵媒 */
    ROLE_QUACK,                      /* 庸医 */
    ROLE_RAIDER,                     /* 强盗 */
    ROLE_REAPER,                     /* 送葬者 */
    ROLE_ROKUROKUBI,                 /* 辘轳首 */
    ROLE_SCARLET,                    /* 魅魔 */
    ROLE_SCARLET_LOVER,              /* 魅魔的恋人 */
    ROLE_SCULPTOR,                   /* 雕塑家 */
    ROLE_SECRET,                     /* 觉醒者 */
    ROLE_SECRET_IN_GAME,             /* 觉醒者 */
    ROLE_SEER,                       /* 先知 */
    ROLE_SHERIFF,                    /* 警长 */
    ROLE_SIDEKICK,                   /* 跟班 */
    ROLE_SNATCHER,                   /* 掠夺者 */
    ROLE_SNIPER,                     /* 狙击手 */
    ROLE_SPECTRE,                    /* 幽灵 */
    ROLE_STIRRER,                    /* 幻术师 */
    ROLE_SWAPPER,                    /* 交换者 */
    ROLE_THURIFER,                   /* 调香师 */
    ROLE_TIE_BREAKER,                /* 破平者 */
    ROLE_TRAPPER,                    /* 陷阱师 */
    ROLE_TYRANT,                     /* 暴君 */
    ROLE_UBIQUITOUS,                 /* 飞手 */
    ROLE_VANITY,                     /* 虚荣者 */
    ROLE_VULTURE,                    /* 秃鹫 */
    ROLE_WHAMMY,                     /* 厄运师 */
    ROLE_ZEAL,                       /* 狂热者 */
    ROLE_COUNT
} nebula_role_id_t;

typedef enum { TEAM_CREW = 0, TEAM_IMPOSTOR = 1, TEAM_NEUTRAL = 2 } nebula_team_t;

const char *nebula_role_name(int role);
nebula_team_t nebula_role_team(int role);
const char *nebula_role_key(int role);   /* 官方英文 id，如 sheriff */
int  nebula_role_count(void);             /* 职业总数 */
int  nebula_role_id_at(int idx);          /* 按序号取枚举值，便于遍历 */
const char *nebula_role_enum_name(int role); /* 枚举名，如 ROLE_SHERIFF */
const char *nebula_role_color(int role);  /* 职业颜色 #RRGGBB */
int  nebula_role_id_by_key(const char *key);   /* 官方英文 id -> 枚举值，如 sheriff */

#endif /* NEBULA_ROLES_H */
