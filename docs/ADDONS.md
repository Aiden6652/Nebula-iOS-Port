# 原版插件（Addon）支持

## 结论：数据包可以吃下，代码包不行

用真实插件 `SCLangPack（基础内容汉化）.zip` 实测确认，星云舰插件是**数据/资源包**：

```
SCLangPack（基础内容汉化）/          ← 一个文件夹，或整个打成 ZIP
├── addon.meta                       JSON 元数据
├── icon.png
├── Language/
│   └── SChinese.dat                 词条文件（110 KB）
└── MoreCosmic/                      外观资源（帽子/面饰）
    ├── Contents.json
    ├── hats/...
    └── visors/...
```

| 插件内容 | iOS 上能否用 | 说明 |
|---|---|---|
| `Language/*.dat` 词条 | ✅ **完全可用** | 已实现读取 |
| `addon.meta` 元数据 | ✅ **完全可用** | 已实现（取 Name / Version）|
| `MoreCosmic/` 外观 | ⚠️ 资源可读 | 注入进游戏外观系统待做 |
| 含 C# 代码的部分 | ❌ 不可执行 | 无 .NET 运行时 + Harmony 需 `Reflection.Emit`，iOS 是 AOT-only |

## 用法：原样丢进去，不用解压

开启 iTunes/Finder 文件共享后，把插件**整个文件夹或 ZIP** 丢进：

```
<App Documents>/addons/
```

dylib 启动时会自动扫描并加载，**ZIP 不需要解压**（内部直接内存 inflate 读取）。

支持三种形态：

| 形态 | 识别方式 | 处理 |
|---|---|---|
| **文件夹** | 目录 | 读 `addon/meta` 与 `Language/*.dat` |
| **ZIP 包** | `*.zip` | 解析中央目录 + inflate，不落盘 |
| **原生插件** | `*.dylib` | `dlopen` 后调用 `nebula_addon_entry` |

优先级：**内置中文兜底 → 用户插件覆盖**（用户插件永远赢）。

## 词条文件格式（原版 Language/*.dat）

```
# 注释

"title.buttons.nebula" : "星云舰"
"title.label.discord"  : "点击加入Discord!"
```

- UTF-8、CRLF 换行
- `"key" : "value"`，`#` 为注释
- 读取器同时兼容简易写法 `key = value`（便于手写 `.naddon`）

## addon.meta 格式

```json
{
  "Id" : null,
  "Name" : "简体中文汉化包",
  "Author" : "小巾&天寸",
  "Description" : "对星云舰进行汉化",
  "Version" : "3.0.2.1"
}
```

读取器只取 `Name` / `Version`（极简字符串提取，不引整个 JSON 库）。

## 本地验证

```bash
cd src/addon && make test                      # 内置用例
./test_addon <真实插件.zip>                     # 用真实插件实测
./test_addon <真实插件文件夹>                   # 文件夹形态
```

实测结果（`SCLangPack`）：ZIP 与文件夹两种形态均加载成功，
`title.buttons.nebula` = `星云舰`。
