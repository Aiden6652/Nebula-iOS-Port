/*
 * rolecfg.h — 本局要上场哪些职业（可在电脑上改）
 *
 * 配置文件：<App Documents>/nebula_roles.txt
 * 每行一个【官方英文职业 id】（见 roles.h 的 key，如 sheriff / jackal），
 * 可选带数量：  sheriff x2
 * 支持 # 注释。
 *
 * 文件不存在或为空 → 用内置默认阵容。
 */

#ifndef NEBULA_ROLECFG_H
#define NEBULA_ROLECFG_H

#include <stddef.h>
#include "../il2cpp/bridge.h"   /* 仅用于 nebula_log */

#define ROLECFG_MAX 32

/* 在 core 中实现；此处仅声明，避免引入 core.h（其枚举与 roles.h 冲突） */
void nebula_core_register_player(uint32_t net_id, int team);

/* 加载配置：成功返回职业个数（>=1）；失败返回 -1。
   out_ids 填【展开后】的职业枚举值序列（sheriff x2 → 出现两次）。 */
int  nebula_rolecfg_load(const char *path, int *out_ids, int max, int *out_n);

/* 内置默认阵容（文件不存在时使用） */
int  nebula_rolecfg_default(int *out_ids, int max, int *out_n);

/* 首次运行把默认阵容写到 path，便于用户在电脑上改 */
int  nebula_rolecfg_emit_default(const char *path);

/* 取第 idx 个上场职业（超出则回绕），便于给玩家循环分配 */
int  nebula_rolecfg_pick(const int *ids, int n, int idx);

#endif /* NEBULA_ROLECFG_H */
