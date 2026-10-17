# 纯本地玩法内核（src/core）

**架构边界（用户确认）**：模组逻辑【全部纯本地】——角色/技能/胜负每台设备独立用相同规则重算，**不碰网络**。联机时只由网络层广播"动作"，两端各自本地重算。

## 文件

| 文件 | 作用 |
|---|---|
| `core.h` / `core.c` | 游戏状态、角色、技能执行、胜负判定；`nebula_core_set_action_cb` 把"本地动作"接到网络层 |
| `test_core.c` | host 端测试（gcc 跑，抓回归） |
| `Makefile` | `make test` |

## 当前最小可玩集

- 角色：`ROLE_CREW` 船员 / `ROLE_IMPOSTOR` 内鬼 / `ROLE_SHERIFF` 警长
- 技能：`ABILITY_KILL` ——
  - 内鬼击杀：目标死亡
  - 警长枪内鬼：内鬼死亡；误杀船员：**警长自己死**
  - 船员：无击杀能力，忽略
- 胜负：内鬼全灭→船员胜；内鬼数 ≥ 好人数→内鬼胜

## 本地 / 网络 分界线

- `nebula_core_use_ability(..., local=1)`：本地发起 → 执行后触发 `action_cb` 去广播。
- `nebula_core_use_ability(..., local=0)`：来自网络 → 只本地重算，不二次广播（防回环）。

## 本地验证

```bash
cd src/core && make test
```

端到端（本地内核 + 网络动作广播）见 `../../tools/sim/core_sim.py`。
