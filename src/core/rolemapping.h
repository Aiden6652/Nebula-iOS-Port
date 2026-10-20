/*
 * rolemapping.h — 把我们的职业映射为【原版 RoleTypes】
 *
 * 思路（与 PC 端 BepInEx / RoleManager 一致）：
 *   游戏底层只认原版的几种角色（Crewmate / Impostor / Scientist / …），
 *   我们的 267 个职业在底层必须"伪装"成其中之一，否则：
 *     - 带刀职业（豺狼、警长）拿不到击杀按钮
 *     - 功能型职业无法正常使用通风管/任务
 *
 * 映射规则：
 *   带刀（内鬼阵营 / 豺狼 / 警长 / 复仇者 …） → Impostor
 *   功能型（大多数船员）                      → Crewmate
 *   有对应原版能力的                          → 映射过去（化形者→Shapeshifter 等）
 *   其余                                      → Crewmate
 */

#ifndef NEBULA_ROLE_MAPPING_H
#define NEBULA_ROLE_MAPPING_H

/* 原版 RoleTypes 枚举值（Among Us） */
#define RT_CREWMATE        0
#define RT_IMPOSTOR        1
#define RT_SCIENTIST       2
#define RT_ENGINEER        3
#define RT_GUARDIAN_ANGEL  4
#define RT_SHAPESHIFTER    5
#define RT_PHANTOM         6
#define RT_TRACKER         7
#define RT_NOISEMAKER      8
#define RT_DETECTIVE       9
#define RT_JUDGE          10
#define RT_SPIRITGUIDE    11
#define RT_VIPER          12

/* 取某职业在底层应伪装的【原版 RoleTypes】值。
   参数 key 为官方英文 id（如 "sheriff"），可为 NULL（则只按阵营判断）。 */
int nebula_role_underlying(int role_id, const char *key, int team_is_impostor);

/* 主职业 + 附加职业的【能力并集】映射：
   例：主=警长(带刀)，附加=赌怪 → 仍是 Impostor；
       主=船员，附加=工程师(能跳管) → Engineer（拿得到跳管能力）。
   附加职业的能力是【叠加】上去的，这也是"同时拥有两个职业技能"的底层依据。 */
int nebula_role_underlying_multi(int main_id, const int *subs, int nsub);

/* 该职业是否带刀（决定能否击杀） */
int nebula_role_has_knife(int role_id, const char *key, int team_is_impostor);

#endif
