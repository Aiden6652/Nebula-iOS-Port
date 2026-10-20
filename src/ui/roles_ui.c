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

int nebula_ui_role_is_sub(int idx) { return nebula_subrole_is_on(idx); }

void nebula_ui_role_toggle_sub(int idx)
{
    nebula_subrole_toggle(idx);
    if (g_sub_path[0]) nebula_subrole_save(g_sub_path);
}

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
