# C# 模组兼容性：怎么让更多 AU 模组跑起来

## 1. 三类模组，三种放法

| 类型 | 放哪 | 谁来加载 |
|---|---|---|
| **BepInEx IL2CPP 插件**（最常见：星云舰、GamePatch 等）| `BepInEx/plugins/` | BepInEx 的 chainloader，**自动** |
| **MonoMod patcher**（要在 BepInEx 之前打补丁的）| `BepInEx/patchers/` | BepInEx 预加载器，**自动** |
| **普通 .NET 程序集**（不依赖 BepInEx 的）| `Mods/` | **我们的宿主**扫描并启动 |

因为宿主最终是把控制权交给 BepInEx（调 `Doorstop.Entrypoint.Start()`），
所以**凡是能在 PC 上跑的 BepInEx IL2CPP 插件，理论上放进来就能用**，不需要我们为它写任何适配代码。这是我们选择"跑原版 BepInEx"而不是自己重写逻辑的最大收益。

## 2. `Mods/` 目录：非 BepInEx 模组的通用加载

不是所有 C# 模组都走 BepInEx。对那些，宿主干这些事：

1. 扫 `Mods/*.dll`（可用 `NEBULA_MODS_DIR` 改目录）
2. 逐个 `mono_domain_assembly_open` 加载
3. 按顺序尝试常见入口，**命中任意一个就启动**：
   - `Doorstop.Entrypoint.Start()`
   - `Entrypoint.Start()`（无命名空间）
   - `Mod.Start()`
   - `<程序集同名类>.Start()`
   - `<程序集同名类>.Main()`
4. 都没命中也没关系——**照样加载进来了**，只是不主动启动。这样它的类型仍能被别的模组引用，或者靠 Harmony 补丁生效。

## 3. 启动顺序

`NEBULA_BOOT` 控制，默认 `auto`：

```
BepInEx (plugins/ 全量加载)
   ↓ 没找到 BepInEx
Mods/ 目录扫描
   ↓ 没东西
Engine.dll POC（仅桌面验证用）
```

可显式指定：`NEBULA_BOOT=bepinex|mods|engine`。

**为什么 BepInEx 优先**：它起来后会自己遍历 `plugins/`，覆盖面比我们一个个猜入口广得多。

## 4. 已知限制

| 限制 | 说明 |
|---|---|
| 目标框架 | 运行时是 **.NET 8 MonoVM**，能跑 net6/net8。面向 .NET Framework 4.x 的老模组不一定兼容 |
| interop | `BepInEx/interop/` **必须是 iOS 版重新生成的**。PC 那套 token 对不上会运行时崩 |
| 原生依赖 | 模组若带 Windows 的 `.dll`（P/Invoke 到原生库），iOS 上不可用，需要对应 iOS 静态库 |
| JIT | 用 Harmony 运行时打补丁的模组，在纯解释器模式下也能跑（解释器能执行动态 IL），只是慢 |
| 联网 | 星云舰加载器默认要联网拉最新版本，**要关掉**（见下） |

## 5. 加一个新模组的步骤

1. 把插件 dll 丢进 `BepInEx/plugins/`（普通模组丢 `Mods/`）
2. 它依赖的库丢 `NebulaLibs/` 或同目录
3. 星云舰附加组件（含职业）丢 `Addons/`——**默认全加载**
4. 重签名 + 注入，跑一次看 `NebulaLog.txt` / BepInEx 日志确认它被 chainloader 认出来了

## 6. 星云舰的两个配置坑

`BepInEx/config/jp.dreamingpig.amongus.nebula.loader.cfg`：

- `AutoUpdate` / `AutoUpdateIfVersionMismatching` → iOS 上建议都 **false**，别让它联网
- `SkipCheckingConsistency` / `IgnoringVersionConsistency` → 版本对不上时放宽（代价是可能踩到不兼容组合）

`BepInEx/config/BepInEx.cfg`：

- `UpdateInteropAssemblies` → iOS 上建议 **false**（没网也没必要，interop 我们离线生成好带过去）
