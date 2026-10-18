/*
 * hook.h — IL2CPP「指针替换型」Hook：无 Frida、无 JIT、不改代码段
 *
 * 原理：IL2CPP 的 MethodInfo 结构体第 0 个成员就是 methodPointer（该函数
 *       的真正入口）。它被放在【可写的数据区】里，所以直接把这个指针换成
 *       我们自己的 C 函数即可完成重定向：
 *         - 不修改 __TEXT 代码段 → 不需要 code-sign 绕过
 *         - 不需要分配可执行跳板 → 不需要 JIT
 *         - 不需要 Frida Gadget → 不需要额外注入
 *
 * 我们的替换函数编在 dylib 自身里，dyld 加载时早已签名可执行，
 * 因此是"纯数据写 + 普通 C 函数调用"，零 iOS 内存保护红线。
 *
 * 局限（诚实说明）：只对「经 MethodInfo 派发」的调用生效；
 * 若某个方法被编译期直接调用（绕过 MethodInfo），需退回第 3 级（JIT trampoline）。
 * 是否生效看日志里的帧计数即可判定。
 */

#ifndef NEBULA_IL2CPP_HOOK_H
#define NEBULA_IL2CPP_HOOK_H

#include <stdint.h>

/* IL2CPP 方法原生签名：(this, MethodInfo*) -> 返回值 */
typedef void *(*nebula_method_fn_t)(void *instance, void *method);

/* 安装 hook：把 class_hint 类的 method_name 方法的入口替换成 replacement。
   成功返回 0，并把原入口写回 *out_orig（可传 NULL）。
   失败返回 -1（类未找到 / 方法未找到 / 入口为空）。 */
int nebula_hook_install(const char *class_hint, const char *method_name,
                        nebula_method_fn_t replacement,
                        nebula_method_fn_t *out_orig);

/* 查询某个已安装 hook 的原入口（用于在自定义逻辑后继续调用原函数） */
nebula_method_fn_t nebula_hook_orig(const char *class_hint, const char *method_name);

/* 注册每帧回调（由 Update hook 在每帧调用） */
void nebula_hook_set_tick(void (*tick_cb)(void));

/* 熔断开关：IPA 可执行文件旁放 nebula_no_hook.txt 即停用 hook（退回纯轮询）。
   返回 1=可安装，0=已停用。 */
int  nebula_hook_enabled(void);

/* 安装 GameManager.Update 的每帧 hook（每帧调 tick 回调，再调原函数） */
int  nebula_hook_install_game_update(void);

int  nebula_hook_count(void);

#endif /* NEBULA_IL2CPP_HOOK_H */
