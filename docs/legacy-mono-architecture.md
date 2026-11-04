# Legacy Mono 分支架构说明（适配老版纯 Mono AU + 老 Mono 模组）

## 为什么要这个分支
`main` 分支（R60）是 **IL2CPP 版**：针对新 AU（Unity IL2CPP 后端），靠 Il2CppInterop
把 NOS 桥接进原生游戏。老版纯 Mono AU（~2021 之前）游戏逻辑是**真实托管 IL**，
走 BepInEx5（Unity Mono 版）才是正路——`typeof` / Harmony 直接打真类型，无需代理层。

两个分支**互不影响**：`main` 保留 R60 IL2CPP 成品，`legacy-mono` 只服务老 AU。

## 核心架构决策（为什么不"寄生游戏自带的 libmono"）
用户最初设想：`dlopen` 游戏自带 `libmono`，补 JIT。实测不可行：
1. Unity iOS 的 Mono 后端是把运行时**静态链进 AU 主二进制**，没有独立 `libmono.dylib`，
   只能从已加载镜像 `dlsym` 抠符号，谈不上"拿到 libmono"。
2. 游戏那个 AOT Mono 二进制里 `mini`（JIT 编译器）被整段裁掉——它肚子里没有能吐
   机器码的东西，APRR 只解决 W^X 页权限，**不提供编译器**，没法"强挂 JIT"。
3. 同进程再塞一颗 JIT Mono = 双 Mono 灾难（两个 GC / 两套 `typeof` / 两个 AppDomain）。

**采用方案**：复用 R60 已验证的嵌入式 **mono-ios-arm64**（自带 JIT + APRR 闸门），
加载 **BepInEx5 Unity Mono core**，由它的 `Preloader.Main(string[])` 把游戏 managed
入口拉起来。单运行时，干净。

## 与 IL2CPP 版(host)的差异（已用 `#ifndef LEGACY_MONO` 关掉不适用段）
| 项 | IL2CPP 版(main) | Legacy Mono 版 |
|---|---|---|
| 入口 | `Doorstop.Entrypoint.Start()` | `BepInEx.Preloader.Preloader.Main(string[])` |
| 游戏代码 | native(UnityFramework) + Il2CppInterop 桥接 | 真实托管 IL（直接 patch） |
| 内嵌资源 | BepInEx6 core + 预生成 interop(131) + unity-libs(66) | BepInEx5 core + 游戏 managed |
| R56 手动触发 | 用（IL2CPPChainloader.NebulaManualExecute） | **禁用**（BepInEx5 自带加载时机） |
| 启动释放 | interop/ + native/ | game-managed/ + native/ |

## 宿主关键改动（`src/mono_host_mono/nebula_mono_legacy.c`）
- `boot_bepinex()`：入口换成 `BepInEx.Preloader` 类、`Main` 方法（1 个 `string[]` 参数）；
  `mono_runtime_invoke` 前用 `nebula_mono_make_entry_args()` 构造长度 1 的托管 `string[]`，
  `args[0]` = 游戏托管入口路径（默认 `$(root)/game-managed/Assembly-CSharp.dll`，
  可用 `NEBULA_GAME_ENTRY` 覆盖）。
- `nebula_r60_extract_bundle()`：新增 `#ifdef LEGACY_MONO` 段，把 `game-managed/` 释放到磁盘。
- R56 的 IL2CPP 手动触发整段用 `#ifndef LEGACY_MONO` 包裹，Mono 下不编译、不调用。
- 其余（blob 内存加载、JIT 闸门、`dllmap`、boot overlay 3s 品牌页、preloader 日志转发、
  看门狗、单 dylib 静态链 dobby + CoreLib shim）**全部复用，未重写**。

## 构建 / 打包 / CI
- `src/mono_host_mono/build_ios_mono.sh`：`build_ios.sh` 分叉，blob 段换成 BepInEx5 core +
  游戏 managed，编译加 `-DLEGACY_MONO`，仍 `force_load` dobby + 5 个 libSystem + ICU + GSS。
- `.github/workflows/build-ios-mono.yml`：复用同一个 `mono-ios` 编译 job，宿主步骤改调
  `build_ios_mono.sh`，产物 `nebula-mono-host-ios-legacy`。
- `scripts/package_ios_mono.sh`：去掉 IL2CPP 专属的 interop / ServerScroll 步骤，NOS 改装包
  参数可选，单 dylib 校验保留。

## 需要你提供的素材（构建前放入对应目录）
1. `src/mono_host_mono/bepinex-mono-core/`：BepInEx5 Unity Mono 版的 `BepInEx/core/*.dll`
   （含 `BepInEx.Preloader.dll`、`BepInEx.Core.dll`、`BepInEx.Unity.Mono.dll` 及依赖）。
   来自 NOS 老 Mono 构建包，或 BepInEx 官方 `BepInEx_5.x.x_UnityMono.zip`。
2. `src/mono_host_mono/game-managed/`：**从老 AU Mono 版 .ipa 的 `Data/Managed/` 提取**的
   全部 `*.dll`（Assembly-CSharp.dll + UnityEngine.* 等）。本目录 `.gitignore` 忽略，
   按设备/版本提供。

> 两个目录都缺也能编译（宿主会告警），适合先做"BepInEx5 能否在 iOS 上起域"的冒烟测试。

## 设备验证清单（明天测）
1. 启动出现 3 秒 `AUI Mod Loader by Aiden` 品牌页。
2. host 日志有 `[host][legacy-mono] 游戏 managed -> ...`、`BepInEx5 入口` 等行。
3. BepInEx5 成功起域、加载插件（LogOutput.log 无致命异常）。
4. 游戏入口（Assembly-CSharp.dll）被正确加载，老 AU 进游戏、NOS Mono 版加载成功。
5. 无 `DllNotFoundException`、无 SIGSEGV。
6. 若游戏入口路径不同，用 `NEBULA_GAME_ENTRY` 指到正确的 `Assembly-CSharp.dll`。
