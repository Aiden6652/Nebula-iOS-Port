/*
 * freeplay.h — 自由模式（Solo Free Play）
 *
 * 来源：移植自原版星云舰（Nebula on the Ship）。
 *   原版 Among Us 【没有】这个模式 —— 星云舰把原版的 freeplay 删掉，
 *   改成一个独立的【游戏模式】（DLL 里的 GameModeFreePlayImpl /
 *   SoloFreePlayOptions / FreePlayRoleAllocator / FreeplayPopover）。
 *   在自由模式里可以一个人试用职业。
 *
 * 解决的实际问题：用户一个人（iPad）无法联机，没法验证 267 个职业在真机上
 *   是否真的生效。自由模式让他单人开一局就能逐个试：
 *     ① 房间人数自动补齐（调游戏自带的 GameData.AddDummy，不用真的拉人）
 *     ② 自己的职业可以指定（role xxx）或按清单自动轮换（rotate 秒）
 *     ③ 底层角色映射强制开启（带刀职业一定拿到击杀按钮）
 *
 * 开关：<Documents>/nebula_freeplay.txt（存在即开启）
 * 配置示例：
 *      fill 10        # 自动补齐到 10 人（0 = 不补齐，默认 10）
 *      role sheriff   # 固定自己的职业（写了就不轮换）
 *      rotate 20      # 没写 role 时，每隔 20 秒换清单里的下一个职业
 *      off            # 显式关闭
 */

#ifndef NEBULA_FREEPLAY_H
#define NEBULA_FREEPLAY_H

#include <stddef.h>

#define FP_FILL_DEFAULT   10    /* 默认补齐到 10 人（原版本地局默认人数） */
#define FP_FILL_MAX       15
#define FP_ROTATE_DEFAULT 20    /* 默认 20 秒换一个职业 */
#define FP_ROTATE_MAX     600
#define FP_ROLE_NONE      (-1)

typedef struct {
    int  enabled;       /* 自由模式是否开启 */
    int  fill;          /* 补齐到几人（0=不补齐） */
    int  role;          /* 固定职业 id（-1=按清单轮换） */
    int  rotate;        /* 轮换间隔秒（0=不轮换） */
    int  cursor;        /* 当前试到清单第几个 */
    int  total;         /* 清单总数（仅用于显示进度） */
    long last_switch;   /* 上次切换时间（秒） */
} nebula_freeplay_t;

/* 载入配置：path 不存在 → 未开启（enabled=0）。成功返回 0。 */
int nebula_freeplay_load(const char *path);

/* 首次运行生成配置示例（默认关闭） */
int nebula_freeplay_emit_sample(const char *path);

/* 取当前状态（内部静态结构，只读） */
const nebula_freeplay_t *nebula_freeplay_state(void);

/* 是否开启 */
int nebula_freeplay_enabled(void);

/* 目标人数（未开启返回 0） */
int nebula_freeplay_fill(void);

/* 玩家 idx 这一轮该拿哪个职业 id。
   roster/n 为本局上场职业清单（nebula_roles.txt）。
   自由模式下：idx==0（自己）用固定/轮换职业，其余按清单顺序。
   未开启时：完全按清单顺序（即原逻辑）。 */
int nebula_freeplay_role(int idx, const int *roster, int n);

/* 心跳：到期则把 cursor 推进到下一个职业。
   now_sec<=0 时自己取当前时间。返回 1 表示本轮切换了。 */
int nebula_freeplay_tick(int total, long now_sec);

/* 进度文本，如 "3/267"；未开启或非轮换状态返回 0。 */
int nebula_freeplay_progress(char *out, size_t osz);

#endif /* NEBULA_FREEPLAY_H */
