/*
 * addon.h — 插件系统 + 本地化（汉化）
 *
 * 目录：开启 iTunes/Finder 文件共享后可在电脑上直接访问
 *       <App Documents>/addons/
 *
 * 支持的插件形态：
 *   1) 数据插件 *.naddon / *.ini / *.dat / *.txt / *.lang
 *      纯文本 "键=值"（可带 [section]），用来改角色名 / 汉化 / 数值。
 *      ★ 星云舰官方的 Lang.dat 之类就是这一类 —— 可直接吃下做汉化。
 *   2) 原生插件 *.dylib
 *      dlopen 后调用导出符号 nebula_addon_entry（需 skip-library-validation）。
 *   3) *.zip
 *      会列出其内部条目名（仅解析中央目录，不解压），便于定位语言文件。
 *
 * 说明：原版星云舰的 C#/.NET 插件在 iOS 上无法执行（无 .NET 运行时 +
 *      Harmony 依赖 Reflection.Emit，而 iOS 是 AOT-only）。但其中的
 *      【语言/配置数据】部分完全可被本系统直接使用。
 */

#ifndef NEBULA_ADDON_H
#define NEBULA_ADDON_H

#include <stddef.h>

#define NEBULA_ADDON_DIR   "addons"
#define NEBULA_LANG_MAX    256
#define NEBULA_LANG_KEYSZ  64
#define NEBULA_LANG_VALSZ  128

/* 初始化：确保 addons 目录存在 → 写出内置中文语言包（便于用户自行改）→ 扫描加载插件
   base_dir 传 $HOME/Documents。返回已加载插件数，-1 表示目录不可用。 */
int  nebula_addon_init(const char *base_dir);

/* 首次运行把内置中文语言包落到 addons/lang_zh_CN.naddon（用户可在电脑上改） */
int  nebula_addon_emit_builtin_langpack(const char *base_dir);

/* 本地化 */
void nebula_lang_set(const char *key, const char *value);
const char *nebula_lang_get(const char *key);   /* 查不到返回 key 本身 */

/* 已加载插件 */
int  nebula_addon_count(void);
const char *nebula_addon_name(int idx);

/* 加载一个原版插件包：ZIP 形态 / 文件夹形态 / 原生 dylib
   （内部会读 addon.meta 取名称版本，并载入 Language/*.dat 词条） */
int  nebula_addon_load_zip(const char *zip_path);
int  nebula_addon_load_dir(const char *dir_path);

/* 列出 zip 内部条目并载入其中语言数据（调试用） */
int  nebula_zip_list(const char *zip_path);

#endif /* NEBULA_ADDON_H */
