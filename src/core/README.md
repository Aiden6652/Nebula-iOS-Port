# 纯本地玩法内核（src/core）

**架构边界（用户确认）**：模组逻辑【全部纯本地】——角色/技能/胜负每台设备独立用相同规则重算，**不碰网络**。联机时只由网络层广播"动作"，两端各自本地重算。

## 文件

| 文件 | 作用 |
|---|---|
| `core.h` / `core.c` | 游戏状态、角色、技能执行、胜负判定；`nebula_core_set_action_cb` 把"本地动作"接到网络层 |
| `test_core.c` | host 端测试（gcc 跑，抓回归） |
| `Makefile` | `make test` |

## 当前可玩集

### 角色
| 角色 | 阵营 | 机制 |
|---|---|---|
| `ROLE_CREW` 船员 | 船员 | 无主动技能 |
| `ROLE_IMPOSTOR` 内鬼 | 内鬼 | 击杀 / 破坏 / 通风管 |
| `ROLE_SHERIFF` 警长 | 船员 | 枪杀；误杀船员则自己死 |
| `ROLE_ENGINEER` 工程师 | 船员 | 可用通风管（更长冷却） |
| `ROLE_JESTER` 小丑 | 中立 | 被投票驱逐即胜 |
| `ROLE_SEER` 占卜师 | 船员 | 识破目标阵营 |
| `ROLE_MADMATE` 狂信者 | 船员表面 / 内鬼同胜 | 不计入船员数，随内鬼胜利 |
| `ROLE_BODYGUARD` 保镖 | 船员 | 给目标护盾，免疫一次致死 |
| `ROLE_SNIPER` 狙击手 | 内鬼 | 远程狙杀，**无反伤** |

### 技能（ABILITY_*，一个 RPC 承载全部动作）
击杀 / 通风管 / 发起会议 / 投票 / 完成任务 / 破坏 / 识破 / 护盾 / 狙杀。

### 胜负
内鬼（含狙击手）全灭→船员胜；内鬼数 ≥ 好人数→内鬼胜（狂信者随胜）；
小丑被驱逐→小丑胜。**会议期间不结算胜负**，待会议结束再判（符合原版节奏）。

### 修饰（Auxiliary）
位掩码：`MOD_BAIT`（被击杀触发会议）、`MOD_SPEED`、`MOD_GIANT`。

## 本地 / 网络 分界线

- `nebula_core_use_ability(..., local=1)`：本地发起 → 执行后触发 `action_cb` 去广播。
- `nebula_core_use_ability(..., local=0)`：来自网络 → 只本地重算，不二次广播（防回环）。

## 本地验证

```bash
cd src/core && make test
```

端到端（本地内核 + 网络动作广播）见 `../../tools/sim/core_sim.py`。
