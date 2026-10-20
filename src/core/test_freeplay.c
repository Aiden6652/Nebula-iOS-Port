/*
 * test_freeplay.c — host 端验证自由模式（Solo Free Play）
 * 跑法：cd src/core && make test
 */

#include "freeplay.h"
#include "rolecfg.h"
#include "roles.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, m) do { if(!(c)){ printf("  [FAIL] %s\n", m); failures++; } \
                         else { printf("  [ok]   %s\n", m); } } while(0)

static void wr(const char *p, const char *c) {
    FILE *f = fopen(p, "w"); if (f) { fputs(c, f); fclose(f); }
}

int main(void) {
    printf("=== Nebula freeplay host test ===\n");
    const char *p = "/tmp/nebula_freeplay_test.txt";
    int ids[ROLECFG_MAX], n = 0;
    nebula_rolecfg_default(ids, ROLECFG_MAX, &n);

    /* ① 没有配置文件 → 不开启（不影响正常联机） */
    remove(p);
    nebula_freeplay_load(p);
    CHECK(!nebula_freeplay_enabled(), "无配置文件时自由模式关闭");
    CHECK(nebula_freeplay_fill() == 0, "未开启时不补齐人数");
    CHECK(nebula_freeplay_role(0, ids, n) == ids[0], "未开启时按清单顺序分配");

    /* ② 空文件 → 开启，用默认值（补齐10人 + 20秒轮换） */
    wr(p, "# 自由模式\n");
    nebula_freeplay_load(p);
    CHECK(nebula_freeplay_enabled(), "空配置也开启自由模式");
    CHECK(nebula_freeplay_fill() == FP_FILL_DEFAULT, "默认补齐到 10 人");
    CHECK(nebula_freeplay_state()->rotate == FP_ROTATE_DEFAULT, "默认 20 秒轮换");
    CHECK(nebula_freeplay_role(0, ids, n) == ids[0], "轮换从清单第 1 个开始");

    /* ③ 轮换：时间没到不换，到了就换下一个 */
    long t0 = (long)nebula_freeplay_state()->last_switch;
    CHECK(nebula_freeplay_tick(n, t0 + 5) == 0, "间隔未到不切换");
    CHECK(nebula_freeplay_tick(n, t0 + 19) == 0, "差 1 秒也不切换");
    CHECK(nebula_freeplay_tick(n, t0 + 21) == 1, "到 20 秒切换一次");
    CHECK(nebula_freeplay_role(0, ids, n) == ids[1], "切换后拿到清单第 2 个职业");
    CHECK(nebula_freeplay_tick(n, t0 + 25) == 0, "刚切换完不会连跳");
    CHECK(nebula_freeplay_tick(n, t0 + 42) == 1, "再过 20 秒又切一次");
    CHECK(nebula_freeplay_role(0, ids, n) == ids[2], "第 3 个职业");

    /* ④ 轮换会回到开头（循环试用） */
    for (int i = 3; i < n; i++) nebula_freeplay_tick(n, t0 + 42 + (long)i * 21);
    nebula_freeplay_tick(n, t0 + 42 + (long)n * 21 + 1);
    CHECK(nebula_freeplay_role(0, ids, n) == ids[0], "走完一圈回到第 1 个职业");

    /* ⑤ 进度文本 */
    char prog[32] = {0};
    nebula_freeplay_tick(n, 0);   /* 让 total 记下来 */
    CHECK(nebula_freeplay_progress(prog, sizeof(prog)) == 1, "轮换时有进度文本");
    CHECK(strchr(prog, '/') != NULL, "进度形如 3/267");

    /* ⑥ 固定职业 → 不轮换 */
    wr(p, "role sheriff\n");
    nebula_freeplay_load(p);
    CHECK(nebula_freeplay_enabled(), "role 配置也开启");
    CHECK(nebula_freeplay_state()->role == nebula_role_id_by_key("sheriff"), "固定职业=警长");
    CHECK(nebula_freeplay_state()->rotate == 0, "固定职业时不轮换");
    CHECK(nebula_freeplay_role(0, ids, n) == nebula_role_id_by_key("sheriff"), "自己拿到警长");
    CHECK(nebula_freeplay_tick(n, 0) == 0, "固定职业时 tick 不切换");

    /* ⑦ 未知职业 key 不崩，退回轮换 */
    wr(p, "role no_such_role\n");
    nebula_freeplay_load(p);
    CHECK(nebula_freeplay_state()->role == FP_ROLE_NONE, "未知 key 退回轮换");

    /* ⑧ 补齐人数与轮换间隔可配，且有上限保护 */
    wr(p, "fill 0\nrotate 30\n");
    nebula_freeplay_load(p);
    CHECK(nebula_freeplay_fill() == 0, "fill 0 = 不补齐");
    CHECK(nebula_freeplay_state()->rotate == 30, "轮换间隔可配为 30 秒");

    wr(p, "fill 999\nrotate 99999\n");
    nebula_freeplay_load(p);
    CHECK(nebula_freeplay_fill() == FP_FILL_MAX, "人数上限被夹住（防填爆房间）");
    CHECK(nebula_freeplay_state()->rotate == FP_ROTATE_MAX, "间隔上限被夹住");

    /* ⑧b 首次生成的配置示例默认是【关闭】的（不能一上来就改用户的联机局） */
    remove(p);
    nebula_freeplay_emit_sample(p);
    nebula_freeplay_load(p);
    CHECK(!nebula_freeplay_enabled(), "自动生成的配置示例默认关闭");

    /* ⑨ 显式关闭 */
    wr(p, "off\n");
    nebula_freeplay_load(p);
    CHECK(!nebula_freeplay_enabled(), "写 off 可关闭自由模式");

    /* ⑩ 悬浮按钮手动切换：立即换下一个，且不会立刻被自动轮换再次跳过 */
    wr(p, "rotate 20\n");
    nebula_freeplay_load(p);
    CHECK(nebula_freeplay_role(0, ids, n) == ids[0], "按钮切换前是第 1 个");
    nebula_freeplay_next(n);
    CHECK(nebula_freeplay_role(0, ids, n) == ids[1], "点一次按钮 → 第 2 个职业");
    nebula_freeplay_next(n);
    CHECK(nebula_freeplay_role(0, ids, n) == ids[2], "再点一次 → 第 3 个职业");
    CHECK(nebula_freeplay_tick(n, (long)nebula_freeplay_state()->last_switch) == 0,
          "刚手动切换完，自动轮换不会紧接着又跳");

    /* 固定职业时按钮不切换（用户盯着一个职业测） */
    wr(p, "role sheriff\n");
    nebula_freeplay_load(p);
    int fr = nebula_freeplay_role(0, ids, n);
    nebula_freeplay_next(n);
    CHECK(nebula_freeplay_role(0, ids, n) == fr, "固定职业时按钮不切换");

    printf("\n%s (%d failures)\n", failures==0 ? "ALL PASS" : "HAS FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
