/*
 * test_addon.c — host 端验证插件系统与汉化
 * 跑法：cd src/addon && make test
 */

#include "addon.h"
#include "../core/roles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static void touch(const char *p, const char *content) {
    FILE *f = fopen(p, "w");
    if (f) { fputs(content, f); fclose(f); }
}

int main(int argc, char **argv) {
    printf("=== Nebula addon / 汉化 host test ===\n");

    /* 1) 默认必须是中文，不能是英文 */
    CHECK(strcmp(nebula_lang_get("ROLE_IMPOSTOR"), "内鬼") == 0, "默认角色名为中文（内鬼）");
    CHECK(strcmp(nebula_lang_get("ROLE_JACKAL"),   "豺狼") == 0, "默认角色名为中文（豺狼）");
    CHECK(strcmp(nebula_lang_get("UI_MEETING"),    "紧急会议") == 0, "默认 UI 文案为中文");
    CHECK(strcmp(nebula_lang_get("UNKNOWN_KEY"),   "UNKNOWN_KEY") == 0, "查不到时回退原 key");

    /* 2) 准备临时 addons 目录 */
    const char *base = "/tmp/nebula_addon_test";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/Documents/%s", base, NEBULA_ADDON_DIR);
    system("rm -rf /tmp/nebula_addon_test && mkdir -p /tmp/nebula_addon_test/Documents/addons");

    /* 3) 放一个覆盖角色名的插件 */
    char f1[512]; snprintf(f1, sizeof(f1), "%s/my_zh.naddon", dir);
    touch(f1, "# 我的汉化覆盖\n[lang]\nROLE_IMPOSTOR = 伪装者\nUI_KILL = 干掉\n");

    int n = nebula_addon_init(base);
    CHECK(n >= 1, "插件被加载");
    CHECK(strcmp(nebula_lang_get("ROLE_IMPOSTOR"), "伪装者") == 0, "插件覆盖了角色名");
    CHECK(strcmp(nebula_lang_get("UI_KILL"), "干掉") == 0, "插件覆盖了 UI 文案");
    CHECK(strcmp(nebula_lang_get("ROLE_CREW"), "船员") == 0, "未覆盖项保持内置中文");

    /* 4) 应自动生成内置中文语言包文件，便于用户在电脑上改 */
    char lp[512]; snprintf(lp, sizeof(lp), "%s/lang_zh_CN.naddon", dir);
    struct stat st;
    CHECK(stat(lp, &st) == 0, "已生成 lang_zh_CN.naddon（可在电脑改）");

    /* 5) 容错：目录不存在 / 参数为 NULL 都不能崩 */
    CHECK(nebula_addon_init(NULL) == -1, "base_dir 为 NULL 安全返回 -1");
    CHECK(nebula_addon_count() >= 0, "插件计数可用");
    CHECK(nebula_addon_name(0) != NULL, "可取插件名");
    CHECK(nebula_addon_name(999) == NULL, "越界取插件名返回 NULL");

    /* 5.5) 官方职业表：内置中文应直接用官方名 */
    /* 77 个 NOS 内置 + 1 个 addons 扩展(审判长) = 78 */
    CHECK(nebula_role_count() == 78, "职业共 78 个（77 内置 + 审判长）");
    CHECK(strcmp(nebula_lang_get("ROLE_JACKAL"), "豺狼") == 0, "官方名：豺狼");
    CHECK(strcmp(nebula_lang_get("ROLE_SHERIFF"), "警长") == 0, "官方名：警长");
    CHECK(strcmp(nebula_lang_get("ROLE_ARSONIST"), "纵火犯") == 0, "官方名：纵火犯");
    CHECK(strcmp(nebula_lang_get("ROLE_NECROMANCER"), "死灵法师") == 0, "官方名：死灵法师");
    CHECK(nebula_role_team(nebula_role_id_at(0)) >= 0, "阵营查询可用");

    /* 6) 可选：传入真实插件路径做实测（zip 或文件夹） */
    if (argc > 1) {
        const char *p = argv[1];
        struct stat st2;
        int rc;
        if (stat(p, &st2) == 0 && S_ISDIR(st2.st_mode)) rc = nebula_addon_load_dir(p);
        else                                             rc = nebula_addon_load_zip(p);
        printf("\n--- 真实插件实测: %s ---\n", p);
        printf("  加载返回: %d (0=成功)\n", rc);
        /* 该汉化包里确实存在这个键 */
        printf("  title.buttons.nebula = [%s]\n", nebula_lang_get("title.buttons.nebula"));
        CHECK(rc == 0, "真实插件加载成功");
        CHECK(strcmp(nebula_lang_get("title.buttons.nebula"), "星云舰") == 0,
              "真实插件词条生效（title.buttons.nebula=星云舰）");
    }

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
