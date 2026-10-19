/*
 * effects.h — 职业效果应用器（让职业真正有能力，而不只是显示个名字）
 *
 * 设计原则：优先【纯数据写入】，调用原版方法(invoke)只作为增强且失败即跳过。
 * 原因：数据写入不会崩；invoke 受方法签名影响，出错风险高。
 *
 * 已实现效果（均用真机 dump 确认的字段）：
 *   隐身   → PlayerControl.shouldAppearInvisible(Boolean) + invisibilityAlpha(Single)
 *   护盾   → PlayerControl.protectedByGuardianThisRound(Boolean)（游戏自带保护判定）
 *   速度   → PlayerPhysics.speed
 *   冷却   → PlayerControl.killTimer
 *   反制   → 新死亡检测 + 按引擎规则反制（调用原版 Die/Revive，失败则记日志跳过）
 *
 * 职业按【PlayerId】稳定映射，不会因实例顺序变化而跳变。
 */

#ifndef NEBULA_IL2CPP_EFFECTS_H
#define NEBULA_IL2CPP_EFFECTS_H

/* 登记/查询：某 PlayerId 的职业（effects 内部按 PlayerId 稳定分配） */
int nebula_effects_role_of(int player_id);
int nebula_effects_set_role(int player_id, int role_id);

/* 取玩家职业名（供 UI 等使用），未知返回 NULL */
const char *nebula_effects_role_name(int player_id);

/* 每 tick 调用：应用持续效果 + 处理死亡反制 */
int  nebula_effects_tick(void);

/* 死亡反制开关：放 nebula_no_revenge.txt 关闭（只保留纯数据效果） */
int  nebula_effects_revenge_enabled(void);

#endif
