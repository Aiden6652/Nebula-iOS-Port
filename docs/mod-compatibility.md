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

## 7. [R83] blittable 补丁（⚠️ 根因判断错误，现仅为无害兜底，见 §8）

> **更正**：本节把崩溃归因于「P/Invoke 参数含 string/bool/LPUTF8Str 导致 marshal-ilgen 生成失败」，**这是错的**。R84 真机日志证明：`.cctor` 里 `il2cpp_domain_get()` 这种**零参数、纯 IntPtr** 的 P/Invoke 同样崩，且 `dobby` / CoreLib 的 P/Invoke 一直能正常跑。真正的根因是**构建脚本链错了 Mono 组件**（§8）。本节补丁对修崩溃无用，但已验证合法、无副作用，保留作兜底。

**当时的现象**：加载第一个用 Harmony 打游戏类型补丁的插件（AutoSave、AUI.ServerScroll 等）即 SIGSEGV，托管栈顶 `Il2CppInterop.Runtime.IL2CPP:.cctor`，native 栈 `stub_emit_marshal_ilgen → g_assert_not_reached() → abort()`。

**当时的（错误）修法**（`tools/patch_marshal_blittable/`）：把 7 个 core dll 里含 `string`/`bool` 的 P/Invoke 改成全 blittable（`string→IntPtr`、`bool→byte`）+ 同名原始签名托管 wrapper（`StringToHGlobalAnsi`/`PtrToStringAnsi`/`FreeHGlobal`，finally 释放），按对象引用相等重定向调用点。共 66 个方法，调用点零改动，幂等。

**验证**：7 个 dll 经 `ikdasm` 回读 + `AssemblyName.GetAssemblyName` 强校验合法，非 blittable 残留 0，全仓库其余 dll 亦 0 残留。

**结论**：R83 补丁**没能修好崩溃**（R84 真机仍崩在同一栈）。保留它只是因为它无害，且能顺带降低完整版 marshal-ilgen 的封送复杂度；`build_ios.sh` 里的幂等钩子仍在，但不再是修复手段。

## 8. [R84] 崩溃真根因：构建脚本链接了 marshal-ilgen 的 **stub 组件**

**现象**：R83 打过补丁的最新包，加载第一个插件（AutoSave）时仍 SIGSEGV，栈一字不差：

```
abort ← mono_assertion_message_unreachable ← stub_emit_marshal_ilgen
     ← emit_native_wrapper_ilgen ← mono_marshal_get_native_wrapper
     ← mono_jit_compile_method ← mono_magic_trampoline
Managed: Il2CppInterop.Runtime.IL2CPP:.cctor <0x000cb> ← EndGameManager:.cctor
       ← Il2CppClassPointerStore`1:.cctor ← Il2CppType:From
       ← HarmonySupportComponent:TryResolve ← Harmony.PatchAll ← AutoSave.Load
```

**根因（证据链）**：

1. 官方 .NET 8 iOS runtime nupkg 对同一组件提供**两份**：
   - `libmono-component-marshal-ilgen-static.a`（208 KB，`marshal-ilgen.c.o`，完整实现，含 `emit_native_wrapper` / `emit_marshal_string_ilgen` / `emit_marshal_boolean_ilgen`）
   - `libmono-component-marshal-ilgen-stub-static.a`（61 KB，`marshal-ilgen-stub.c.o`，内含字符串 `Cannot marshal nonblittlable types without marshal-ilgen.`）
   - **两者导出同名符号 `mono_component_marshal_ilgen_init`**。
2. `build_ios.sh` 用 `find "$MONO_PREFIX" -name "*.a"` 把两份**都**收进 `LIBS`；case 只给 debugger 做了「stub/full 二选一」，**漏了 marshal-ilgen**。静态库按命令行顺序先到先得 → stub 版胜出，注册的回调就是 `stub_emit_marshal_ilgen`（导出符号表实测：`_stub_emit_marshal_ilgen`），内部 `g_assert_not_reached()`。
3. 本宿主强制 `mono_aot_only=0` **纯 JIT**（官方 iOS 是 AOT，才用 stub）。纯 JIT 下**任何 P/Invoke 首次调用都要运行时生成 native wrapper IL** → `mono_marshal_get_native_wrapper` → stub → abort。
   - 所以 `dobby` / `libSystem.Native` 能跑（它们是 dllmap 到主 dylib 的 icall 静态链，或已在 AOT 侧解析），而 `Il2CppInterop.Runtime` 的 197 个 `pinvokeimpl("GameAssembly")` 一碰就炸。
   - `.cctor` 里 `il2cpp_domain_get()`、`il2cpp_domain_get_assemblies(native int, uint32&)`、`il2cpp_assembly_get_image(native int)` **全是纯 blittable**，照样崩 → 证明与参数类型无关，R83 的 blittable 补丁打偏了。

**修法**（`src/mono_host/build_ios.sh`，不动任何模组/dll 内容）：

| 位置 | 改动 |
|---|---|
| LIBS 收集 case | 新增 `libmono-component-marshal-ilgen-stub-static.a) continue`，只链 full |
| 清单强校验 | 遍历 LIBS，要求 `full=1 && stub=0`，否则 `exit 1` 拒绝产出 |
| 链接后终检 | `strings -a` 对最终 dylib 查 stub 独有字符串 `Cannot marshal nonblittlable types without marshal-ilgen`，命中即删产物并 `exit 1` |
| 版本标记 | `nebula_mono_host.c` 改为 R84，启动日志明示本次修复 |

> 为什么清单校验 + 产物校验两道：清单只能保证 `LIBS` 里没列 stub；`-force_load` 的其它归档或 `MONO_PREFIX` 下别处若又提供一份 stub，清单看不出来，但产物里必然留下那句错误文本。两道都是**构建期失败**，绝不让坏包流到真机。

**真机怎么验**：日志第 3 行应为 `[host][r84] ===== 版本标记 R84：...`（若为 r82/r83 说明跑的是旧包）。构建脚本输出应出现 `[R84] marshal-ilgen: 完整版已入选、stub 已排除` 和 `[R84] 终检通过`。之后 `Loading [AutoSave ...]` 不应再出现 `stub_emit_marshal_ilgen` / SIGSEGV。

**为什么之前几轮都没发现**：`build_ios.sh` 第 51-54 行注释早就写了「components.c 会引用 `marshal_ilgen_init`，少一个就 Undefined symbol」，作者据此把 `find` 到的全链上，却没意识到「同名符号、stub 优先」会静默替换实现——直到反汇编 `libmono-component-marshal-ilgen-stub-static.a` 看到 `stub_emit_marshal_ilgen` 与崩溃栈完全对上。

