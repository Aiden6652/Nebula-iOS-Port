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

## 9. [R85] NOS 没起来：`kernel32.dll` 的 Windows 增量 GC 干预 → 宿主层 no-op

**现象**（R84 真机包，游戏已正常跑进主界面、UnityIAP/存档都在动，说明 §8 的 marshal-ilgen 修复已生效）：

```
[Info   :   BepInEx] Loading [NebulaLoader 1.0.0]
[Error  :   BepInEx] Error loading [NebulaLoader 1.0.0]: System.Reflection.TargetInvocationException
 ---> System.DllNotFoundException: kernel32.dll
   at Nebula.Utilities.IncrementalGcInvalidator.DiscoverFlagAddress()
   at Nebula.Utilities.IncrementalGcInvalidator.TryDisable()
   at Nebula.NebulaPlugin.LoadInternal(Boolean android)
   at NebulaLoader.NebulaLoader.TryLoadNebula(String dllFilePath)
```

`NebulaLoader` 已找到并加载 33MB 的 `Nebula.dll`，但 `LoadInternal` 第一步就抛异常，BepInEx 记为 Error 并中断该插件，**NOS 本体没起来**。

**根因**（查 NOS 上游源码 `Dolly1016/Nebula-Public` 坐实，非猜测）：

- `IncrementalGcInvalidator.cs` 用 `[DllImport("kernel32.dll")] GetModuleHandleW` + `GetProcAddress` 定位 `GameAssembly.dll` 里 il2cpp 增量 GC 的 flag 并写 0，目的是**保性能**。这是纯 Windows PE 逻辑，iOS 是 Mach-O、无 `GameAssembly.dll` 模块，本就不该成功。
- 宿主层**早已用 `MONO_GC_PARAMS=major=marksweep`（非增量 marksweep）关掉增量 GC**（启动日志实锤），这一步在 iOS 上完全多余。
- 关键：调用点 `Nebula.cs:145` 是 `if (!android) IncrementalGcInvalidator.TryDisable();` —— **返回值被忽略**，且源码自带安全分支：

```csharp
DiscoverFlagAddress() {
    nint h = GetModuleHandleW("GameAssembly.dll");
    if (h == 0) return 0;              // ★ 只要返回 0，整条链安全退出
    ...
}
TryDisable() {
    nint flagAddr = DiscoverFlagAddress();
    if (flagAddr == 0) return false;   // ★ 直接返回，不读不写任何内存
    ... *p = 0; ...                    // 只有 Windows 才会走到
}
```

所以只要让 `GetModuleHandleW` 返回 0，NOS 自己就会静默跳过，连 `IsIncrementalNative()`（`DllImport("GameAssembly")`）都不会碰。

**修法**（`src/mono_host/nebula_mono_host.c` + `build_ios.sh`）——**模组 DLL 一字节不动、不做 IL 补丁、不预加载、BepInEx 照常原生加载**：

1. 宿主导出 6 个 `visibility("default")` 的恒返回 0 stub：`GetModuleHandleW/A/(无后缀)/WW`、`GetProcAddress/A`（覆盖 Mono 对 `CharSet` + `ExactSpelling` 的符号名探测差异，避免 `EntryPointNotFoundException`）。
2. `nebula_install_win32_gc_noop_dllmap()` 把 `kernel32.dll` 家族（含大小写/`lib` 变体）用 `mono_dllmap_insert` 映射到主 dylib 自身。挂在与 `libSystem.*` / `nebula_jit` 同批的初始化阶段（早于 `mono_jit_init`、早于 `Start()/Execute()` 扫插件）。**纯 C 返回 0，无 detour / 无 JIT trampoline**，不像 dobby 必须延后到装插件阶段。
3. `build_ios.sh` 的 `export_list.txt` 追加这 6 个 `_` 前缀符号（ld64 一旦用 `-exported_symbols_list` 就只导出清单内符号），并新增构建期 `nm` 强校验：任一 stub 未导出即 `exit 1` 拒绝产出。GC 完全归宿主层管理 —— 即「GC 只允许在宿主层里干」。

> 兼容意义：不止 NOS。任何模组里 Windows 专属、iOS 上多余的 `GetModuleHandle*` / `GetProcAddress` P/Invoke 都一并兜住，不再因 `DllNotFoundException` 中断加载链 —— 这正是「兼容大部分模组」的方向。

**沙箱验证**：`bash -n` 通过；抽出新增 C 块用 gcc `-Wall -Wextra` 桩编译 0 error，6 个 stub 全部为全局 `T` 符号；全量扫 `/tmp/monoios` 所有 `.a`，无裸同名强符号冲突（只有带 `SystemNative_` 前缀的，不撞）；定义顺序（2824 定义 / 2890 调用）与 `mono_dllmap_insert(NULL, 名, NULL, self, NULL)` 参数均与现有 dobby/SystemNative 调用一致。iOS 全量构建需 macOS+Xcode，真机待复测。

**真机怎么验**：日志第 3 行应为 `版本标记 R85`；搜 `[host][r85] kernel32.dll -> 宿主 no-op stub` 确认 dllmap 已挂；`Loading [NebulaLoader 1.0.0]` 之后**不再有** `DllNotFoundException: kernel32.dll`，随后应出现 NOS 自身的启动日志。

> 本轮范围仅限「把 NOS 跑通」。同日志里 AutoSave / ServerScroll 的 `TypeLoadException: System.Runtime.InteropServices.Marshal ... System.Runtime 6.0.0.0`、GamePatch 的 `FormatException`、ImmersionAudioBridge 的 `InjectorHelpers` 是各模组**各自独立**的问题（BCL 版本对齐 / 版本号解析 / class injection），不是 NOS 加载链的一部分，未在本轮处理。


## 10. [R86] 路径基准全面改到可写 Documents：NOS 往只读 `.app/NebulaLibs` 建目录被拒

**现象（R85 真机日志）**：R85 的 kernel32 no-op 生效了（日志第 49 行，不再是
`DllNotFoundException: kernel32.dll`），`NebulaLoader` 已从
`Documents/BepInEx/nebula/Nebula.dll`（33,928,192 B，日志 998）读到程序集，
CWD 也在 Documents（617）——但 `LoadInternal` 仍中断：

```
Error loading [NebulaLoader 1.0.0]: System.Reflection.TargetInvocationException
 ---> System.UnauthorizedAccessException: Access to the path
      '/private/var/containers/Bundle/Application/.../AmongUs.app/NebulaLibs' is denied.
   at System.IO.Directory.CreateDirectory(String path)
   at Nebula.NebulaPlugin.<LoadInternal>g__LoadLibrary|30_2(Stream, String)
   at Nebula.NebulaPlugin.<LoadInternal>g__LoadLibraryFromZip|30_1(String)
   at Nebula.NebulaPlugin.LoadInternal(Boolean android)
```

**根因（查上游 `Dolly1016/Nebula-Public` 源码坐实，非猜测）**：
- `NebulaPluginNova/Utilities/PathHelpers.cs`：
  `NebulaLibsPath => PathHelpers.GameRootPath + "/NebulaLibs"`，其中 PC 编译版
  `GameRootPath => BepInEx.Paths.GameRootPath`。
- `LoadLibrary()` 对 `dirPath = PathHelpers.NebulaLibsPath` 调
  `Directory.CreateDirectory(dirPath)` + `File.WriteAllBytes(...)`，把内嵌
  `Nebula.Resources.Libs.zip` 里的依赖 dll 释放到磁盘。
- 宿主此前把 `DOORSTOP_PROCESS_PATH` 设成 `root/AmongUs`，导致
  `BepInEx.Paths.GameRootPath = 只读 .app`（日志 701）。于是 NOS 往只读签名
  bundle 建目录 → `UnauthorizedAccessException`。**模组文件明明都在 Documents，
  却因这个根指错而去 .app 创建。**

**修法（用户明确要求：模组 DLL 一字节不动、BepInEx 原生加载，只把宿主层
所有"指向 .app 的目录"改成 Documents，除游戏本体只读资源外）**：

统一用 `nebula_doc_root()` = `HOME/Documents` 作为唯一可写基准：

| 类别 | 落点 | 处理 |
|---|---|---|
| `GameRootPath`（`DOORSTOP_PROCESS_PATH`） | **Documents** | 改为 `Documents/AmongUs`；NOS 的 `NebulaLibsPath` 随之变 `Documents/NebulaLibs`（可写） |
| `BepInExRootPath`（`DOORSTOP_INVOKE_DLL_PATH`） | Documents | 保持 `Documents/BepInEx` |
| `MONO_PATH` 6 个目录 | **Documents** | 由 `.app/BepInEx/*`、` .app/NebulaLibs` 改为 `Documents/...`，与 NOS 释放位置同址 |
| interop / native / Cecil refs / plugins 释放 | **Documents** | 删除往 `.app` 投的那一份（R85 日志第 30 行"新写 0"实锤是无效功） |
| BepInEx.cfg 写入（GlobalMetadataPath / UnityBaseLibrariesSource / ScanMethodRefs / UpdateInteropAssemblies） | **Documents** | 由遍历 `bases`(含 .app) 收敛为单写 Documents；R85 日志 605~614 生效的本就是 Documents 那份 |
| unity-libs / interop 兜底释放 | **Documents** | `wbase[1] = { Documents }`，保留 `for`+`continue` 结构 |
| CWD | **Documents** | 删掉 `NEBULA_CWD=root` 诊断开关与 `chdir(root)` 兜底，恒为 Documents |
| preloader 失败标志 / LogOutput / diag / 日志 / Mods / BepInEx 入口文件兜底 / 版本检测 | **Documents** | 全部由 `root` 改 `nebula_doc_root()` |
| **游戏本体只读资源（保留 .app）** | `.app` | `nebula_fix_il2cpp_paths` 的三个探测 base（`app`/`root`/`doc`）用于找 UnityFramework、`global-metadata.dat`、`Data/`；`BEPINEX_GAME_ASSEMBLY_PATH`、cfg 里的 `GlobalMetadataPath` 仍是 `.app` 绝对路径；`module_dir`/宿主 dylib 自身不变 |

**`Documents/Data -> .app/Data` 符号链接**：`GameRootPath` 改到 Documents 后，
BepInEx 的 `Paths.SetExecutablePath` 会把 `GameDataPath` 算成 `Documents/Data`、
`ManagedPath` 算成 `Documents/Data/Managed`。真实 Unity 数据在只读 `.app/Data`，
故用 `symlink` 把两者接上（幂等：目标已存在则不动，绝不覆盖用户内容），
保证 `Directory.Exists(GameDataPath)` 仍为真、游戏本体继续从 .app 读。
这是"本该指向 .app 的只读资源"的例外，用链接而非复制，逻辑上仍归 .app。

**为什么这样能成**：PC 版 NOS 的 `NebulaLibsPath` 完全由 `BepInEx.Paths.GameRootPath`
派生；把它从只读 `.app` 换成可写 Documents，`CreateDirectory` / `WriteAllBytes` /
`Assembly.LoadFrom` 整条释放链路自然落到 Documents，且 `MONO_PATH` 已包含同址的
`Documents/NebulaLibs`，依赖 dll 可被解析。**零 IL 补丁、零预加载、模组 DLL
一字节未改、BepInEx 照常原生加载**，符合用户一贯要求。

**沙箱验证**：全文件大括号/圆括号配平（470/470、2318/2318）；抽出
`nebula_doc_root`/`setup_mono_path`/`nebula_r60_extract_bundle`/
`nebula_init_bepinex_paths`/`nebula_detect_bepinex`/`nebula_preloader_failed`/
`boot_mods_dir` 七个改动函数 + `nebula_fix_il2cpp_paths` 全函数（187 行），
配最小桩用 gcc `-fsyntax-only -Wall -Wextra` 编译 **0 error**；删除变量在函数体内
无残留引用；R86 行引号成对。全量复核剩余 `root` 仅两处正当用途：
`bases[nb++]=root`（只读探测游戏本体）、`target = root/Data`（符号链接源）。
iOS 全量构建需 macOS+Xcode，真机待复测。

**真机怎么验**：日志第 3 行应为 `版本标记 R86`；
`[host][r86] BepInEx 环境就绪: ... GameRootPath=<...>/Documents(Documents,可写)`；
`[host][r86] CWD -> <...>/Documents`；`[host][r86] 预生成 interop` 等释放路径
全部是 Documents、不再出现 `.app/BepInEx/interop`；
`Loading [NebulaLoader 1.0.0]` 之后**不再有** `UnauthorizedAccessException:
.../AmongUs.app/NebulaLibs`，随后应出现 NOS 自身启动日志与
`[host][r86] Data 链接: ... -> .../AmongUs.app/Data`。

> AutoSave / ServerScroll 的 `Marshal` TypeLoad、GamePatch 的 `FormatException`、
> ImmersionAudioBridge 的 `InjectorHelpers`、`dobby` DllNotFound 均属各模组/组件
> 独立问题，不在本轮"NOS 加载链 + 路径基准"范围内，未处理。
