# 移植技术方案（分阶段）

> 核心判断：**“重写”和“自建框架”是唯一通路。** 直接把星云舰 C# DLL 搬上 iOS 不可行（见 README 的“为什么必须 C++ 重写”）。

---

## 阶段零：认清现状（已确认）

- BepInEx 不支持 iOS / IL2CPP ARM。
- 星云舰 = C# 程序集，靠 BepInEx + HarmonyX + **Reactor**（自定义网络）加载运行。
- Harmony 依赖 `Reflection.Emit`（JIT），iOS AOT 下不可用 → **逻辑必须 native 重写**。
- 用户环境已具备：脱壳 IPA、无越狱注入能力、JIT、PAC 由 Frida 处理。

## 阶段一：准备与工具链

- **源码获取（重要更正）**：`Dolly1016/Nebula` 公开仓库**不含 C# 源码**，只含 `Lang.dat / epoch.dat / builtIn` 等发布数据包。真正的协议真相在 **Reactor** 框架（开源）。玩法逻辑需从 PC 安装后的 DLL（或社区反编译）整理，或基于公开 wiki 的行为描述重建。
- **iOS 逆向工具**：
  - `frida` + `frida-il2cpp-bridge`：运行时 dump IL2CPP 类/方法/字段，定位 Hook 点（见 `src/frida/dump.js`）。
  - 签名/注入工具：`insert_dylib` / `optool` + `zsign` / 全能签 / Sideloadly（用户自行处理签名）。

## 阶段二：自建 iOS 模组加载器（dylib）

替代 BepInEx 的简易框架：

1. **Dylib 入口**：`__attribute__((constructor))` 在 dyld 加载阶段执行（`src/dylib/loader.c` 已实现空壳）。
2. **IL2CPP 桥接**：用 frida-il2cpp-bridge 获取 `Il2CppDomain` 与 `Assembly-CSharp` 镜像，作为与游戏 C# 对象交互的入口。
3. **定位目标方法**：dump 出需要 Hook 的 C# 方法原生地址（如 `PlayerControl.FixedUpdate`）。
4. **Native Hook**：**用 Frida Interceptor**（自动处理 W^X 切换与 PAC），不要手搓 inline hook。

## 阶段三：重写星云舰逻辑（C# → native）

- 按功能清单把玩法逻辑（角色、修饰、模式、胜负）用 C/C++ 重写，通过 IL2CPP API 操作原生对象字段/方法。
- 在 Hook 到的原版方法（如 `GameManager.Update`）中插入重写逻辑。
- 中文语言包等附加内容：读本地配置文件加载翻译文本。

## 阶段四：注入、签名与测试

- 编译 ARM64 dylib → `insert_dylib` 注入脱壳 IPA → 重签 → 安装（见 `tools/inject.sh`）。
- 用私人服务器 / 小号测试，规避风险。通过日志（Frida / 自定义输出）定位崩溃。

---

## 联机策略（关键决策）

| 方案 | 难度 | 说明 |
|---|---|---|
| iOS ↔ iOS（本移植版互机） | ✅ 现实可行 | 协议自洽，只需两份构建一致；基础 Among Us 联机由原版游戏承载 |
| iOS ↔ PC 原版 | 🔴 极难 | 须字节级复刻 Reactor + 星云舰 RPC，且随 PC 更新漂移 |

**采用 iOS-to-iOS 优先**：先证明两份 iOS 构建能正确同步，再按需加 PC 协议适配器。

## 验收门禁

- 差分测试：把 iOS 客户端发出的自定义消息序列与一份参考运行对比（`docs/PROTOCOL_NOTES.md` + `src/net/` 后续补齐）。
- 两台上角色分配、技能、胜负判定一致，不踢人、不崩溃。

## 时间估算（单人、有逆向基础）

- 最小 PoC（工具链 + dump + 第一个 native Hook 改本地数值）：2–4 周
- 移植一小撮不联网本地功能：+2–4 个月
- 完整星云舰（全角色 + 特技 + 自洽联机）：6–12 个月全职，且需持续维护
