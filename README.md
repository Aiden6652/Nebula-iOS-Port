# Nebula-iOS-Port（星云舰 iOS 原生移植）

把 PC 版 **星云舰（Nebula on the Ship）** 这个 BepInEx/Harmony 模组，以 **“重写 + 注入”** 方式移植到 iOS 端 *Among Us*（IL2CPP / ARM64）。

> ⚠️ 本项目为**技术逆向 / 学习用途**。与 Innersloth 无任何关联，不修改官方服务器，仅做本地客户端研究。请遵守你所在地区的法律法规与游戏服务条款。

> 🚀 **只想快点跑起来？** 看 [`QUICKSTART.md`](QUICKSTART.md)（三步拿到方法地址清单，命令都在电脑上敲）。

> ⚠️ **不再需要 Frida、不需要越狱、也不需要 JIT**：dylib 自己 `dlsym` 解析 libil2cpp，
> 只读/写游戏对象字段（纯数据操作），自带后台轮询线程驱动。
> 原 Frida 脚本已移出主链路到 `tools/frida-optional/`（仅作开发备用，不用管）。

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
src/dylib/            自建 iOS 模组加载器（可编译的 arm64 dylib，已接入网络层+本地内核）
src/core/             纯本地玩法内核（角色/技能/胜负，每台设备独立算，不碰网络）
src/net/              自洽网络层（iOS↔iOS，联机时只广播"动作"，两端各算各的）
src/il2cpp/      ★无 Frida 游戏桥：dlsym 解析 libil2cpp、读写字段、枚举实例、轮询生效
tools/frida-optional/ 早期 Frida 脚本（已移出主链路，不用管）
tools/                insert_dylib + 重签 示意脚本；sim/ 双客户端联机模拟器
.github/workflows/    GitHub Actions：Linux 跑测试 + macOS 交叉编译 ARM64 dylib
```

> **架构边界（已确认）**：模组逻辑【全部纯本地】——角色/技能/胜负每台设备独立用相同规则重算；**只有联机才走网络**，且网络层【只广播动作】（如"玩家X对Y用了技能"），两端各自本地重算 → 状态自然收敛。

## 构建

- **GitHub Actions（推荐）**：推送即触发，`macos-latest` 用 Xcode 的 iPhoneOS SDK 交叉编译 `nebula_ios_loader.dylib`，产物作为 artifact 上传。
- **本地**：`cd src/dylib && make`（需 macOS + Xcode 命令行工具）。

## 现状

✅ **本地玩法引擎 + 网络层 + Hook 管理器结构 已落地并测通**（CI 全绿）：
- `src/core`：**纯本地玩法引擎**已实现（船员/内鬼/警长/工程师/小丑 + 击杀/通风管/会议/投票/任务 + 胜负 + 修饰），host 测试全过。
- `src/net`：自洽协议 + 分发层已实现；三个 Python 模拟器验证「纯本地逻辑 + 动作广播」从单步到完整对局两端收敛。
- `src/il2cpp`：**无 Frida 游戏桥**（dlsym 解析 libil2cpp + 字段读写 + FindObjectsOfType 枚举实例 + 后台轮询），取代原 Frida 方案；host 降级测试全过。
- `src/dylib`：已接入网络层、本地内核与游戏桥；构造即跑「自查 dump + 加速效果」，日志走 syslog。
- **剩余**：等你的真机日志确认真实类名/字段名后，把「加速」换成真正的职业/技能渲染（UI 文本与角色显示）。
- **剩余（必须真机，沙箱无法代劳）**：用 `dump.js` 取 Among Us 真实方法地址填入 config → 注入 dylib 到 IPA → iPad 实测。代码已写好，只差真机跑通与按真机字段补全若干 TODO。
