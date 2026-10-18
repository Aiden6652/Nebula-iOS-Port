/*
 * bridge.h — 无 Frida 的 IL2CPP 桥（dylib 自包含，零外部依赖）
 *
 * 为什么不再需要 Frida：
 *  - 之前所有"改游戏"的逻辑都写在 hook.js 里，必须由 Frida Gadget 加载才执行；
 *    用户没注入 Gadget → dylib 只打日志 → 游戏里什么都不会变。
 *  - 现在 dylib 自己用 dlsym 解析 libil2cpp 的导出 API，直接读/写游戏对象的
 *    【字段数据】，并可调用 il2cpp_runtime_invoke 执行原版方法。
 *
 * 为什么也不需要 JIT：
 *  - 全程只访问【数据内存】（对象+字段偏移处的读写）与【函数调用】，
 *    不写任何可执行跳板、不 patch 代码 → 不触发 W^X 限制。
 *
 * 结果：只注入这一个 dylib 即可生效（设备上有无 JIT 都不影响本模块）。
 */

#ifndef NEBULA_IL2CPP_BRIDGE_H
#define NEBULA_IL2CPP_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

/* 统一日志：同时写 syslog 与 $HOME/Documents/nebula_log.txt。
   开启 iTunes/Finder 文件共享后无需越狱即可在电脑上取该文件。 */
void nebula_log(const char *fmt, ...);

/* 运行时 ABI 探针：把所有镜像的 arm64 / arm64e(PAC) 打进日志（真机实测用） */
void nebula_il2cpp_list_images(void);

/* 初始化：解析 libil2cpp 导出符号。0=成功；-1=有符号缺失（会打印缺失清单）。 */
int  nebula_il2cpp_init(void);

/* 是否已就绪（游戏 IL2CPP 运行时已起来且类可解析） */
int  nebula_il2cpp_ready(void);

/* 自查 + dump：把感兴趣的类及其字段（名/类型/偏移）写到 path 文件。
   用于在你自己的设备上确认真实的类名与字段名。 */
int  nebula_il2cpp_dump_report(const char *path, const char **class_hints, int hint_n);

/* 按类名子串查找到类（跨所有 assembly image）。返回 Il2CppClass* 或 NULL */
void *nebula_il2cpp_find_class_by_hint(const char *substr);

/* 取类里的某个方法（MethodInfo*），用于指针替换型 hook。返回 NULL 表示未找到。 */
void *nebula_il2cpp_class_get_method(void *klass, const char *name);

/* 取某类的所有实例（走 UnityEngine.Object.FindObjectsOfType），返回实例数。
   out 数组容纳最多 max 个实例指针。 */
int  nebula_il2cpp_get_instances(void *klass, void **out, int max);

/* 字段读写：按字段名查找偏移后直接读写（纯数据操作）。 */
int  nebula_il2cpp_get_float(void *obj, const char *field, float *out);
int  nebula_il2cpp_set_float(void *obj, const char *field, float v);
int  nebula_il2cpp_get_int(void *obj, const char *field, int *out);
int  nebula_il2cpp_set_int(void *obj, const char *field, int v);

/* 启动后台轮询线程：按 interval_ms 周期执行 "第一个可见效果"（如加速）。
   返回 0 表示线程已启动。 */
int  nebula_il2cpp_start_poll(int interval_ms);

/* 第一个可见效果入口：给玩家加速。返回实际改过的实例数。 */
int  nebula_il2cpp_apply_speed(float multiplier);

#endif /* NEBULA_IL2CPP_BRIDGE_H */
