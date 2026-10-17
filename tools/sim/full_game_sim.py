#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
full_game_sim.py — 完整对局端到端验证（纯本地引擎 + 网络只广播动作）

跑一整局：建房分角色 → 击杀 → 会议 → 投票 → 驱逐 → 判胜负。
两台"iPad"全程各自本地重算，断言每一步与最终状态都字节级一致。

跑法：python3 tools/sim/full_game_sim.py
"""

import sys
from protocol_sim import (encode_msg, decode_msg, RPC_ABILITY_USE, RPC_HELLO,
                          pack_ability, unpack_ability, pack_hello, EPOCH,
                          ABILITY_KILL, Hub)

# 角色 / 动作 / 修饰（与 src/core/core.h 对齐）
ROLE_CREW, ROLE_IMPOSTOR, ROLE_SHERIFF, ROLE_ENGINEER, ROLE_JESTER = 0, 1, 2, 3, 4
ABILITY_VENT, ABILITY_MEETING, ABILITY_VOTE, ABILITY_TASK = 2, 3, 4, 5
MOD_BAIT = 0x0002
PHASE_PLAY, PHASE_MEET, PHASE_OVER = 0, 1, 2


class Core:
    def __init__(self):
        self.players = {}
        self.phase = PHASE_PLAY
        self.winner = 0
        self.meeting_by = -1

    def add_player(self, nid, role):
        self.players[nid] = {"role": role, "alive": True, "in_vent": False,
                             "voted": False, "vote_target": 0, "mods": 0}

    def find(self, nid):
        return self.players.get(nid)

    def apply_ability(self, caster, ability, target, x=0.0, y=0.0):
        if self.phase == PHASE_OVER:
            return
        c = self.find(caster)
        if not c or not c["alive"]:
            return
        if ability == ABILITY_KILL:
            t = self.find(target)
            if not t or not t["alive"]:
                return
            if c["role"] == ROLE_IMPOSTOR:
                t["alive"] = False
            elif c["role"] == ROLE_SHERIFF:
                if t["role"] == ROLE_IMPOSTOR:
                    t["alive"] = False
                else:
                    c["alive"] = False
            else:
                return
            if t["mods"] & MOD_BAIT:
                self.call_meeting(target)
        elif ability == ABILITY_VENT:
            if c["role"] in (ROLE_IMPOSTOR, ROLE_ENGINEER):
                c["in_vent"] = not c["in_vent"]
            else:
                return
        elif ability == ABILITY_TASK:
            pass  # 仅标记
        if self.phase == PHASE_PLAY:
            self.check_win()

    def call_meeting(self, caster):
        if self.phase == PHASE_OVER:
            return
        if not self.find(caster):
            return
        self.phase = PHASE_MEET
        self.meeting_by = caster
        for p in self.players.values():
            p["voted"] = False
            p["vote_target"] = 0

    def cast_vote(self, voter, target):
        if self.phase != PHASE_MEET:
            return
        v = self.find(voter)
        if not v or not v["alive"] or v["voted"]:
            return
        v["voted"] = True
        v["vote_target"] = target

    def resolve_meeting(self):
        if self.phase != PHASE_MEET:
            return -1
        cnt = {nid: 0 for nid in self.players}
        for p in self.players.values():
            if p["voted"]:
                cnt[p["vote_target"]] = cnt.get(p["vote_target"], 0) + 1
        maxc = max(cnt.values()) if cnt else 0
        tops = [n for n, v in cnt.items() if v == maxc and maxc > 0]
        ejected = -1
        if len(tops) == 1:
            ejected = tops[0]
            self.players[ejected]["alive"] = False
            if self.players[ejected]["role"] == ROLE_JESTER:
                self.phase, self.winner = PHASE_OVER, ROLE_JESTER
                return ejected
        self.phase = PHASE_PLAY
        self.meeting_by = -1
        self.check_win()
        return ejected

    def check_win(self):
        if self.phase == PHASE_OVER:
            return
        ai = sum(1 for p in self.players.values()
                 if p["alive"] and p["role"] == ROLE_IMPOSTOR)
        ao = sum(1 for p in self.players.values()
                 if p["alive"] and p["role"] not in (ROLE_IMPOSTOR, ROLE_JESTER))
        if ai == 0:
            self.phase, self.winner = PHASE_OVER, 1
        elif ai >= ao:
            self.phase, self.winner = PHASE_OVER, 2

    def add_mod(self, nid, mod):
        if nid in self.players:
            self.players[nid]["mods"] |= mod

    def snapshot(self):
        ps = {n: (d["role"], d["alive"], d["in_vent"], d["mods"])
              for n, d in sorted(self.players.items())}
        return ps, (self.phase, self.winner, self.meeting_by)

    def __eq__(self, o):
        return self.snapshot() == o.snapshot()


class Client:
    def __init__(self, name, hub):
        self.name = name
        self.hub = hub
        self.core = Core()
        self.inbox = __import__("queue").Queue()
        hub.join(self)

    def do(self, caster, ability, target=None, local=True):
        self.core.apply_ability(caster, ability, target or 0)
        if local:
            buf = encode_msg(RPC_ABILITY_USE,
                             pack_ability(caster, ability, target or 0, 0.0, 0.0))
            self.hub.broadcast(self, buf)

    def do_meeting(self, caster, local=True):
        self.core.call_meeting(caster)
        if local:
            buf = encode_msg(RPC_ABILITY_USE,
                             pack_ability(caster, ABILITY_MEETING, caster, 0.0, 0.0))
            self.hub.broadcast(self, buf)

    def vote(self, voter, target, local=True):
        self.core.cast_vote(voter, target)
        if local:
            buf = encode_msg(RPC_ABILITY_USE,
                             pack_ability(voter, ABILITY_VOTE, target, 0.0, 0.0))
            self.hub.broadcast(self, buf)

    def on_recv(self, rpc, payload):
        if rpc == RPC_ABILITY_USE:
            c, a, t, x, y = unpack_ability(payload)
            # 来自网络：只本地重算，不再广播
            if a == ABILITY_MEETING:
                self.core.call_meeting(c)
            elif a == ABILITY_VOTE:
                self.core.cast_vote(c, t)
            else:
                self.core.apply_ability(c, a, t, x, y)

    def pump(self):
        while not self.inbox.empty():
            buf = self.inbox.get()
            rpc, payload = decode_msg(buf)
            self.on_recv(rpc, payload)


def main():
    print("=== Nebula 完整对局端到端验证 ===")
    hub = Hub()
    a = Client("iPad-A", hub)
    b = Client("iPad-B", hub)

    roster = [(1, ROLE_SHERIFF), (2, ROLE_CREW), (3, ROLE_IMPOSTOR),
              (4, ROLE_ENGINEER), (5, ROLE_JESTER)]
    for nid, role in roster:
        a.core.add_player(nid, role)
        b.core.add_player(nid, role)

    # 步骤1：内鬼(3) 杀船员(2)，两端收敛（内鬼仍存活，局继续）
    a.do(3, ABILITY_KILL, 2)
    a.pump(); b.pump()
    assert a.core == b.core, "步骤1 不一致"
    print("  [ok] 击杀后两端一致")

    # 步骤2：工程师(4) 发起会议 → 投票驱逐小丑(5) → 小丑胜
    # （内鬼仍存活，未提前结束；玩家2已死无法投票，符合真实）
    a.do_meeting(4)
    a.vote(1, 5); a.vote(4, 5)    # A 本地投，广播
    a.pump(); b.pump()            # B 收到会议+投票并本地重算
    ej = a.core.resolve_meeting(); b.core.resolve_meeting()
    a.pump(); b.pump()
    assert a.core == b.core, "步骤2 不一致"
    assert a.core.winner == ROLE_JESTER, "小丑被驱逐应小丑胜"
    print("  [ok] 会议+投票+驱逐后两端一致，小丑胜：", a.core.snapshot()[1])

    print("\n结果: ALL PASS — 完整对局（击杀/会议/投票/驱逐/胜负）架构成立。")
    print("含义: 本地引擎独立可玩，联机仅广播动作，两端各算各的自然同步。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
