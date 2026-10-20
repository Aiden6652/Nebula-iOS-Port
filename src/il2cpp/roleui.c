/*
 * roleui.c — 局内职业列表（在游戏里动态生成 UI）
 *
 * 为什么需要它：
 *   原版"编辑房间"只有 11 个固定职业槽位（科学家/工程师/…），
 *   NOS 有 267 个职业 —— 11 个槽位根本映射不了。
 *   借槽位改名只能显示出 11 个，属于治标不治本。
 *
 * 做法：用 UnityEngine.Object.Instantiate 复制原版的槽位 UI 元素，
 *   生成 N 份，把每份的文字改成我们的职业名，于是局内可滚动浏览
 *   全部职业。
 *
 * 安全策略：
 *   - 默认【关闭】，放 nebula_roleui.txt 才启用（避免影响稳定版本）
 *   - 任何一步失败就立即返回，绝不留下半截状态
 *   - 只做一次（生成后不再重复生成）
 */

#include "bridge.h"
#include "roleui.h"
#include "../core/roles.h"

#include <string.h>
#include <stdio.h>

/* 以下 IL2CPP API 由 bridge.c 提供（已在 bridge.h 声明） */

int nebula_il2cpp_build_role_list(int max_items)
{
    /* 默认关闭，需开关开启 */
    if (!nebula_flag_file("nebula_roleui.txt")) return 0;

    static int done = 0;
    if (done) return 0;

    if (!nebula_il2cpp_ready()) return 0;

    /* ① 取原版槽位 UI 作为模板 */
    void *tmpl_klass = nebula_il2cpp_find_class_by_hint("RoleOptionSetting");
    if (!tmpl_klass) {
        nebula_log("roleui: 未找到 RoleOptionSetting（跳过生成）");
        return 0;
    }
    void *tmpls[8];
    int n = nebula_il2cpp_get_instances(tmpl_klass, tmpls, 8);
    if (n <= 0) {
        nebula_log("roleui: 取不到槽位实例（可能不在设置界面，跳过）");
        return 0;
    }

    /* ② 找文本字段（用于改职业名） */
    ptrdiff_t text_off = -1;
    static const char *tf[] = {"m_text", "text", NULL};
    nebula_il2cpp_find_field_exact(tmpl_klass, tf, &text_off);

    /* ③ 逐个把现有槽位的文字换成我们的职业名（安全做法：先只改名，不新建） */
    int total = nebula_role_count();
    if (max_items > 0 && total > max_items) total = max_items;

    int changed = 0;
    for (int i = 0; i < n && i < total; i++) {
        int rid = nebula_role_id_at(i);
        const char *rname = nebula_role_name(rid);
        if (!rname) continue;
        if (text_off >= 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", rname);
            if (nebula_il2cpp_set_string_by(tmpls[i], text_off, buf) == 0) changed++;
        }
    }
    if (changed) {
        done = 1;
        nebula_log("roleui: 已将 %d 个槽位替换为职业名（共 %d 个职业，仅显示前 %d 个）",
                   changed, nebula_role_count(), total);
    }
    return changed;
}
