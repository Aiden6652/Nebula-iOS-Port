# C# 模组兼容性：怎么让更多 AU 模组跑起来

## 1. 三类模组，三种放法

| 类型 | 放哪 | 谁来加载 |
|---|---|---|
| **BepInEx IL2CPP 插件**（最常见：星云舰、GamePatch 等）| `BepInEx/plugins/` | BepInEx 的 chainloader，**自动** |
| **MonoMod patcher**（要在 BepInEx 之前打补丁的）| `BepInEx/patchers/` | BepInEx 预加载器，**自动** |
| **普通 .NET 程序集**（不依赖 BepInEx 的）| `Mods/` | **我们的宿主**扫描并启动 |

因为宿主最终是把控制权交给 BepInEx（调 `Doorstop.Entrypoint.Start()`），
所以**凡是能在 PC 上跑的 BepInEx IL2CPP 插件，理论上放进来就能用**，不需要我们为它写任何适配代码。这是我们选择"跑原版 BepInEx"而不是自己重写逻辑的最大收益。

## 1.5 版本检测门（iOS 只跑得动 BepInEx 6）

宿主在进 `boot_bepinex` 之前，先用 `nebula_detect_bepinex()` 辨认模组自带 `BepInEx/` 文件夹的版本，再做分流：

| 检测结果 | 处理 |
|---|---|
| **BepInEx 6**（指纹：`BepInEx.Unity.IL2CPP.dll` + `Il2CppInterop.Runtime.dll`） | 走适配路径，再交由 Mono 虚拟机实测能否运行 |
| **非 6**（BepInEx 5 / Unhollower 系：`BepInEx.IL2CPP.dll` / `UnhollowerBaseLib.dll`） | **不硬加载**，直接回退原版游戏（iOS IL2CPP 无对应实现，硬加载必崩） |
| **未检测到 BepInEx 文件夹** | 回退原版游戏 |

判定优先级：`root/BepInEx/core`（模组合进 app 根）> `HOME/Documents/BepInEx/core`（用户丢进 Documents 的 / 宿主释放的）> 宿主 blob 内置的 v6 入口（NOS 默认路径）。

> 结论：**BepInEx 版本不用我们挑，模组包自带的文件夹说什么就是什么**。BepInEx 5 只支持纯 Mono，iOS 全是 IL2CPP，所以只有 v6 能跑；其余一律原版兜底，保证游戏不崩。
> 日志里搜 `[host][compat]` 可看到实际判定结果与版本号（用于核对"和 NOS 齐平"）。

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
| JIT | 用 Harmony 运行时打补丁的模组，在统一编译器（JIT）版下能跑（解释器已废除，始终走 JIT）|
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

## 7. [R83] marshal-ilgen 崩溃根因与补丁（修好一切 Harmony 打 IL2CPP 类型的模组）

**现象**：加载第一个用 Harmony 打游戏类型补丁的插件（AutoSave、AUI.ServerScroll 等）即 SIGSEGV，托管栈顶是 `Il2CppInterop.Runtime.IL2CPP:.cctor`，native 栈 `stub_emit_marshal_ilgen → g_assert_not_reached() → abort()`。C# 的 try/catch 接不住（这是 native `abort()`）。

**根因**：这套 iOS 裁剪版 MonoVM 的 **marshal-ilgen 后端被 stub 掉**（`build_ios.sh` 早有记录 "Cannot marshal nonblittlable types without marshal-ilgen"）。`Il2CppInterop.Runtime.dll` 里 il2cpp 桥接的 P/Invoke 大量用 `string`/`bool` 等非 blittable 类型（含 `UnmanagedType.LPUTF8Str`），**首次 JIT 生成 native wrapper 即 abort**——与具体是 LPUTF8Str 还是 LPStr 无关，单纯改枚举没用。BepInEx.Core / Unity.IL2CPP / Preloader.Core / MonoMod.Utils / Capstone / AsmResolver 里也有少量同类 P/Invoke，同样会崩。

> 注意：这不是 NOS 的问题。任何用 Harmony 碰 IL2CPP 类型的插件都会触发，所以"修一个模组"不如"修桥"。

**修法**（`tools/patch_marshal_blittable/`）：对每个含 `string`/`bool` 的 P/Invoke 方法 M——
1. 把 M 改成全 blittable（`string→IntPtr`、`bool→byte`），清掉 MarshalInfo，保留 `PInvokeInfo`（仍是同一个 il2cpp 导出，ABI 不变）；
2. 把改写后的 M 改名 `_Raw`，新建一个**同名、原始签名**的托管 wrapper W（用 `Marshal.StringToHGlobalAnsi`/`PtrToStringAnsi`/`FreeHGlobal` 做转换 + finally 释放）；
3. 按**对象引用相等**把同模块内所有调用点重定向到 W（Cecil 按名字+参数解析会忽略返回类型造成歧义，必须按引用改）。

调用点零改动，131 个 interop 程序集、HarmonySupport、NOS 全部不受影响。构建时由 `build_ios.sh` 幂等调用（已打过的 dll 会因 `Needs()=0` 自动跳过）。

**验证**：7 个 core dll 经 `ikdasm` 反汇编回读 + `AssemblyName.GetAssemblyName` 强校验全部合法，非 blittable 残留 = 0；`GetErrorMessage` 等调用点已正确绑到 string wrapper。

**真机怎么验**：日志里搜 `[r83]`。若仍有插件加载即崩，先看崩点是不是别的 P/Invoke（理论上已全覆盖 core，但 interop/插件自带的非 blittable P/Invoke 若被调用也可能触发，届时把对应 dll 加进补丁清单即可）。

