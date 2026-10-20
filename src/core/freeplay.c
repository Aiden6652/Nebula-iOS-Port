/*
 * freeplay.c — 自由模式（Solo Free Play）状态与配置
 *
 * 详见 freeplay.h。这里只做【纯逻辑】：解析配置、决定当前该拿哪个职业、
 * 到期轮换。真正去改游戏（补齐人数、写职业、写名字）在 il2cpp/bridge.c。
 */

#include "freeplay.h"
#include "roles.h"
#include "../il2cpp/bridge.h"   /* nebula_log */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

static nebula_freeplay_t g_fp = {0};

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    char *e = s + strlen(s) - 1;
    while (e >= s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t')) *e-- = '\0';
}

/* 取第一个空格后的整段（值） */
static const char *value_of(char *line)
{
    char *sp = strpbrk(line, " \t");
    if (!sp) return NULL;
    *sp = '\0';
    char *v = sp + 1;
    while (*v == ' ' || *v == '\t') v++;
    trim(v);
    return v;
}

int nebula_freeplay_load(const char *path)
{
    memset(&g_fp, 0, sizeof(g_fp));

    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) {
        /* 没有配置文件 = 不开启自由模式（保持原行为，不影响正常联机） */
        return -1;
    }

    g_fp.enabled = 1;
    g_fp.fill    = FP_FILL_DEFAULT;
    g_fp.role    = FP_ROLE_NONE;
    g_fp.rotate  = FP_ROTATE_DEFAULT;
    g_fp.autostart = 0;
    g_fp.cursor  = 0;
    g_fp.last_switch = (long)time(NULL);

    char line[256];
    int  saw_role = 0;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        char key[64];
        snprintf(key, sizeof(key), "%.63s", line);
        char *sp = strpbrk(key, " \t");
        if (sp) *sp = '\0';
        for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);

        if (strcmp(key, "off") == 0) { g_fp.enabled = 0; break; }

        const char *v = value_of(line);
        if (!v || !*v) continue;

        if (strcmp(key, "fill") == 0) {
            int n = atoi(v);
            if (n < 0) n = 0;
            if (n > FP_FILL_MAX) n = FP_FILL_MAX;
            g_fp.fill = n;
        } else if (strcmp(key, "role") == 0) {
            int id = nebula_role_id_by_key(v);
            if (id >= 0) {
                g_fp.role = id;
                saw_role = 1;
            } else {
                nebula_log("freeplay: 未知职业 key: %s（将按清单轮换）", v);
            }
        } else if (strcmp(key, "autostart") == 0) {
            g_fp.autostart = (atoi(v) != 0) ? 1 : 0;
        } else if (strcmp(key, "rotate") == 0) {
            int n = atoi(v);
            if (n < 0) n = 0;
            if (n > FP_ROTATE_MAX) n = FP_ROTATE_MAX;
            g_fp.rotate = n;
        }
    }
    fclose(f);

    /* 指定了固定职业 → 不轮换（用户是想盯着这一个测） */
    if (saw_role) g_fp.rotate = 0;

    if (g_fp.enabled) {
        nebula_log("freeplay: 自由模式已开启（补齐到 %d 人，%s）",
                   g_fp.fill,
                   saw_role ? "固定职业" : (g_fp.rotate > 0 ? "自动轮换试用" : "不轮换"));
    }
    return 0;
}

/* 首次运行生成配置示例（默认【关闭】：写 off），
   方便用户在 iPad 的「文件」App 里直接改，不用猜配置怎么写。 */
int nebula_freeplay_emit_sample(const char *path)
{
    if (!path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("# 自由模式（星云舰 Solo Free Play）：删掉下面的 off 即开启\n", f);
    fputs("# 一个人也能开局试职业：房间人数自动补齐 + 职业可按清单轮换\n\n", f);
    fputs("off\n\n", f);
    fputs("# 自动补齐到几人（0=不补齐，最多 15）\n", f);
    fputs("fill 10\n\n", f);
    fputs("# 固定自己拿哪个职业（写了就不轮换），例如：role sheriff\n", f);
    fputs("# 职业 id 见 nebula_roles_all.txt\n\n", f);
    fputs("# 不写 role 时，每隔多少秒自动换清单里的下一个职业（逐个试用）\n", f);
    fputs("rotate 20\n\n", f);
    fputs("# 兜底：上面都改不动时，在大厅直接调用开始（1=开，默认 0）\n", f);
    fputs("autostart 0\n", f);
    fclose(f);
    return 0;
}

const nebula_freeplay_t *nebula_freeplay_state(void) { return &g_fp; }

int nebula_freeplay_enabled(void) { return g_fp.enabled; }

int nebula_freeplay_fill(void) { return g_fp.enabled ? g_fp.fill : 0; }

int nebula_freeplay_autostart(void) { return g_fp.enabled ? g_fp.autostart : 0; }

int nebula_freeplay_role(int idx, const int *roster, int n)
{
    if (!g_fp.enabled) {
        if (!roster || n <= 0) return -1;
        return roster[idx % n];
    }
    /* 自己（第一个 PlayerControl）= 当前试用的职业 */
    if (idx == 0) {
        if (g_fp.role != FP_ROLE_NONE) return g_fp.role;
        if (!roster || n <= 0) return -1;
        return roster[g_fp.cursor % n];
    }
    /* 其余（补齐出来的 dummy 等）按清单顺序排 */
    if (!roster || n <= 0) return -1;
    return roster[(idx + g_fp.cursor) % n];
}

int nebula_freeplay_tick(int total, long now_sec)
{
    if (!g_fp.enabled || g_fp.rotate <= 0) return 0;
    if (total > 0) g_fp.total = total;

    long now = now_sec > 0 ? now_sec : (long)time(NULL);
    if (now - g_fp.last_switch < g_fp.rotate) return 0;

    g_fp.last_switch = now;
    int n = (total > 0) ? total : g_fp.total;
    if (n > 0) g_fp.cursor = (g_fp.cursor + 1) % n;
    else       g_fp.cursor++;
    return 1;
}

/* 手动切换（悬浮按钮）：立即换下一个，并重置自动轮换计时 */
void nebula_freeplay_next(int total)
{
    if (!g_fp.enabled || g_fp.role != FP_ROLE_NONE) return;   /* 固定职业不切换 */
    if (total > 0) g_fp.total = total;
    int n = (total > 0) ? total : g_fp.total;
    if (n > 0) g_fp.cursor = (g_fp.cursor + 1) % n;
    else       g_fp.cursor++;
    g_fp.last_switch = (long)time(NULL);   /* 重置：按钮切完不会紧接着自动又跳 */
}

/* 面板上的自由模式开关：立即改运行时状态，并把配置落盘
   （关 = 写 off 行；开 = 不写 off），下次启动保持用户的选择。 */
void nebula_freeplay_set_enabled(int on, const char *path)
{
    if (on) {
        g_fp.enabled = 1;
        if (g_fp.fill == 0) g_fp.fill = FP_FILL_DEFAULT;
        /* 首次开启且没配过轮换 → 给个默认轮换间隔 */
        if (g_fp.rotate == 0 && g_fp.role == FP_ROLE_NONE) g_fp.rotate = FP_ROTATE_DEFAULT;
    } else {
        g_fp.enabled = 0;
    }
    g_fp.last_switch = (long)time(NULL);

    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (!on) fputs("off\n\n", f);
    fprintf(f, "fill %d\n", g_fp.fill);
    if (g_fp.role != FP_ROLE_NONE) {
        const char *k = nebula_role_key(g_fp.role);
        if (k) fprintf(f, "role %s\n", k);
    }
    if (g_fp.rotate > 0) fprintf(f, "rotate %d\n", g_fp.rotate);
    fclose(f);
}

int nebula_freeplay_progress(char *out, size_t osz)
{
    if (!out || osz == 0) return 0;
    if (!g_fp.enabled || g_fp.role != FP_ROLE_NONE || g_fp.total <= 0) return 0;
    snprintf(out, osz, "%d/%d", (g_fp.cursor % g_fp.total) + 1, g_fp.total);
    return 1;
}
