#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
core_sim.py — 「纯本地逻辑 + 网络只同步动作」端到端验证

架构（用户确认）：
  - 玩法逻辑【全部纯本地】：每台设备独立用相同规则重算。
  - 网络层【只广播动作】(ABILITY_USE)，两端各自本地重算 → 自然收敛。

本脚本把 src/core 的规则用 Python 镜像，并接上 protocol_sim 的网络层，
模拟两台 iPad：A 本地开枪 → 广播动作 → B 本地重算 → 断言两端状态一致。

跑法：python3 tools/sim/core_sim.py
"""

import sys
from protocol_sim import (encode_msg, decode_msg, RPC_ABILITY_USE,
                          RPC_HELLO, pack_ability, unpack_ability,
                          pack_hello, EPOCH, ABILITY_KILL, Hub)

# ---------------- 本地内核（镜像 src/core/core.c） ----------------
ROLE_CREW, ROLE_IMPOSTOR, ROLE_SHERIFF = 0, 1, 2


class Core:
    def __init__(self):
        self.players = {}   # net_id -> dict(role, alive)
        self.over = False
        self.winner = 0

    def add_player(self, net_id, role):
        self.players[net_id] = {"role": role, "alive": True}

    def apply_ability(self, caster, ability, target):
        if self.over:
            return
        c = self.players.get(caster)
        t = self.players.get(target)
        if not c or not t or not c["alive"] or not t["alive"]:
            return
        if ability != ABILITY_KILL:
            return
        if c["role"] == ROLE_IMPOSTOR:
            t["alive"] = False
        elif c["role"] == ROLE_SHERIFF:
            if t["role"] == ROLE_IMPOSTOR:
                t["alive"] = False
            else:
                c["alive"] = False     # 误杀船员，自己死
        # 船员无击杀能力 → 忽略
        self.check_win()

    def check_win(self):
        if self.over:
            return
        alive_imp = sum(1 for p in self.players.values()
                        if p["alive"] and p["role"] == ROLE_IMPOSTOR)
        alive_other = sum(1 for p in self.players.values()
                          if p["alive"] and p["role"] != ROLE_IMPOSTOR)
        if alive_imp == 0:
            self.over, self.winner = True, 1
        elif alive_imp >= alive_other:
            self.over, self.winner = True, 2

    def snapshot(self):
        return {n: (d["role"], d["alive"]) for n, d in sorted(self.players.items())}, \
               (self.over, self.winner)

    def __eq__(self, other):
        return self.snapshot() == other.snapshot()


# ---------------- 客户端（接网络层） ----------------
class Client:
    def __init__(self, name, hub):
        self.name = name
        self.core = Core()
        self.inbox = __import__("queue").Queue()
        self.hub = hub
        hub.join(self)

    def use_ability(self, caster, target, local):
        # 本地先执行（不论 local 与否，本地都要落地）
        self.core.apply_ability(caster, ABILITY_KILL, target)
        if local:
            # 本地发起 → 广播动作，对端用同一逻辑重算
            buf = encode_msg(RPC_ABILITY_USE,
                             pack_ability(caster, ABILITY_KILL, target, 0.0, 0.0))
            self.hub.broadcast(self, buf)

    def on_recv(self, rpc, payload):
        if rpc == RPC_ABILITY_USE:
            c, a, t, x, y = unpack_ability(payload)
            # 来自网络：只本地重算，不再广播（防回环）
            self.core.apply_ability(c, a, t)

    def pump(self):
        while not self.inbox.empty():
            buf = self.inbox.get()
            rpc, payload = decode_msg(buf)
            self.on_recv(rpc, payload)


def main():
    print("=== Nebula 端到端：纯本地逻辑 + 网络只同步动作 ===")

    hub = Hub()
    a = Client("iPad-A", hub)
    b = Client("iPad-B", hub)

    # 两端建房，角色一致（真实里由建房者分配后同步）
    for nid, role in [(1, ROLE_SHERIFF), (2, ROLE_CREW), (3, ROLE_IMPOSTOR)]:
        a.core.add_player(nid, role)
        b.core.add_player(nid, role)

    # A 本地：警长(1) 枪内鬼(3) —— 本地执行 + 广播
    a.use_ability(1, 3, local=True)

    # 两端收包
    a.pump(); b.pump()

    # 断言：两端状态字节级一致（联机兼容核心判据）
    assert a.core == b.core, "两端状态不一致！"
    assert a.core.players[3]["alive"] is False, "内鬼应被击杀"
    assert a.core.over and a.core.winner == 1, "内鬼全灭 → 船员胜"
    print("  [ok] 两端最终状态一致：", a.core.snapshot())
    print("\n结果: ALL PASS — 纯本地逻辑 + 动作广播 架构成立。")
    print("含义: 单机逻辑独立可玩；联机只需广播动作，两端各算各的即同步。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
