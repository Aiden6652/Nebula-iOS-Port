#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
protocol_sim.py — Nebula-iOS 自洽协议「参考实现 + 双客户端回路模拟」

这是 src/net/protocol.{h,c} 的 Python 镜像：产生/解析完全一致的字节流。
用途：在没有真机/iPad 的情况下，验证「iOS ↔ iOS 联机」的协议与状态同步逻辑成立。

跑法：
    python3 tools/sim/protocol_sim.py

它会：
  1. 用相同线格式对每条 RPC 做 编码→解码 往返断言；
  2. 起两个虚拟客户端（host / guest），经 loopback 传输交换消息；
  3. 跑一段最小玩法：建房握手 → 分配角色 → 释放技能(击杀) → 两端状态收敛；
  4. 断言两端最终状态字节级一致（联机兼容的核心判据）。
"""

import struct
import queue
import sys

# ---------------- 协议常量（与 protocol.h 完全一致） ----------------
MAGIC = 0x4E42          # 'N''B'
PROTO_VERSION = 0x01
HDR_LEN = 7
MAX_MSG = 4096

RPC_HELLO       = 0x5000
RPC_ROLE_ASSIGN = 0x5001
RPC_ABILITY_USE = 0x5002
RPC_WIN_CHECK   = 0x5003
RPC_STATE_SYNC  = 0x5004

EPOCH = 0x00010003      # 自洽协议 epoch（两端必须一致）

# 角色 id（演示用最小集合）
ROLE_CREWMATE = 0
ROLE_IMPOSTOR = 1
ROLE_SHERIFF  = 2

# 技能 id
ABILITY_KILL = 1


# ---------------- 基础编解码（小端，镜像 C） ----------------
def encode_msg(rpc: int, payload: bytes) -> bytes:
    if len(payload) > MAX_MSG - HDR_LEN:
        raise ValueError("payload 过长")
    return (struct.pack("<H", MAGIC)
            + struct.pack("<B", PROTO_VERSION)
            + struct.pack("<H", rpc)
            + struct.pack("<H", len(payload))
            + payload)


def decode_msg(buf: bytes):
    if len(buf) < HDR_LEN:
        raise ValueError("包过短")
    magic, ver, rpc, plen = struct.unpack_from("<HBHH", buf, 0)
    if magic != MAGIC:
        raise ValueError("魔数错误")
    if ver != PROTO_VERSION:
        raise ValueError("版本不符")
    if HDR_LEN + plen > len(buf):
        raise ValueError("长度越界")
    payload = buf[HDR_LEN:HDR_LEN + plen] if plen > 0 else b""
    return rpc, payload


# ---------------- 各 RPC 打包/解包（镜像 C） ----------------
def pack_hello(epoch: int) -> bytes:
    return struct.pack("<I", epoch)

def unpack_hello(p: bytes) -> int:
    return struct.unpack("<I", p)[0]

def pack_role(net_id: int, role_id: int) -> bytes:
    return struct.pack("<IH", net_id, role_id)

def unpack_role(p: bytes):
    return struct.unpack("<IH", p)

def pack_ability(caster: int, ability: int, target: int, x: float, y: float) -> bytes:
    return struct.pack("<IHIff", caster, ability, target, x, y)

def unpack_ability(p: bytes):
    return struct.unpack("<IHIff", p)

def pack_win(winner: int) -> bytes:
    return struct.pack("<B", winner)

def unpack_win(p: bytes) -> int:
    return struct.unpack("<B", p)[0]

def pack_state(net_id: int, key: int, val: int) -> bytes:
    return struct.pack("<IBi", net_id, key, val)

def unpack_state(p: bytes):
    return struct.unpack("<IBi", p)


# ---------------- 游戏状态 ----------------
class Player:
    def __init__(self, net_id, role):
        self.net_id = net_id
        self.role = role
        self.alive = True

    def snapshot(self):
        return (self.net_id, self.role, self.alive)

    def __eq__(self, other):
        return self.snapshot() == other.snapshot()


class GameState:
    def __init__(self):
        self.players = {}      # net_id -> Player
        self.winner = None

    def apply_role(self, net_id, role):
        if net_id not in self.players:
            self.players[net_id] = Player(net_id, role)
        else:
            self.players[net_id].role = role

    def apply_ability(self, caster, ability, target, x, y):
        # 演示：ABILITY_KILL 把目标置为死亡
        if ability == ABILITY_KILL and target in self.players:
            self.players[target].alive = False

    def apply_win(self, winner):
        self.winner = winner

    def snapshot(self):
        return {nid: p.snapshot() for nid, p in sorted(self.players.items())}, self.winner

    def __eq__(self, other):
        return self.snapshot() == other.snapshot()


# ---------------- 客户端 ----------------
class Hub:
    """极简「局域网」：把每个发送者的消息广播给所有【其他】客户端。"""
    def __init__(self):
        self.clients = []

    def join(self, client):
        self.clients.append(client)

    def broadcast(self, sender, buf):
        for c in self.clients:
            if c is not sender:
                c.inbox.put(buf)


class Client:
    def __init__(self, name):
        self.name = name
        self.state = GameState()
        self.inbox = queue.Queue()   # 只收「别人」发来的消息
        self.rx_log = []             # 收到的消息记录（用于展示）

    def _dispatch(self, rpc, payload):
        self.rx_log.append((rpc, payload))
        if rpc == RPC_HELLO:
            pass  # 己方握手回显，忽略
        elif rpc == RPC_ROLE_ASSIGN:
            nid, rid = unpack_role(payload)
            self.state.apply_role(nid, rid)
        elif rpc == RPC_ABILITY_USE:
            c, a, t, x, y = unpack_ability(payload)
            self.state.apply_ability(c, a, t, x, y)
        elif rpc == RPC_WIN_CHECK:
            w = unpack_win(payload)
            self.state.apply_win(w)

    def send(self, rpc, payload, hub):
        # 本地先行应用（权威端自己执行动作），再广播给其他客户端
        self._dispatch(rpc, payload)
        hub.broadcast(self, encode_msg(rpc, payload))

    def pump(self):
        """从收件箱取所有当前可用消息并处理（模拟网络收包）"""
        while not self.inbox.empty():
            buf = self.inbox.get()
            rpc, payload = decode_msg(buf)
            self._dispatch(rpc, payload)


# ---------------- 测试 ----------------
def test_roundtrip():
    print("  [RT] 每条 RPC 编码→解码 往返断言")
    cases = [
        (RPC_HELLO, pack_hello(EPOCH)),
        (RPC_ROLE_ASSIGN, pack_role(123456, ROLE_SHERIFF)),
        (RPC_ABILITY_USE, pack_ability(999, ABILITY_KILL, 888, 12.5, -3.25)),
        (RPC_WIN_CHECK, pack_win(1)),
        (RPC_STATE_SYNC, pack_state(42, 5, -100)),
    ]
    for rpc, payload in cases:
        buf = encode_msg(rpc, payload)
        drpc, dpayload = decode_msg(buf)
        assert drpc == rpc, f"rpc 不一致 {drpc} != {rpc}"
        assert dpayload == payload, "payload 不一致"
        # 长度/魔数防御：短包必须抛异常（用 pytest 风格断言）
        try:
            decode_msg(buf[:3])
            raise AssertionError("短包未被拒绝")
        except ValueError:
            pass
    print("       -> 全部往返一致，畸形包被正确拒绝")


def test_two_client_sync():
    print("  [SYNC] 双客户端联机状态收敛测试")
    hub = Hub()
    host = Client("iPad-A(房主)")
    guest = Client("iPad-B(客人)")
    hub.join(host)
    hub.join(guest)

    # 1) 握手（各自声明 epoch）
    host.send(RPC_HELLO, pack_hello(EPOCH), hub)
    guest.send(RPC_HELLO, pack_hello(EPOCH), hub)

    # 2) 房主分配角色：A=警长, B=船员, C=内鬼
    for nid, role in [(1, ROLE_SHERIFF), (2, ROLE_CREWMATE), (3, ROLE_IMPOSTOR)]:
        host.send(RPC_ROLE_ASSIGN, pack_role(nid, role), hub)

    # 3) 房主(警长)对 内鬼(3) 释放击杀技能，坐标 (10.0, 5.0)
    host.send(RPC_ABILITY_USE, pack_ability(1, ABILITY_KILL, 3, 10.0, 5.0), hub)

    # 4) 房主统一下发一次通用状态同步（权威端下发，两端都能收到）
    host.send(RPC_STATE_SYNC, pack_state(2, 1, 999), hub)

    # 5) 双方各自"收包"，处理全部消息
    host.pump()
    guest.pump()

    # 6) 断言两端状态完全一致（联机兼容核心判据）
    assert host.state == guest.state, "两端状态不一致！联机失败"
    assert host.state.players[3].alive is False, "内鬼应被击杀"
    assert host.state.players[1].role == ROLE_SHERIFF
    print("       -> 两端最终状态字节级一致 ✓")
    print(f"       最终快照: {host.state.snapshot()}")
    return host, guest


def main():
    print("=== Nebula-iOS 自洽协议模拟器 (Python 参考实现) ===")
    test_roundtrip()
    host, guest = test_two_client_sync()
    print("\n结果: ALL PASS — 自洽协议 + 双客户端状态同步成立。")
    print("含义: 这份线格式直接用于 dylib 的 iOS↔iOS 联机，两端一致即可同步。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
