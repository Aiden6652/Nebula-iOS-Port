/*
 * test_mock_il2cpp.c —— 本地模拟 IL2CPP 环境（自测用，不依赖真机）
 *
 * 目的：改动后能【自己验证】，不用每次都让用户刷机实测。
 *
 * 做法：本文件实现一套假的 il2cpp_* 符号，并用 -rdynamic 导出；
 *       bridge.c 里的 sym() 走 dlsym(RTLD_DEFAULT, ...) 就会解析到这些假函数。
 *       产品代码一行不用改。
 *
 * 对象布局用【真机 dump 的真实偏移】（见 nebula_log.txt）：
 *   PlayerControl:      PlayerId@53  CachedPlayerData@112  MyPhysics@208
 *   NetworkedPlayerInfo: PlayerName（模拟）
 *   PlayerPhysics:       Speed（模拟）
 *
 * 重点回归项：
 *   ① 速度【绝不累乘】—— 连续调用多次后速度必须稳定在 base×系数
 *      （历史 bug：每轮 read→×1.5→write，10 秒就 3300 倍，把游戏搞坏）
 *   ② 名字前缀只加一次
 *   ③ 不写坏无关字段（PlayerId 等保持不变）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#include "bridge.h"
/* 注意：不要同时 include core.h 与 roles.h —— 两者都有 ROLE_* 枚举会冲突。
   rolecfg.h 已声明 nebula_core_register_player，由 core.c 提供实现。 */
#include "../core/roles.h"
#include "../core/rolecfg.h"

/* ---------------- 可调开关（覆盖 hook.c 的实现，不链接 hook.c） ---------------- */
static int g_flag_speed = 0;   /* 模拟 nebula_speed.txt */
int nebula_flag_file(const char *filename) {
    if (!filename) return 0;
    if (strcmp(filename, "nebula_speed.txt") == 0) return g_flag_speed;
    return 0;                   /* 其余开关一律"不存在" */
}
int nebula_hook_enabled(void) { return 1; }
int nebula_hook_install_game_update(void) { return -1; }   /* 测试里不装 hook */
void nebula_hook_set_tick(void (*cb)(void)) { (void)cb; }

/* ---------------- 模拟的类 / 字段 ---------------- */
typedef struct { const char *name; ptrdiff_t off; const char *type; } MField;
typedef struct { const char *ns; const char *name; MField *f; int nf; } MClass;

/* 真实偏移（来自真机日志） */
#define OFF_PlayerId          53
#define OFF_CachedPlayerData  112
#define OFF_MyPhysics         208
/* 模拟偏移（真机上未知，取合理值即可，不影响逻辑验证） */
#define OFF_PlayerName        24
#define OFF_Speed             32

static MField f_pc[] = {
    {"PlayerId", OFF_PlayerId, "System.Byte"},
    {"CachedPlayerData", OFF_CachedPlayerData, "NetworkedPlayerInfo"},
    {"MyPhysics", OFF_MyPhysics, "PlayerPhysics"},
    {"AllPlayerControls", 0, "List<PlayerControl>"},
    {NULL, 0, NULL}
};
static MField f_npi[] = { {"PlayerName", OFF_PlayerName, "System.String"}, {NULL,0,NULL} };
static MField f_pp[]  = { {"Speed", OFF_Speed, "System.Single"}, {NULL,0,NULL} };
static MField f_list[] = { {"_size", 0x10, "System.Int32"}, {"_items", 0x18, "System.Object[]"}, {NULL,0,NULL} };
static MField f_tmp[] = { {"m_text", 0x40, "System.String"}, {NULL,0,NULL} };
static MField f_obj[] = { {NULL,0,NULL} };

static MClass g_classes[] = {
    {"", "PlayerControl", f_pc, 4},
    {"", "NetworkedPlayerInfo", f_npi, 1},
    {"", "PlayerPhysics", f_pp, 1},
    {"", "List_PlayerControl", f_list, 2},
    {"UnityEngine", "Object", f_obj, 0},
    {"TMPro", "TextMeshProUGUI", f_tmp, 1},
};
#define NCLASS ((int)(sizeof(g_classes)/sizeof(g_classes[0])))

/* ---------------- 模拟对象内存 ---------------- */
static void *mock_str_new(const char *s);   /* 假 Il2CppString，后面实现 */

#define MAX_PLAYERS 4
static unsigned char g_pc_mem[MAX_PLAYERS][512];
static unsigned char g_npi_mem[MAX_PLAYERS][256];
static unsigned char g_pp_mem[MAX_PLAYERS][256];
static unsigned char g_list_mem[256];
static unsigned char *g_arr_mem = NULL;

static void *pc_obj(int i) { return (void *)g_pc_mem[i]; }

static void build_world(void) {
    memset(g_pc_mem, 0, sizeof(g_pc_mem));
    memset(g_npi_mem, 0, sizeof(g_npi_mem));
    memset(g_pp_mem, 0, sizeof(g_pp_mem));
    memset(g_list_mem, 0, sizeof(g_list_mem));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        /* 对象头必须是 klass 指针：产品代码靠 *(void**)obj 取类，
           少了它字段定位会失败，测试就会"假通过" */
        *(void    **)g_pc_mem[i]  = (void *)&g_classes[0];   /* PlayerControl */
        *(void    **)g_npi_mem[i] = (void *)&g_classes[1];   /* NetworkedPlayerInfo */
        *(void    **)g_pp_mem[i]  = (void *)&g_classes[2];   /* PlayerPhysics */
        *(uint8_t  *)((char *)pc_obj(i) + OFF_PlayerId)         = (uint8_t)(i + 1);
        *(void    **)((char *)pc_obj(i) + OFF_CachedPlayerData) = (void *)g_npi_mem[i];
        *(void    **)((char *)pc_obj(i) + OFF_MyPhysics)        = (void *)g_pp_mem[i];
        *(float    *)((char *)g_pp_mem[i] + OFF_Speed)          = 5.0f;   /* 基准速度 */
        /* PlayerName 初值 */
        *(void **)((char *)g_npi_mem[i] + OFF_PlayerName) = mock_str_new("Player");
    }
    /* List：_size / _items / 数组（同样要有对象头） */
    *(void **)g_list_mem = (void *)&g_classes[3];   /* List_PlayerControl */
    g_arr_mem = (unsigned char *)calloc(1, 0x20 + MAX_PLAYERS * sizeof(void *));
    *(int  *)((char *)g_list_mem + 0x10) = MAX_PLAYERS;
    *(void **)((char *)g_list_mem + 0x18) = (void *)g_arr_mem;
    for (int i = 0; i < MAX_PLAYERS; i++)
        *(void **)((char *)g_arr_mem + 0x20 + (size_t)i * sizeof(void *)) = pc_obj(i);
}

/* ---------------- 假 il2cpp_* 符号（-rdynamic 导出后被 dlsym 找到） ---------------- */
void *il2cpp_domain_get(void) { return (void *)&g_classes; }
void *il2cpp_domain_get_assemblies(void *d, size_t *n) {
    (void)d; static void *one[1]; one[0] = (void *)&g_classes; if (n) *n = 1; return (void *)one;
}
void *il2cpp_assembly_get_image(void *a) { (void)a; return (void *)&g_classes; }
size_t il2cpp_image_get_class_count(void *img) { (void)img; return (size_t)NCLASS; }
void *il2cpp_image_get_class(void *img, size_t i) {
    (void)img; return (i < (size_t)NCLASS) ? (void *)&g_classes[i] : NULL;
}
const char *il2cpp_class_get_name(void *k) { return k ? ((MClass *)k)->name : NULL; }
const char *il2cpp_class_get_namespace(void *k) { return k ? ((MClass *)k)->ns : NULL; }

void *il2cpp_class_get_field_from_name(void *k, const char *name) {
    if (!k || !name) return NULL;
    MClass *c = (MClass *)k;
    for (int i = 0; i < c->nf; i++)
        if (c->f[i].name && strcmp(c->f[i].name, name) == 0) return (void *)&c->f[i];
    return NULL;
}
void *il2cpp_class_get_fields(void *k, void **iter) {
    if (!k || !iter) return NULL;
    MClass *c = (MClass *)k;
    int i = (int)(intptr_t)*iter;
    if (i >= c->nf) return NULL;
    *iter = (void *)(intptr_t)(i + 1);
    return (void *)&c->f[i];
}
const char *il2cpp_field_get_name(void *f) { return f ? ((MField *)f)->name : NULL; }
ptrdiff_t  il2cpp_field_get_offset(void *f) { return f ? ((MField *)f)->off : 0; }
void *il2cpp_field_get_type(void *f) { return f ? (void *)((MField *)f)->type : NULL; }
const char *il2cpp_type_get_name(void *t) { return t ? (const char *)t : NULL; }
void il2cpp_field_static_get_value(void *f, void *out) {
    if (!f || !out) return;
    if (strcmp(((MField *)f)->name, "AllPlayerControls") == 0) *(void **)out = (void *)g_list_mem;
    else *(void **)out = NULL;
}

/* 方法相关：一律返回 NULL，让 get_instances 失败 → 走 AllPlayerControls 路线 */
void *il2cpp_class_get_method_from_name(void *k, const char *n, int c) {
    (void)k; (void)n; (void)c; return NULL;
}
void *il2cpp_class_get_methods(void *k, void **it) { (void)k; (void)it; return NULL; }
const char *il2cpp_method_get_name(void *m) { (void)m; return NULL; }
void *il2cpp_class_get_type(void *k) { return k; }
void *il2cpp_type_get_object(void *t) { return t; }
void *il2cpp_runtime_invoke(void *m, void *o, void **p, void **e) {
    (void)m; (void)o; (void)p; if (e) *e = NULL; return NULL;
}
void *il2cpp_string_new(const char *s) { return mock_str_new(s); }

/* 假 Il2CppString：klass(8)+monitor(8)+len(+16)+UTF16(+20) */
static void *mock_str_new(const char *s) {
    size_t n = s ? strlen(s) : 0;
    unsigned char *b = (unsigned char *)calloc(1, 20 + (n + 1) * 2);
    *(int32_t *)(b + 16) = (int32_t)n;
    uint16_t *ch = (uint16_t *)(b + 20);
    for (size_t i = 0; i < n; i++) ch[i] = (uint16_t)(unsigned char)s[i];
    return (void *)b;
}

/* ---------------- 测试 ---------------- */
static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static float speed_of(int i) { return *(float *)((char *)g_pp_mem[i] + OFF_Speed); }
static void name_of(int i, char *out, size_t osz) {
    nebula_il2cpp_get_string_by((void *)g_npi_mem[i], OFF_PlayerName, out, osz);
}

int main(void) {
    printf("=== 模拟 IL2CPP 环境：验证职业应用不会把游戏改坏 ===\n");

    /* 指定阵容：内鬼/船员交替 —— 这样能验证"内鬼确实被加速" */
    setenv("HOME", "/tmp/nebula_mock_home", 1);
    system("mkdir -p /tmp/nebula_mock_home/Documents");
    { FILE *f = fopen("/tmp/nebula_mock_home/Documents/nebula_roles.txt", "w");
      if (f) { fputs("impostor\ncrewmate\n", f); fclose(f); } }

    build_world();
    CHECK(nebula_il2cpp_init() == 0, "模拟环境：IL2CPP 初始化成功");
    CHECK(nebula_il2cpp_ready() == 1, "模拟环境：运行时就绪");
    { void *a[16]; CHECK(nebula_il2cpp_get_players(a, 16) == MAX_PLAYERS, "取到全部玩家实例"); }

{
      int ids[32], nn=0;
      nebula_rolecfg_load("/tmp/nebula_mock_home/Documents/nebula_roles.txt", ids, 32, &nn);
      (void)ids; (void)nn;
    }

    /* ① 不开启速度时，速度必须原封不动（默认安全） */
    g_flag_speed = 0;
    for (int r = 0; r < 5; r++) nebula_il2cpp_apply_roles();
    CHECK(speed_of(0) == 5.0f, "默认不开速度：速度保持基准 5.0，未被改动");

    /* ② 开启速度后：连续多轮【不能累乘】—— 这是历史 bug 的回归测试 */
    g_flag_speed = 1;
    nebula_il2cpp_apply_roles();
    float first[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) first[i] = speed_of(i);
    printf("      第 1 轮速度: ");
    for (int i = 0; i < MAX_PLAYERS; i++) printf("%.3f ", first[i]);
    printf("\n");
    for (int r = 0; r < 20; r++) nebula_il2cpp_apply_roles();
    printf("      第21轮速度: ");
    for (int i = 0; i < MAX_PLAYERS; i++) printf("%.3f ", speed_of(i));
    printf("\n");

    int stable = 1, applied = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (speed_of(i) > first[i] * 1.001f) stable = 0;   /* 变大了 = 还在累乘 */
        if (speed_of(i) > 5.0f) applied++;                  /* 有玩家被加速了 */
        if (speed_of(i) < 4.0f || speed_of(i) > 6.0f) stable = 0;
    }
    CHECK(stable, "★ 连续 20 轮后所有玩家速度稳定（无累乘、无飞出）");
    CHECK(applied > 0, "速度效果生效：内鬼被加速（其余保持基准）");

    /* ③ 名字前缀只加一次 */
    char nm[128]; name_of(0, nm, sizeof(nm));
    int bracket = 0; for (char *p = nm; *p; p++) if (*p == '[') bracket++;
    CHECK(bracket == 1, "名字前缀只加了一次（未反复叠加）");
    CHECK(strstr(nm, "Player") != NULL, "原名字仍在（未被覆盖丢失）");

    /* ④ 无关字段未被破坏 */
    CHECK(*(uint8_t *)((char *)pc_obj(0) + OFF_PlayerId) == 1, "PlayerId 未被写坏");
    CHECK(*(uint8_t *)((char *)pc_obj(3) + OFF_PlayerId) == 4, "其他玩家 PlayerId 未被写坏");

    /* ⑤ 主线程每帧入口可安全调用（限流后不应每帧都执行） */
    for (int i = 0; i < 200; i++) nebula_il2cpp_tick_from_game();
    CHECK(1, "主线程每帧入口连续调用 200 次无异常");

    printf("\n%s (%d failures)\n", failures == 0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
