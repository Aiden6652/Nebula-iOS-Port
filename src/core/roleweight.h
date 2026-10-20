/*
 * roleweight.h — 267 个职业的【上场权重】表（供独立 UI 面板调整）
 *
 * 为什么需要它：原版大厅的身份调整列表只有 11 个固定槽位，装不下 267 个职业。
 * 所以原版那 11 个槽位【废弃】，改由我们自己的面板接管全部职业的概率设置。
 *
 * 存储：<Documents>/nebula_roles.txt，格式与原来兼容：
 *      sheriff x2
 *      jackal
 *   权重 = 上场份数；0 或不写 = 不上场。
 */

#ifndef NEBULA_ROLEWEIGHT_H
#define NEBULA_ROLEWEIGHT_H

#define WEIGHT_MAX      15      /* 单个职业最多上场几份（防手滑点爆） */

/* 载入配置；文件不存在 → 用默认阵容。返回启用职业数。 */
int  nebula_weight_load(const char *path);

/* 保存到配置文件（只写权重 > 0 的职业） */
int  nebula_weight_save(const char *path);

/* 职业总数（267） */
int  nebula_weight_count(void);

/* 第 idx 个职业的 key / 中文名 / 当前权重 */
const char *nebula_weight_key(int idx);
const char *nebula_weight_name(int idx);
int  nebula_weight_get(int idx);

/* 设置 / 增减权重（自动夹在 0..WEIGHT_MAX） */
void nebula_weight_set(int idx, int w);
void nebula_weight_bump(int idx, int delta);

/* 按权重展开成"上场职业序列"（sheriff x2 → 出现两次），返回个数 */
int  nebula_weight_expand(int *out_ids, int max);

/* —— 附加职业（副职业）——
   主职业只有一个，但可以同时挂多个附加职业，技能取并集。
   例如「警长 + 附加赌怪」= 同时拥有警长和赌怪的能力。
   存在 <Documents>/nebula_subroles.txt，一行一个 key。 */
int  nebula_subrole_load(const char *path);
int  nebula_subrole_save(const char *path);
int  nebula_subrole_is_on(int idx);
void nebula_subrole_set(int idx, int on);
void nebula_subrole_toggle(int idx);
int  nebula_subrole_count(void);
/* 把当前开启的附加职业 id 填进 out，返回个数 */
int  nebula_subrole_list(int *out_ids, int max);

/* 全清（一个都不上场）与恢复默认 */
void nebula_weight_clear(void);
void nebula_weight_reset_default(void);

#endif /* NEBULA_ROLEWEIGHT_H */
