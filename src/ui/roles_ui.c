/*
 * roles_ui.c — 面板数据层：把权重表 / 自由模式状态暴露给原生 UI
 * 详见 roles_ui.h。ObjC 只管画，数据全在这里，且可本地测试。
 */

#include "roles_ui.h"

#include "../core/roleweight.h"
#include "../core/freeplay.h"
#include "../core/roles.h"
#include "../il2cpp/bridge.h"   /* nebula_log */

#include <stdio.h>
#include <string.h>

static char g_docs[1024] = {0};
static char g_roles_path[1024] = {0};
static char g_fp_path[1024] = {0};
static char g_sub_path[1024] = {0};

/* 面板里"当前正在编辑的主职业"：附加职业都挂在它下面 */
static int g_cur_main = -1;

/* 默认主职业：权重 > 0 的第一个（都没有就用第 0 个）。
   这样打开面板就能直接勾附加，不必先点职业名。 */
static void pick_default_main(void)
{
    int n = nebula_weight_count();
    for (int i = 0; i < n; i++) {
        if (nebula_weight_get(i) > 0) { g_cur_main = i; return; }
    }
    g_cur_main = (n > 0) ? 0 : -1;
}

void nebula_ui_init(const char *docs_dir)
{
    if (docs_dir) snprintf(g_docs, sizeof(g_docs), "%s", docs_dir);
    else          g_docs[0] = '\0';
    snprintf(g_roles_path, sizeof(g_roles_path), "%s/nebula_roles.txt", g_docs);
    snprintf(g_fp_path,    sizeof(g_fp_path),    "%s/nebula_freeplay.txt", g_docs);
    snprintf(g_sub_path,   sizeof(g_sub_path),   "%s/nebula_subroles.txt", g_docs);

    nebula_weight_load(g_roles_path);
    nebula_subrole_load(g_sub_path);
    nebula_freeplay_load(g_fp_path);
    if (g_cur_main < 0) pick_default_main();
}

int         nebula_ui_role_count(void)     { return nebula_weight_count(); }
const char *nebula_ui_role_name(int idx)   { return nebula_weight_name(idx); }
int         nebula_ui_role_weight(int idx) { return nebula_weight_get(idx); }

void nebula_ui_role_bump(int idx, int delta)
{
    nebula_weight_bump(idx, delta);
    if (g_roles_path[0]) nebula_weight_save(g_roles_path);
}

void nebula_ui_role_set(int idx, int w)
{
    nebula_weight_set(idx, w);
    if (g_roles_path[0]) nebula_weight_save(g_roles_path);
}

int nebula_ui_current_main(void) { return g_cur_main; }

void nebula_ui_set_current_main(int idx)
{
    if (idx < 0 || idx >= nebula_weight_count()) return;
    g_cur_main = idx;
}

int nebula_ui_current_main_name(char *out, size_t osz)
{
    if (!out || osz == 0 || g_cur_main < 0) return 0;
    const char *nm = nebula_weight_name(g_cur_main);
    if (!nm) return 0;
    snprintf(out, osz, "%s", nm);
    return 1;
}

int nebula_ui_role_is_sub(int idx)
{
    if (g_cur_main < 0) pick_default_main();
    if (g_cur_main < 0) return 0;
    return nebula_subrole_is_on(g_cur_main, idx);
}

void nebula_ui_role_toggle_sub(int idx)
{
    if (g_cur_main < 0) pick_default_main();   /* 没选过就自动选一个，别让点击没反应 */
    if (g_cur_main < 0) return;
    nebula_subrole_toggle(g_cur_main, idx);
    if (g_sub_path[0]) nebula_subrole_save(g_sub_path);
    nebula_log("面板：主职业[%s] 附加[%s] → %s",
               nebula_weight_name(g_cur_main) ? nebula_weight_name(g_cur_main) : "?",
               nebula_weight_name(idx) ? nebula_weight_name(idx) : "?",
               nebula_subrole_is_on(g_cur_main, idx) ? "已挂上" : "已取消");
}

int nebula_ui_role_is_allowed(int idx) { return nebula_subrole_is_allowed(idx); }

int nebula_ui_sub_count(void) { return nebula_subrole_count(); }

int nebula_ui_freeplay_on(void) { return nebula_freeplay_enabled(); }

void nebula_ui_set_freeplay(int on)
{
    nebula_freeplay_set_enabled(on ? 1 : 0,
                                g_fp_path[0] ? g_fp_path : NULL);
    nebula_log("面板：自由模式 %s", on ? "已开启" : "已关闭");
}

void nebula_ui_next_role(void)
{
    /* total=0 → 用 apply_roles 最近登记的清单长度 */
    nebula_freeplay_next(0);
}

int nebula_ui_current_role(char *out, size_t osz)
{
    if (!out || osz == 0) return 0;
    const nebula_freeplay_t *st = nebula_freeplay_state();
    int ids[512], n = nebula_weight_expand(ids, 512);
    int rid = nebula_freeplay_role(0, ids, n);
    if (rid < 0) return 0;
    const char *nm = nebula_role_name(rid);
    if (!nm) return 0;
    snprintf(out, osz, "%s", nm);
    (void)st;
    return 1;
}
