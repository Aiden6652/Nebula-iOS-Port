/*
 * roles_ui.h — 原生面板（大厅里我们自己生成的职业面板）的数据接口
 *
 * 存在的意义：ObjC 那边（overlay.m）只负责画界面，
 * 所有数据读写都走这里的 C 函数 —— 好处是这层【能在 Linux 上本地测试】，
 * 不用真机就能验证"点 +/- 是否真的改变了权重并落盘"。
 */

#ifndef NEBULA_ROLES_UI_H
#define NEBULA_ROLES_UI_H

#include <stddef.h>

/* 初始化：传入 App 的 Documents 目录（配置都放这里） */
void nebula_ui_init(const char *docs_dir);

/* —— 职业列表（面板要画的 267 行） —— */
int         nebula_ui_role_count(void);
const char *nebula_ui_role_name(int idx);
int         nebula_ui_role_weight(int idx);

/* 面板点 +/- ：改权重并立即落盘 */
void nebula_ui_role_bump(int idx, int delta);
void nebula_ui_role_set(int idx, int w);

/* —— 附加职业（副职业）：勾上后技能叠加到主职业之上 —— */
int  nebula_ui_role_is_sub(int idx);
void nebula_ui_role_toggle_sub(int idx);
int  nebula_ui_sub_count(void);

/* —— 自由模式开关（面板顶部的开关按钮） —— */
int  nebula_ui_freeplay_on(void);
void nebula_ui_set_freeplay(int on);

/* —— 换职业（面板/悬浮按钮） —— */
void nebula_ui_next_role(void);

/* 当前自己试用的职业名（给面板标题显示） */
int  nebula_ui_current_role(char *out, size_t osz);

#endif /* NEBULA_ROLES_UI_H */
