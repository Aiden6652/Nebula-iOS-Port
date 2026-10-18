/*
 * addon.c — 星云舰原版插件（Addon）读取器 + 本地化
 *
 * 原版插件形态（已用真实插件 SCLangPack 验证）：
 *   一个【文件夹】或【ZIP 包】，丢进 addons/ 即可，内部为：
 *       addon.meta              JSON：Id / Name / Author / Description / Version
 *       icon.png
 *       Language/<名字>.dat     词条："key" : "value"，# 注释，CRLF
 *       MoreCosmic/Contents.json + hats/ visors/  (外观资源)
 *
 * 读取方式：不解压到磁盘，ZIP 直接在内存里 inflate 后解析（iOS 上 libz 可用）。
 * 注意：原版插件里的 C# 代码部分在 iOS 无法执行（无 .NET / AOT-only），
 *      但数据包（语言、配置、资源）可被本读取器完整吃掉。
 */

#include "addon.h"
#include "../il2cpp/bridge.h"   /* 复用 nebula_log */

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>

typedef struct { char key[NEBULA_LANG_KEYSZ]; char val[NEBULA_LANG_VALSZ]; int used; } lang_ent_t;

static lang_ent_t g_lang[NEBULA_LANG_MAX];
static char g_addon_names[32][128];
static int  g_addon_n = 0;
static int  g_builtin_loaded = 0;

/* ===================== 内置中文兜底（没装任何插件时也不会是英文） ===================== */
static const char *BUILTIN_ZH[][2] = {
    {"ROLE_CREW","船员"},{"ROLE_IMPOSTOR","内鬼"},{"ROLE_SHERIFF","警长"},
    {"ROLE_ENGINEER","工程师"},{"ROLE_JESTER","小丑"},{"ROLE_SEER","占卜师"},
    {"ROLE_MADMATE","狂信者"},{"ROLE_BODYGUARD","保镖"},{"ROLE_SNIPER","狙击手"},
    {"ROLE_AVENGER","易怒者"},{"ROLE_BUBBLEGUNNER","泡泡枪手"},
    {"ROLE_JACKAL","豺狼"},{"ROLE_SIDEKICK","跟班"},
    {"ROLE_NINJA","忍者"},{"ROLE_OUTSIDER","局外人"},
    {"MOD_NEET","NEET"},{"MOD_STAR","明星"},{"MOD_LIGHTING","照明"},
    {"MOD_SUNGLASS","墨镜"},{"MOD_WATCHING","旁观"},{"MOD_SENDING","传信"},
    {"UI_KILL","击杀"},{"UI_VENT","通风管"},{"UI_MEETING","紧急会议"},
    {"UI_VOTE","投票"},{"UI_REPORT","报告尸体"},{"UI_TASK","任务"},
    {"UI_SABOTAGE","破坏"},{"UI_SHIELD","护盾"},{"UI_REVEAL","识破"},{"UI_RECRUIT","招募"},
    {"MSG_WIN_CREW","船员阵营胜利"},{"MSG_WIN_IMPOSTOR","内鬼阵营胜利"},
    {"MSG_WIN_JACKAL","豺狼阵营胜利"},{"MSG_WIN_JESTER","小丑胜利"},
    {NULL,NULL}
};

static void load_builtin_zh(void)
{
    for (int i = 0; BUILTIN_ZH[i][0]; i++)
        nebula_lang_set(BUILTIN_ZH[i][0], BUILTIN_ZH[i][1]);
}

static void ensure_builtin(void)
{
    if (!g_builtin_loaded) { g_builtin_loaded = 1; load_builtin_zh(); }
}

void nebula_lang_set(const char *key, const char *value)
{
    if (!key || !value) return;
    ensure_builtin();
    for (int i = 0; i < NEBULA_LANG_MAX; i++)
        if (g_lang[i].used && strcmp(g_lang[i].key, key) == 0) {
            snprintf(g_lang[i].val, sizeof(g_lang[i].val), "%s", value); return;
        }
    for (int i = 0; i < NEBULA_LANG_MAX; i++)
        if (!g_lang[i].used) {
            snprintf(g_lang[i].key, sizeof(g_lang[i].key), "%s", key);
            snprintf(g_lang[i].val, sizeof(g_lang[i].val), "%s", value);
            g_lang[i].used = 1; return;
        }
}

const char *nebula_lang_get(const char *key)
{
    if (!key) return "";
    ensure_builtin();
    for (int i = 0; i < NEBULA_LANG_MAX; i++)
        if (g_lang[i].used && strcmp(g_lang[i].key, key) == 0) return g_lang[i].val;
    return key;
}

/* ===================== 词条解析（内存） =====================
 * 兼容两种写法：原版 "key" : "value" 与简易 key = value，# 为注释 */
static int lang_parse_mem(const char *text, size_t len)
{
    if (!text) return -1;
    size_t i = 0; int n = 0;
    char line[512];
    while (i < len) {
        size_t j = 0;
        while (i < len && j < sizeof(line) - 1) {
            char c = text[i++];
            if (c == '\n') break;
            if (c != '\r') line[j++] = c;
        }
        line[j] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\0') continue;
        if (*p == '[') continue;                       /* 忽略 section 行 */

        /* 找分隔符：优先 : 其次 = */
        char *sep = strchr(p, ':');
        if (!sep) sep = strchr(p, '=');
        if (!sep) continue;
        *sep = '\0';
        char *k = p, *v = sep + 1;

        /* 去引号与空白 */
        if (*k == '"') { k++; char *e = strrchr(k, '"'); if (e) *e = '\0'; }
        while (*v == ' ' || *v == '\t') v++;
        if (*v == '"') { v++; char *e = strrchr(v, '"'); if (e) *e = '\0'; }
        char *t = k + strlen(k) - 1;
        while (t > k && (*t == ' ' || *t == '\t')) *t-- = '\0';
        t = v + strlen(v) - 1;
        while (t > v && (*t == ' ' || *t == '\t')) *t-- = '\0';

        if (k[0] && v[0]) { nebula_lang_set(k, v); n++; }
    }
    return n;
}

static int lang_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    int n = lang_parse_mem(buf, rd);
    free(buf);
    return n;
}

/* ===================== addon.meta 极简解析（只取 Name / Version） ===================== */
static void meta_field(const char *text, const char *field, char *out, size_t osz)
{
    out[0] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    const char *p = strstr(text, needle);
    if (!p) return;
    p = strchr(p + strlen(needle), ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (!e) return;
        size_t n = (size_t)(e - p);
        if (n >= osz) n = osz - 1;
        memcpy(out, p, n); out[n] = '\0';
    }
}

/* ===================== ZIP 读取（中央目录 + raw inflate） ===================== */
static int raw_inflate(const unsigned char *src, size_t srcLen,
                       unsigned char *dst, size_t dstCap)
{
    z_stream zs; memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return -1;
    zs.next_in = (Bytef *)src; zs.avail_in = (uInt)srcLen;
    zs.next_out = dst;         zs.avail_out = (uInt)dstCap;
    int r = inflate(&zs, Z_FINISH);
    size_t out = dstCap - zs.avail_out;
    inflateEnd(&zs);
    return (r == Z_STREAM_END || r == Z_OK) ? (int)out : -1;
}

#define CD_SIG   0x02014b50u
#define EOCD_SIG 0x06054b50u
#define LFH_SIG  0x04034b50u

static unsigned int rd32(const unsigned char *b){ return (unsigned)(b[0]|(b[1]<<8)|((unsigned)b[2]<<16)|((unsigned)b[3]<<24)); }
static unsigned short rd16(const unsigned char *b){ return (unsigned short)(b[0]|(b[1]<<8)); }

/* 读整个 zip 到内存（插件一般几百 KB，可接受） */
static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)sz, f);
    fclose(f);
    *out_len = r;
    return b;
}

/* 遍历 zip 条目；对每个条目回调（name, 解压后数据, 长度） */
typedef void (*zip_cb)(const char *name, const char *data, size_t len, void *ctx);

static int zip_walk(const unsigned char *z, size_t zlen, zip_cb cb, void *ctx)
{
    /* 从尾部找 EOCD */
    long eo = -1;
    for (long i = (long)zlen - 22; i >= 0 && i > (long)zlen - 66000; i--) {
        if (rd32(z + i) == EOCD_SIG) { eo = i; break; }
    }
    if (eo < 0) return -1;
    unsigned int cd_off = rd32(z + eo + 16);
    unsigned short nent = rd16(z + eo + 10);

    const unsigned char *p = z + cd_off;
    for (unsigned short i = 0; i < nent; i++) {
        if ((size_t)(p - z) + 46 > zlen || rd32(p) != CD_SIG) break;
        unsigned short method = rd16(p + 10);
        unsigned int  csize   = rd32(p + 20);
        unsigned int  usize   = rd32(p + 24);
        unsigned short nlen   = rd16(p + 28);
        unsigned short elen   = rd16(p + 30);
        unsigned short clen   = rd16(p + 32);
        unsigned int  lho     = rd32(p + 42);
        char name[512];
        size_t nl = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
        memcpy(name, p + 46, nl); name[nl] = '\0';

        /* 定位数据 */
        const unsigned char *lh = z + lho;
        if ((size_t)(lh - z) + 30 <= zlen && rd32(lh) == LFH_SIG) {
            unsigned short ln = rd16(lh + 26), le = rd16(lh + 28);
            const unsigned char *data = lh + 30 + ln + le;
            if (method == 0) {
                if ((size_t)(data - z) + csize <= zlen) cb(name, (const char *)data, csize, ctx);
            } else if (method == 8 && usize > 0 && usize < (8u << 20)) {
                unsigned char *outb = (unsigned char *)malloc(usize + 1);
                if (outb) {
                    int got = raw_inflate(data, csize, outb, usize);
                    if (got > 0) { outb[got] = '\0'; cb(name, (const char *)outb, (size_t)got, ctx); }
                    free(outb);
                }
            }
        }
        p += 46 + nlen + elen + clen;
    }
    return 0;
}

/* ---- 回调：挑出 addon.meta 与 Language/*.dat ---- */
typedef struct { char name[128]; char ver[64]; int lang_n; } pkg_ctx_t;

static void pkg_cb(const char *n, const char *data, size_t len, void *ctxp)
{
    pkg_ctx_t *c = (pkg_ctx_t *)ctxp;
    if (strstr(n, "addon.meta")) {
        char *tmp = (char *)malloc(len + 1);
        if (!tmp) return;
        memcpy(tmp, data, len); tmp[len] = '\0';
        meta_field(tmp, "Name", c->name, sizeof(c->name));
        meta_field(tmp, "Version", c->ver, sizeof(c->ver));
        free(tmp);
    } else if (strstr(n, "Language/") && strstr(n, ".dat")) {
        int k = lang_parse_mem(data, len);
        if (k > 0) c->lang_n += k;
    }
}

/* ===================== 对外：加载一个插件包（zip 或文件夹） ===================== */
int nebula_addon_load_zip(const char *zip_path)
{
    size_t zlen = 0;
    unsigned char *z = slurp(zip_path, &zlen);
    if (!z) return -1;
    pkg_ctx_t c; memset(&c, 0, sizeof(c));
    zip_walk(z, zlen, pkg_cb, &c);
    free(z);
    nebula_log("addon[zip]: %s 名=%s 版=%s 载入词条=%d",
               zip_path, c.name[0] ? c.name : "?", c.ver[0] ? c.ver : "?", c.lang_n);
    return (c.lang_n > 0 || c.name[0]) ? 0 : -1;
}

int nebula_addon_load_dir(const char *dir_path)
{
    pkg_ctx_t c; memset(&c, 0, sizeof(c));
    char p[1024];
    snprintf(p, sizeof(p), "%s/addon.meta", dir_path);
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *t = (char *)malloc((size_t)sz + 1);
        if (t) { size_t r = fread(t, 1, (size_t)sz, f); t[r] = '\0';
                 meta_field(t, "Name", c.name, sizeof(c.name));
                 meta_field(t, "Version", c.ver, sizeof(c.ver)); free(t); }
        fclose(f);
    }
    snprintf(p, sizeof(p), "%s/Language", dir_path);
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            if (!strstr(e->d_name, ".dat")) continue;
            char fp[1024]; snprintf(fp, sizeof(fp), "%s/%s", p, e->d_name);
            int k = lang_load_file(fp);
            if (k > 0) c.lang_n += k;
        }
        closedir(d);
    }
    nebula_log("addon[dir]: %s 名=%s 版=%s 载入词条=%d",
               dir_path, c.name[0] ? c.name : "?", c.ver[0] ? c.ver : "?", c.lang_n);
    return (c.lang_n > 0 || c.name[0]) ? 0 : -1;
}

/* ===================== 扫描 addons/ ===================== */
static int has_ext(const char *n, const char *e)
{
    size_t nl = strlen(n), el = strlen(e);
    return nl >= el && strcasecmp(n + nl - el, e) == 0;
}

static void remember(const char *n)
{
    if (g_addon_n < 32) {
        snprintf(g_addon_names[g_addon_n], sizeof(g_addon_names[0]), "%s", n);
        g_addon_n++;
    }
}

/* builtin_only=1 只吃内置模板，保证它先加载、用户插件后加载（用户优先） */
static void scan_once(const char *dir, int builtin_only)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        int is_tpl = (strcmp(n, "lang_zh_CN.naddon") == 0);
        if (builtin_only != is_tpl) continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, n);

        struct stat st;
        int ok = -1;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            ok = nebula_addon_load_dir(full);                 /* 文件夹形态插件 */
        } else if (has_ext(n, ".zip")) {
            ok = nebula_addon_load_zip(full);                 /* ZIP 形态插件 */
        } else if (has_ext(n, ".dylib")) {
            void *h = dlopen(full, RTLD_NOW | RTLD_LOCAL);
            if (h) {
                void (*en)(void) = (void (*)(void))dlsym(h, "nebula_addon_entry");
                if (en) { en(); ok = 0; } else dlclose(h);
            }
        } else if (has_ext(n, ".naddon") || has_ext(n, ".ini") ||
                   has_ext(n, ".dat") || has_ext(n, ".txt") || has_ext(n, ".lang")) {
            ok = (lang_load_file(full) >= 0) ? 0 : -1;
        }
        if (ok == 0) remember(n);
    }
    closedir(d);
}

int nebula_addon_emit_builtin_langpack(const char *base_dir)
{
    if (!base_dir) return -1;
    char dir[1024], path[1024];
    snprintf(dir,  sizeof(dir),  "%s/%s", base_dir, NEBULA_ADDON_DIR);
    snprintf(path, sizeof(path), "%s/lang_zh_CN.naddon", dir);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# 内置中文兜底（装了原版汉化插件后以插件为准）\n[lang]\n");
    for (int i = 0; BUILTIN_ZH[i][0]; i++)
        fprintf(f, "%s=%s\n", BUILTIN_ZH[i][0], BUILTIN_ZH[i][1]);
    fclose(f);
    return 0;
}

int nebula_addon_init(const char *base_dir)
{
    load_builtin_zh();
    if (!base_dir) return -1;

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", base_dir, NEBULA_ADDON_DIR);
    struct stat st;
    if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0) {
        nebula_log("addon: 目录不可用 %s", dir);
        return -1;
    }
    nebula_addon_emit_builtin_langpack(base_dir);

    g_addon_n = 0;
    scan_once(dir, 1);   /* 先内置模板 */
    scan_once(dir, 0);   /* 再用户插件（覆盖优先） */

    nebula_log("addon: 共加载 %d 个插件（%s）", g_addon_n, dir);
    return g_addon_n;
}

int nebula_addon_count(void) { return g_addon_n; }
const char *nebula_addon_name(int idx)
{
    return (idx >= 0 && idx < g_addon_n) ? g_addon_names[idx] : NULL;
}

/* 保留：列出 zip 条目名（调试用） */
int nebula_zip_list(const char *zip_path)
{
    size_t zlen = 0;
    unsigned char *z = slurp(zip_path, &zlen);
    if (!z) return -1;
    pkg_ctx_t c; memset(&c, 0, sizeof(c));
    zip_walk(z, zlen, pkg_cb, &c);
    free(z);
    return 0;
}
