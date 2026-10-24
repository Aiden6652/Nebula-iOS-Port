# BepInEx / 星云舰 启动链路（iOS）

## 1. 入口不是猜的

PC 改装包里的 `doorstop_config.ini` 写死了：

```ini
[General]
target_assembly = BepInEx\core\BepInEx.Unity.IL2CPP.dll
# NOTE: The entrypoint must be of format `static void Doorstop.Entrypoint.Start()`
[Il2Cpp]
coreclr_path = dotnet\coreclr.dll
corlib_dir   = dotnet
```

所以整条链路是：

```
doorstop(winhttp.dll，Windows DLL 劫持)
   └─> 加载 BepInEx.Unity.IL2CPP.dll
         └─> 调用 static void Doorstop.Entrypoint.Start()
               └─> BepInEx 接管：
                     · 起 CLR（Windows 上是 CoreCLR）
                     · Il2CppInterop 绑定到游戏的 IL2CPP 域
                     · chainloader 扫描 BepInEx/plugins/
                           └─> NebulaLoader.dll
                                 └─> 取 BepInEx/nebula/Nebula.dll（星云舰本体）
```

**iOS 上的等价替换**：我们没有 `winhttp.dll` 那套 DLL 劫持，改成

```
你注入的 dylib
   └─> 我们的 libnebula_standard.dylib（单一编译器/JIT 版，constructor 异步 boot）
         └─> mono_jit_init() 起 MonoVM
               └─> 加载 BepInEx/core/BepInEx.Unity.IL2CPP.dll
                     └─> 调用 Doorstop.Entrypoint.Start()   ← 同一入口
```

入口点完全一样，**BepInEx 以下几层我们一行都不用重写**。

## 2. 运行时：为什么换成官方 MonoVM 而不是编 Mono 6.12

抽包实测各程序集的目标框架：

| 程序集 | 目标框架 |
|---|---|
| `BepInEx.Unity.IL2CPP.dll` | **.NETCoreApp 6.0** |
| `Il2CppInterop.Runtime.dll` | **.NETCoreApp 6.0** |
| `NebulaLoader.dll` | **.NETCoreApp 6.0** |
| `Nebula.dll` | **.NETCoreApp 6.0 / 8.0** |
| `BepInEx.Core.dll` | .NETStandard 2.0 |
| `0Harmony.dll` | .NETStandard 2.0 |

**Mono 6.12 只支持到 netstandard2.0**，加载 net6 程序集会直接崩。
最初"用 godot-mono-builds 从源码编 Mono 6.12"的方向因此是错的。

改用微软官方包 **`Microsoft.NETCore.App.Runtime.Mono.ios-arm64`（8.0.32）**——
iOS 上的 .NET 用的就是这套 MonoVM，天然支持 net6/net8：

| 内容 | 大小 |
|---|---|
| `native/libmonosgen-2.0.a` | 19.98 MB（静态） |
| `native/libmonosgen-2.0.dylib` | 5.23 MB（动态） |
| `native/include/mono-2.0/` | 88 个头文件 |
| `native/System.Private.CoreLib.dll` | 4.17 MB |
| `lib/net8.0/*.dll` | 完整 .NET 8 BCL |

收益：iOS 构建从"编译 15 分钟 + 一堆平台坑"变成"下载几十秒"。

## 3. 编译期踩到的坑（已修）

.NET 8 的 Mono 头文件重排过，`object.h` 不再间接包含 `class.h`，
且 `mono_domain_assembly_open` 在 `appdomain.h`：

```
error: call to undeclared function 'mono_domain_assembly_open'
error: call to undeclared function 'mono_class_from_name'
error: call to undeclared function 'mono_class_get_method_from_name'
```

补 `#include <mono/metadata/appdomain.h>` 与 `#include <mono/metadata/class.h>` 即可。

## 4. 环境变量

| 变量 | 作用 |
|---|---|
| `NEBULA_BOOT` | `bepinex` / `engine` / `auto`（默认 auto：有 BepInEx 就走它） |
| `NEBULA_GAME_DIR` | 游戏根目录（BepInEx 所在处），默认 dylib 同目录 |
| `NEBULA_PROFILE` | 已废除（解释器废弃，统一为编译器/JIT 版）|
| `NEBULA_HOST_CFG` | 直接指定配置文件路径（优先级最高） |

宿主会自动组装 `MONO_PATH` = BCL + `BepInEx/{core,interop,plugins,nebula,patchers}` + `NebulaLibs`，
让 Mono 能解析到程序集依赖。

## 5. 还没做的

- iOS 的 `global-metadata.dat` 与 PC 不一致（已比对过），`BepInEx/interop/` 里那套
  unhollowed 包装层**要用 iOS 的 metadata 重新生成**（Cpp2IL + Il2CppInterop.Generator）。
  直接用 PC 那份会因为 token 不同而在运行时炸。
- `BepInEx.cfg` 里 `UpdateInteropAssemblies` 在 iOS 上要关掉（没网也没必要自动生成）。
- 星云舰版本锁：`jp.dreamingpig.amongus.nebula.loader.cfg` 里的
  `SkipCheckingConsistency` / `IgnoringVersionConsistency` 视情况放宽。
