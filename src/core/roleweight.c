/*
 * roleweight.c — 267 个职业的权重表（供独立 UI 面板调整）
 * 详见 roleweight.h。纯 C 逻辑，可在 Linux 上本地测试。
 */

#include "roleweight.h"
#include "roles.h"
#include "../il2cpp/bridge.h"   /* nebula_log */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ROLES  512          /* 职业表上限（当前 267） */
static int idx_to_id(int idx);   /* 前置声明：下面的函数会用到 */

static int g_w[MAX_ROLES];
static int g_count = 0;         /* 职业总数 */
static int g_loaded = 0;

static void ensure_count(void)
{
    if (!g_count) {
        g_count = nebula_role_count();
        if (g_count > MAX_ROLES) g_count = MAX_ROLES;
    }
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    char *e = s + strlen(s) - 1;
    while (e >= s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t')) *e-- = '\0';
}

void nebula_weight_reset_default(void)
{
    ensure_count();
    static const char *def[] = { "sheriff", "jackal", "sniper", "mayor",
                                 "doctor",  "seer",   "jester", "bait" };
    memset(g_w, 0, sizeof(g_w));
    for (size_t i = 0; i < sizeof(def) / sizeof(def[0]); i++) {
        int id = nebula_role_id_by_key(def[i]);
        if (id >= 0 && id < MAX_ROLES) g_w[id] = 1;
    }
    g_loaded = 1;
}

/* ---------------- 附加职业（副职业） ---------------- */
static int g_sub[MAX_ROLES];

static void sub_ensure(void) { ensure_count(); }

int nebula_subrole_load(const char *path)
{
    sub_ensure();
    memset(g_sub, 0, sizeof(g_sub));
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        int id = nebula_role_id_by_key(line);
        if (id >= 0 && id < MAX_ROLES) g_sub[id] = 1;
    }
    fclose(f);
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        int id = nebula_role_id_at(i);
        if (id >= 0 && id < MAX_ROLES && g_sub[id]) n++;
    }
    nebula_log("subrole: 载入 %d 个附加职业", n);
    return n;
}

int nebula_subrole_save(const char *path)
{
    if (!path) return -1;
    sub_ensure();
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# 附加职业：挂上去后【技能叠加在主职业之上】\n");
    fprintf(f, "# 例：主职业写 sheriff，这里写 gambler → 你同时拥有警长和赌怪的能力\n\n");
    for (int i = 0; i < g_count; i++) {
        int id = nebula_role_id_at(i);
        if (id < 0 || id >= MAX_ROLES || !g_sub[id]) continue;
        const char *k = nebula_role_key(id);
        if (k && k[0]) fprintf(f, "%s\n", k);
    }
    fclose(f);
    return 0;
}

int nebula_subrole_count(void)
{
    sub_ensure();
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        int id = nebula_role_id_at(i);
        if (id >= 0 && id < MAX_ROLES && g_sub[id]) n++;
    }
    return n;
}

int nebula_subrole_list(int *out_ids, int max)
{
    sub_ensure();
    int n = 0;
    for (int i = 0; i < g_count && n < max; i++) {
        int id = nebula_role_id_at(i);
        if (id >= 0 && id < MAX_ROLES && g_sub[id]) out_ids[n++] = id;
    }
    return n;
}

int nebula_subrole_is_on(int idx)
{
    int id = idx_to_id(idx);
    return (id >= 0 && id < MAX_ROLES) ? g_sub[id] : 0;
}

void nebula_subrole_set(int idx, int on)
{
    int id = idx_to_id(idx);
    if (id >= 0 && id < MAX_ROLES) g_sub[id] = on ? 1 : 0;
}

void nebula_subrole_toggle(int idx)
{
    int id = idx_to_id(idx);
    if (id >= 0 && id < MAX_ROLES) g_sub[id] = g_sub[id] ? 0 : 1;
}

void nebula_weight_clear(void)
{
    ensure_count();
    memset(g_w, 0, sizeof(g_w));
    g_loaded = 1;
}

int nebula_weight_load(const char *path)
{
    ensure_count();
    memset(g_w, 0, sizeof(g_w));

    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) {
        nebula_weight_reset_default();      /* 文件不存在 → 默认阵容 */
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        int count = 1;
        char *x = strchr(line, 'x');
        if (x && (x == line || *(x - 1) == ' ')) {
            count = atoi(x + 1);
            if (count < 1) count = 1;
            if (count > WEIGHT_MAX) count = WEIGHT_MAX;
            *x = '\0';
            trim(line);
        }
        int id = nebula_role_id_by_key(line);
        if (id >= 0 && id < MAX_ROLES) g_w[id] = count;
    }
    fclose(f);
    g_loaded = 1;

    int n = 0;
    for (int i = 0; i < g_count; i++) {
        int id = nebula_role_id_at(i);
        if (id >= 0 && id < MAX_ROLES && g_w[id] > 0) n++;
    }
    nebula_log("roleweight: 载入 %d 个职业有权重", n);
    return n;
}

int nebula_weight_save(const char *path)
{
    if (!path) return -1;
    ensure_count();
    if (!g_loaded) nebula_weight_reset_default();

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# 职业上场权重（由游戏内「职业」面板生成，也可手改）\n");
    fprintf(f, "# 格式：key xN    N=上场份数；不写或 0 = 不上场（最多 %d）\n\n", WEIGHT_MAX);
    for (int i = 0; i < g_count; i++) {
        int id = nebula_role_id_at(i);
        if (id < 0 || id >= MAX_ROLES || g_w[id] <= 0) continue;
        const char *k = nebula_role_key(id);
        if (k && k[0]) fprintf(f, "%s x%d\n", k, g_w[id]);
    }
    fclose(f);
    return 0;
}

/* 面板下标 → 职业 id：下标与 id 不是一回事，必须经 nebula_role_id_at 转换。
   之前直接把下标当 id 用，导致权重错位、并把越界 id 交给上层（真机会崩）。 */
static int idx_to_id(int idx)
{
    ensure_count();
    if (idx < 0 || idx >= g_count) return -1;
    return nebula_role_id_at(idx);
}

int  nebula_weight_count(void) { ensure_count(); return g_count; }

const char *nebula_weight_key(int idx)
{
    ensure_count();
    if (idx < 0 || idx >= g_count) return NULL;
    return nebula_role_key(nebula_role_id_at(idx));
}

const char *nebula_weight_name(int idx)
{
    ensure_count();
    if (idx < 0 || idx >= g_count) return NULL;
    return nebula_role_name(nebula_role_id_at(idx));
}

int nebula_weight_get(int idx)
{
    int id = idx_to_id(idx);
    return (id >= 0 && id < MAX_ROLES) ? g_w[id] : 0;
}

void nebula_weight_set(int idx, int w)
{
    int id = idx_to_id(idx);
    if (id < 0 || id >= MAX_ROLES) return;
    if (w < 0) w = 0;
    if (w > WEIGHT_MAX) w = WEIGHT_MAX;
    g_w[id] = w;
    g_loaded = 1;
}

void nebula_weight_bump(int idx, int delta)
{
    int id = idx_to_id(idx);
    if (id < 0 || id >= MAX_ROLES) return;
    nebula_weight_set(idx, g_w[id] + delta);
}

int nebula_weight_expand(int *out_ids, int max)
{
    ensure_count();
    if (!g_loaded) nebula_weight_reset_default();
    int n = 0;
    for (int i = 0; i < g_count && n < max; i++) {
        int id = nebula_role_id_at(i);
        if (id < 0 || id >= MAX_ROLES) continue;
        for (int k = 0; k < g_w[id] && n < max; k++) out_ids[n++] = id;
    }
    if (n == 0) {   /* 一个都没配 → 至少给一个船员，避免空局 */
        out_ids[0] = nebula_role_id_by_key("crewmate");
        n = (out_ids[0] >= 0) ? 1 : 0;
    }
    return n;
}
