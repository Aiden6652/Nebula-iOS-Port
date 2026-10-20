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

/* 全清（一个都不上场）与恢复默认 */
void nebula_weight_clear(void);
void nebula_weight_reset_default(void);

#endif /* NEBULA_ROLEWEIGHT_H */
