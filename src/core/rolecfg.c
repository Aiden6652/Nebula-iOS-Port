/*
 * rolecfg.c — 职业阵容配置
 */

#include "rolecfg.h"
#include <unistd.h>
#include "roles.h"
#include "../il2cpp/bridge.h"   /* nebula_log */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    char *e = s + strlen(s) - 1;
    while (e >= s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t')) *e-- = '\0';
}

int nebula_rolecfg_default(int *out_ids, int max, int *out_n)
{
    static const char *def[] = {
        "sheriff", "jackal", "sniper", "mayor",
        "doctor",  "seer",   "jester", "bait"
    };
    int n = 0;
    for (size_t i = 0; i < sizeof(def) / sizeof(def[0]) && n < max; i++) {
        int id = nebula_role_id_by_key(def[i]);
        if (id >= 0) out_ids[n++] = id;
    }
    if (out_n) *out_n = n;
    return n;
}

int nebula_rolecfg_load(const char *path, int *out_ids, int max, int *out_n)
{
    if (!out_ids || max <= 0) return -1;
    int n = 0;

    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) {
        nebula_log("rolecfg: 未找到 %s，使用默认阵容", path ? path : "(null)");
        return nebula_rolecfg_default(out_ids, max, out_n);
    }

    char line[256];
    while (fgets(line, sizeof(line), f) && n < max) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        /* 解析 "key" 或 "key xN" */
        int count = 1;
        char *sp = strchr(line, ' ');
        char *x = strchr(line, 'x');
        if (x && (x == line || *(x - 1) == ' ')) {
            count = atoi(x + 1);
            if (count < 1) count = 1;
            if (count > 8) count = 8;
            *x = '\0';
            trim(line);
        }
        (void)sp;

        int id = nebula_role_id_by_key(line);
        if (id < 0) {
            nebula_log("rolecfg: 未知职业 key: %s（已跳过）", line);
            continue;
        }
        for (int k = 0; k < count && n < max; k++) out_ids[n++] = id;
    }
    fclose(f);

    if (n == 0) {
        nebula_log("rolecfg: 配置为空，使用默认阵容");
        return nebula_rolecfg_default(out_ids, max, out_n);
    }
    if (out_n) *out_n = n;
    nebula_log("rolecfg: 载入 %d 个上场职业", n);
    return n;
}

/* 首次运行把默认阵容落盘，便于用户在电脑上直接改 */
int nebula_rolecfg_emit_default(const char *path)
{
    if (!path) return -1;
    /* ★ 已存在就不要覆盖：否则每次启动都会把用户改好的阵容重置成默认值 */
    if (access(path, F_OK) == 0) return 0;
    int ids[ROLECFG_MAX], n = 0;
    if (nebula_rolecfg_default(ids, ROLECFG_MAX, &n) <= 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# 本局上场职业（官方英文 id，一行一个，可写 xN 指定数量）\n");
    fprintf(f, "# 可用 id 共 77 个，见 src/core/roles.h\n");
    for (int i = 0; i < n; i++) {
        const char *k = nebula_role_key(ids[i]);
        if (k && k[0]) fprintf(f, "%s\n", k);
    }
    fclose(f);
    return 0;
}

int nebula_rolecfg_pick(const int *ids, int n, int idx)
{
    if (!ids || n <= 0) return -1;
    return ids[idx % n];
}
