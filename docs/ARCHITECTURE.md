# 架构设计（iOS-to-iOS 优先）

## 核心边界（已与用户确认）

> **玩法逻辑【全部纯本地】**：角色/技能/会议/投票/胜负，每台设备用相同规则**独立重算**，**不碰网络**。
> **联机【只广播动作】**：网络层只把"谁对谁用了什么技能/投了谁"这类动作发给对端，两端各自本地重算 → 状态自然收敛。

这套边界让联机兼容从"字节级复刻 PC 协议"降级成"两份构建互相对比"，难度骤降。

## 总览

```
┌──────────────────────────────────────────────────┐
│  Among Us (iOS, IL2CPP, 脱壳)                      │
│   ├─ 原版逻辑（任务/击杀/投票/建房）← 不变         │
│   └─ 被 Hook 的网络收发点 / 输入按钮               │
└───────────────┬──────────────────────────────────┘
                │ Hook (Frida Interceptor, 自动处理 PAC)
┌───────────────┴──────────────────────────────────┐
│  nebula_ios_loader.dylib (自建加载器)              │
│   ├─ loader.c          入口 / 网络+内核接线        │
│   ├─ src/core/   ★纯本地玩法引擎（角色/技能/会议/胜负/修饰）│
│   └─ src/net/    自洽网络层（iOS↔iOS，只广播动作）  │
└───────────────────────────────────────────────────┘
        ▲ Frida JS（src/frida/hook.js）按名解析 IL2CPP 方法并桥接 C
```

## 模块职责

### 1. 加载器（`src/dylib/loader.c`）
- dylib 随 IPA 经 `LC_LOAD_DYLIB` 加载，`constructor` 启动即执行。
- 已接入：本地内核初始化、本地动作→广播回调、对端动作→本地重算、RPC 注册。
- 导出 `nebula_get_game()` 供 Frida/JS 取本地游戏实例指针。
- IL2CPP 桥接 / 真实 Hook 注册为占位（待真机 dump 地址后填 `src/frida/hook.js`）。

### 2. 纯本地玩法引擎（`src/core/`）★已完成可测
- 角色：船员 / 内鬼 / 警长 / 工程师 / 小丑（框架稳定，后续扩）。
- 技能：击杀 / 通风管 / 会议 / 投票 / 任务 / 破坏。
- 会议-投票-驱逐、胜负判定（含中立小丑）、修饰（Auxiliary，位掩码，如 BAIT/加速）。
- `nebula_core_set_action_cb`：本地发起的动作→广播。
- **host 测试 `test_core.c` 全过**（gcc）。

### 3. 自洽网络层（`src/net/`）★已完成可测
- 线格式 + 5 类 RPC（握手/角色/技能/胜负/状态）+ 收包分发。
- 联机时只广播动作；对端收到→本地重算。
- **host 测试 `test_protocol.c`/`test_dispatch.c` 全过**。

### 4. Hook 管理器（`src/frida/hook.js`）★已完成结构，待真机地址
- 配置驱动解析 IL2CPP 方法（抗 AU 更新：只改 `config.example.json`/dump 产出）。
- 桥接 dylib 导出的 C 函数（`nebula_core_*` / `nebula_net_*`）。
- 接管网络收发、击杀/会议输入、主循环驱动。
- 一律用 Frida Interceptor（自动处理 W^X + PAC），不自搓 inline hook。

## 验证手段（不依赖真机即可证明架构成立）
- `tools/sim/protocol_sim.py`：双客户端联机状态收敛。
- `tools/sim/core_sim.py`：纯本地逻辑 + 动作广播端到端。
- `tools/sim/full_game_sim.py`：完整对局（击杀→会议→投票→驱逐→判胜负）两端收敛。
- CI（`.github/workflows/build.yml`）：Linux 跑上述全部 + C 测试；macOS 交叉编译 iOS dylib。

## Hook 点（逻辑名，真实地址由 `dump.js` → `config` 补全）
- `GameManager.Update` — 主循环/兜底胜负
- `PlayerControl.FixedUpdate` — 移动/技能
- 网络 `Send`/`Receive` 点 — 承载自定义消息
- `KillButton.OnClick` / `EmergencyButton.OnClick` — 击杀/会议输入

## 为什么不手搓 inline hook
iOS ARM64 有 PAC（指针认证），重定向控制流需正确签名否则 SIGILL。Frida 的 `Interceptor` 已处理 W^X 切换与 PAC，自搓风险高、维护难。
