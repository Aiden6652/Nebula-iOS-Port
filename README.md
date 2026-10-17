# Nebula-iOS-Port（星云舰 iOS 原生移植）

把 PC 版 **星云舰（Nebula on the Ship）** 这个 BepInEx/Harmony 模组，以 **“重写 + 注入”** 方式移植到 iOS 端 *Among Us*（IL2CPP / ARM64）。

> ⚠️ 本项目为**技术逆向 / 学习用途**。与 Innersloth 无任何关联，不修改官方服务器，仅做本地客户端研究。请遵守你所在地区的法律法规与游戏服务条款。

> 🚀 **只想快点跑起来？** 看 [`QUICKSTART.md`](QUICKSTART.md)（三步拿到方法地址清单，命令都在电脑上敲）。

## 目标

- **第一里程碑（当前主线）：iPad ↔ iPad 自洽联机**
  两端都是本移植版，协议格式由本仓库定义，只要两份构建彼此一致即可，**不绑定 PC 原版协议、不追 PC 版本**。
- 远期（可选）：在自洽网络层之上加一层 **Reactor 协议适配器**，使其也能加入 PC 原版房间。iOS-to-iOS 是其严格子集。

## 为什么必须“C++ 重写”而不是搬 DLL

1. **BepInEx 不支持 iOS**：官方对 IL2CPP 的平台支持里，ARM 设备（含 iOS）为“不支持”。
2. **Harmony 在 iOS 上无法工作**：星云舰依赖 HarmonyX 用 `System.Reflection.Emit` 运行时织入补丁；iOS 是 **AOT-only、无 JIT**，运行时代码生成不可用 → 真·C# 模组在 iOS 上跑不起来。
3. 结论：逻辑必须手动翻译成 **native（C / ObjC / C++）**，并自建一套加载 + Hook 框架替代 BepInEx。

## 已确认可行的基建前提（用户环境已具备）

| 条件 | 说明 |
|---|---|
| 脱壳 IPA | FairPlay 加密已去除，可分析/注入 |
| 无越狱注入 | 离线改 `LC_LOAD_DYLIB` + 重签即可，沙盒不挡 |
| JIT 已开 | 解决 W^X：可用 `MAP_JIT` + `pthread_jit_write_protect_np` 写后执行 trampoline |
| PAC | 重定向控制流需指针认证；**用 Frida 的 Interceptor 处理，不自搓 inline hook** |

## 仓库结构

```
docs/                 技术方案 / 架构 / 协议笔记
src/dylib/            自建 iOS 模组加载器（可编译的 arm64 dylib 空壳）
src/net/              自洽网络层接口（iOS↔iOS，不绑定 PC 协议）
src/frida/            frida-il2cpp-bridge dump 脚本（定位 Hook 点）
tools/                insert_dylib + 重签 示意脚本
.github/workflows/    GitHub Actions：macOS 交叉编译 ARM64 dylib
```

## 构建

- **GitHub Actions（推荐）**：推送即触发，`macos-latest` 用 Xcode 的 iPhoneOS SDK 交叉编译 `nebula_ios_loader.dylib`，产物作为 artifact 上传。
- **本地**：`cd src/dylib && make`（需 macOS + Xcode 命令行工具）。

## 现状

🚧 **骨架阶段**：加载器空壳 + 网络层接口 + 协议分析占位已就位。**尚未包含任何星云舰玩法逻辑**，也未完成真实 Hook。下一步见 `docs/PORTING_PLAN.md` 的阶段拆分。
