# 自洽网络层（iOS ↔ iOS）

这是"在 iPad 上玩星云舰、并能和另一台 iPad 联机"的**联机地基**，不依赖 PC 协议。

## 文件

| 文件 | 作用 |
|---|---|
| `protocol.h` / `protocol.c` | 线格式 + RPC 打包/解包（C 端，给 dylib 用） |
| `dispatch.c` | 收包解码 → 路由到已注册的 RPC 处理器；发包经 transport 出门 |
| `net_layer.h` | dylib 调用入口：`nebula_net_register` / `nebula_net_on_recv` / `nebula_net_send` |
| `test_protocol.c` / `test_dispatch.c` | host 端测试（gcc 跑，抓回归） |
| `Makefile` | `make test` 编译并运行上述测试 |

Python 参考实现（与 C 字节级一致，用于无真机验证）：
`../../tools/sim/protocol_sim.py` — 跑一段双客户端联机模拟，断言两端状态收敛。

## 线格式

```
+--------+--------+--------+--------+----------+
| magic  | ver    | rpc_id | plen   | payload  |
| 2B 'NB'| 1B     | 2B u16 | 2B u16 | plen B   |
+--------+--------+--------+--------+----------+
```

## RPC 清单（自洽，从 0x5000 起）

- `0x5000 RPC_HELLO` 握手（携带协议 epoch）
- `0x5001 RPC_ROLE_ASSIGN` 角色分配 `net_id(u32) + role_id(u16)`
- `0x5002 RPC_ABILITY_USE` 技能释放 `caster(u32)+ability(u16)+target(u32)+x(f32)+y(f32)`
- `0x5003 RPC_WIN_CHECK` 胜负判定 `winner_team(u8)`
- `0x5004 RPC_STATE_SYNC` 通用状态同步 `net_id(u32)+key(u8)+val(i32)`

## 在 dylib 里怎么用

```c
#include "net_layer.h"
nebula_net_set_transport(原版发送点封装);   // 初始化时接入
nebula_net_register(RPC_ROLE_ASSIGN, 处理器); // 为每类消息注册
// 游戏 recv 钩子里：
nebula_net_on_recv(收到的字节, 长度);        // 自动解码并分发
```

## 本地验证

```bash
cd src/net && make test      # gcc 跑协议 + 分发测试
python3 ../../tools/sim/protocol_sim.py   # 双客户端联机模拟
```
