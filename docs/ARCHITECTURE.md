# 架构设计（iOS-to-iOS 优先）

## 总览

```
┌─────────────────────────────────────────────┐
│  Among Us (iOS, IL2CPP, 脱壳)                │
│   ├─ 原版游戏逻辑（任务/击杀/投票/建房）← 不变 │
│   └─ 自定义消息通道（被我们 Hook 的收发点）   │
└───────────────┬─────────────────────────────┘
                │ Hook (Frida Interceptor, 处理 PAC)
┌───────────────┴─────────────────────────────┐
│  nebula_ios_loader.dylib (自建加载器)         │
│   ├─ loader.c      入口 / IL2CPP 桥接 / Hook 注册 │
│   ├─ src/net/      自洽网络层（iOS↔iOS）      │
│   └─ src/nebula/   玩法逻辑重写（C++）← 待铺   │
└───────────────────────────────────────────────┘
```

## 模块职责

### 1. 加载器（`src/dylib/`）
- dylib 随 IPA 通过 `LC_LOAD_DYLIB` 加载，`constructor` 在启动时执行。
- 通过 frida-il2cpp-bridge 拿到 `Il2CppDomain` / `Assembly-CSharp` 镜像。
- 用 **Frida Interceptor** 在目标原生地址打补丁（自动处理 W^X + PAC）。

### 2. 网络层（`src/net/`，当前为接口骨架）
- **自洽**：自定义 RPC ID / 序列化 / 分发由本仓库定义，两份 iOS 构建共用 → iPad↔iPad 一致。
- 不绑定 PC Reactor 协议；PC 兼容作为后续可选适配器。
- 底层：Hook 原版 Among Us 的网络收发点，把自定义消息挂在其消息通道上传输。

### 3. 玩法逻辑（`src/nebula/`，待建）
- 把星云舰角色 / 修饰 / 模式 / 胜负用 C++ 重写。
- 在 Hook 到的原版方法内调用，操作 IL2CPP 原生对象。

## Hook 点（待 dump 确认，示例）
- `PlayerControl.FixedUpdate` — 移动/技能逻辑
- `GameManager.Update` — 回合/胜负判定
- 原版网络 `Send/Receive` 点 — 承载自定义消息

## 为什么不手搓 inline hook
iOS ARM64 有 PAC（指针认证），重定向控制流需正确签名否则 SIGILL。Frida 的 `Interceptor` 已处理 W^X 切换与 PAC，自搓风险高、维护难。
