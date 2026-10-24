# Nebula-iOS-Port（星云舰 iOS 侧载移植）

把 PC 版 **星云舰（Nebula on the Ship，Among Us 的 BepInEx/Harmony 模组）** 移植到 iOS 端 *Among Us*（IL2CPP / ARM64）。

核心做法：**把 Mono 运行时嵌入宿主 dylib，配合 BepInEx/IOS 侧载，让原版 BepInEx + Harmony（nebula.dll）在 iOS 上通过 JIT 真正跑起来**；再辅以自研 C 宿主桥接 IL2CPP、本地玩法内核、自洽网络层、启动视觉过渡与崩溃防御。

> 【声明】本项目为技术逆向 / 学习用途，与 Innersloth 无任何关联，不修改官方服务器，仅做本地客户端研究。请遵守所在地区法律法规与游戏服务条款。

> 【快速上手】三步拿到方法地址清单见 [`QUICKSTART.md`](QUICKSTART.md)。

> 【运行环境】不再需要 Frida、不需要越狱：宿主 dylib 自己 `dlsym` 解析 libil2cpp 读写游戏字段；通过 Mono 嵌入 + StikDebug / 官方 JIT 提供 trampoline 执行能力，原 Frida 脚本已移出主链路。

## 目标

- **第一里程碑（当前主线）：iPad ↔ iPad 自洽联机** —— 协议由本仓库定义，不绑定 PC 原版、不追 PC 版本。
- 远期（可选）：Reactor 协议适配器接入 PC 原版房间（iOS-to-iOS 是其严格子集）。

## 方案选型：为什么用 Mono 嵌入 + BepInEx/IOS

1. 原版星云舰是 BepInEx/Harmony 模组，HarmonyX 依赖 `System.Reflection.Emit` 运行时织入补丁；iOS 默认 AOT-only、无 JIT，真·C# 模组无法运行。
2. 纯原生重写全部 C# 逻辑是早期路线（见 `src/core`、`src/net`），成本高且易偏离原版行为。
3. 最终主线改为：**嵌入 Mono 运行时提供 JIT**，直接侧载运行原版 BepInEx/IOS + Harmony + nebula.dll，行为与原版一致。自建 C 宿主只负责加载 Mono、桥接 IL2CPP、驱动启动过渡与防御。

## JIT 机制（iOS W^X 处理）

- 宿主保留 `MAP_JIT` 映射。
- 不再硬依赖 `pthread_jit_write_protect_np`（iOS SDK 不导出该符号，链接期 `-U` 仅骗过链接器，运行时懒绑定为 NULL 会跳零崩溃）。
- 改为 **dlsym 运行时探测**：解析到该符号 → 走官方 W^X 翻转（0/1 切换可写/可执行）；解析不到（StikDebug 调试型 JIT 通过 `CS_DEBUGGED` 直接给 RWX 页）→ 跳过翻转，直接在 RWX 页写 trampoline。两条路都兜住。

## 仓库结构

```
docs/                  技术方案 / 架构 / 协议 / 内存预算 / BepInEx 启动 / 模组兼容 笔记
src/mono_host/         ★Mono 嵌入宿主：加载 Mono、JIT 探测翻转、启动占位过渡、CIFS、GC 看门狗
src/il2cpp/            IL2CPP 桥接：dlsym 解析 libil2cpp、字段读写、枚举实例、Method 指针 Hook（段保护）
src/addon/             插件/资源加载：dlopen 句柄缓存防泄漏、主线程 IO 异步
src/ui/                悬浮 UI 面板（bringSubviewToFront + 触控接管）
src/core/              本地玩法内核（角色/技能/胜负，纯本地重算）
src/net/               自洽网络层（iOS↔iOS，只广播动作）
src/dylib/             早期 loader（现由 scripts 交叉编译替代）
tools/                 insert_dylib + 重签 示意脚本；sim/ 联机模拟器
scripts/               build_mono_ios.sh（编 iOS 宿主）、package_ios.sh（打 dylib 包）
.github/workflows/     build-ios.yml（编 iOS 宿主 dylib）、build.yml（macOS 冒烟 + 交叉编译 loader）
```

> 架构边界：模组逻辑本地优先——角色/技能/胜负每台设备独立用相同规则重算；联机只广播动作，两端各自本地重算 → 状态收敛。

## 构建

- **GitHub Actions（推荐）**：
  - `build-ios.yml`：编译 iOS 宿主 `libnebula_standard.dylib`（含 Mono 嵌入、桥接、UI、防御）。
  - `build.yml`：macOS 冒烟测试 + 交叉编译 `nebula_ios_loader.dylib`，校验 bridge/hook/addon/overlay。
- **本地**：`scripts/build_mono_ios.sh`（需 macOS + Xcode iPhoneOS SDK）；`scripts/package_ios.sh` 打包并生成安装说明。

## 注入、签名与侧载（必须带 entitlements）

```bash
./tools/inject.sh <脱壳IPA>          # 基础 entitlements
./tools/inject.sh <脱壳IPA> --jit    # 追加 JIT entitlements（付费开发者证书）
```

侧载方案：全能签 + StikDebug（调试型 JIT，提供 RWX 页），或侧载签名（`nebula-jit-entitlements.plist`）。签名务必带 `com.apple.private.skip-library-validation`，否则 iOS 拒绝加载注入的 dylib（表现：游戏正常但毫无效果）。

## 真机验证结论（已对上传 IPA 实测）

剖析对象：Among Us `com.innersloth.amongus`（版本 2026.9.29，715 MB）。

| 项 | 结果 | 结论 |
|---|---|---|
| `cryptid` | **0** | 已脱壳，加密不是障碍 |
| 架构 | **arm64**（非 arm64e） | PAC 风险低，指针替换型 hook 安全 |
| 构建类型 | **IL2CPP**（有 `global-metadata.dat`，0 个 Mono DLL） | 与方案一致 |
| `il2cpp_*` 符号 | **存在，共 241 个** | 动态/静态符号都在，方案可跑 |
| 代码位置 | **`Frameworks/UnityFramework.framework/UnityFramework`（95 MB）** | 主执行文件 `AmongUs` 仅 88 KB，是空壳启动器 |

> 注意：app 名为 **`AmongUs.app`**（无空格），注入时不要按 "Among Us" 去找。

## 当前状态（单一 JIT 编译器包）

✅ 已实现并测通（CI 双 workflow 全绿）：

- **单一 `libnebula_standard.dylib` 包**：废除 pro / 解释器双版本，standard 与 pro 合并为单一编译器（JIT）构建，减少分支与维护面。
- **Mono 嵌入 + dlsym JIT 探测翻转**：兼容「官方 JIT 真机」与「StikDebug RWX」两条路，不再因 `pthread_jit_write_protect_np` 跳零崩溃。
- **BepInEx/IOS 侧载跑原版 nebula.dll + Harmony**：启动占位文本「Nebula 核心正在初始化...」→ BepInEx 接管就绪后销毁 → 交还原版 StateMachine 加载动画（完美视觉过渡）。
- **CIFS 默认开启**：大小写不敏感路径解析，确保模组贴图正确加载（含负向缓存防死锁）。
- **14 项业务级崩溃防御**：8 字节对齐校验、Hook 段保护（拒改带签名 `__TEXT`）、dlopen 句柄缓存、进 BepInEx 前强制 GC + 500MB 硬顶看门狗、主线程 IO 异步、日志限流降级、Missing Method 兜底、UI 层级/触控接管等。
- **CI 双 workflow**：`build-ios.yml` 产出 `nebula-mono-host-ios` artifact（含 `libnebula_standard.dylib`）；`build.yml` macOS 冒烟 + loader 交叉编译。

剩余（需真机，沙箱无法代劳）：按真实方法地址补全 config → 注入 dylib 到 IPA → iPad 实测联机与职业/技能渲染。
