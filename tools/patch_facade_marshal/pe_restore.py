#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[R88] 把 Cecil 语义补丁版的 metadata 覆盖回「官方原版 PE 骨架」，产出 PE 保真版 facade。

用法: pe_restore.py <官方原版 System.Runtime.dll> <Cecil 补丁版> <输出>

背景（R87 真机实证）：
  R87 用 Mono.Cecil 的 mod.Write() 全量重写 System.Runtime.dll，语义上只多了
  一行 TypeForwardedTo(Marshal)，dnfile/ikdasm 都验证正确；但 Cecil 把整个官方 PE
  改得面目全非：
    - 文件 43344B -> 33280B（剥掉 10064B Authenticode overlay）
    - 强名签名 128B 清零，却仍把 CLI flags 的 STRONGNAMESIGNED 位置 1（自相矛盾）
    - PE checksum 归零、DebugDir 变小、#~ 流整段 0xa8..0x8008 重排
  真机上 R87 比 R86 更早崩：连 NebulaManualExecute ENTER 都没打出来，
  BepInEx LogOutput.log 本次增量为 0，mono_runtime_invoke 直接抛
  TypeInitializationException —— 高度怀疑「声明已强名签名但签名为空」这个
  不一致的 PE 让 iOS .NET 8 MonoVM 在加载 facade 时翻车（本地 Mono 6.12 宽松，
  能 LoadFile，所以沙箱没暴露）。

本脚本的做法（字节级手术，只动必须动的）：
  以【官方原版】为基底，保留它的 PE 头 / checksum / StrongName 目录 /
  DebugDir / SecurityDir / Authenticode overlay / 全部 section 布局；
  仅把【Cecil 补丁版】的 metadata 流（含新增的 Marshal ExportedType）覆盖进去，
  同步改 CLI header 的 MetaData.Size，并做两处一致性修正：
    - 清掉 STRONGNAMESIGNED 位（Cecil 没有私钥、无法重签名，就不该再声称已签名）
    - 强名签名区清零（内容变了签名必然无效，明确留空而非留半个无效签名）
  产物与官方原版字节数完全一致、IL 语义只多一行 Marshal 转发。

幂等：输入已是 PE 保真版时（Cecil 版 == 骨架 + 同 metadata）再跑结果不变。
"""

import hashlib
import struct
import sys


def pe_info(buf):
    pe = struct.unpack_from("<I", buf, 0x3C)[0]
    if buf[pe:pe + 4] != b"PE\0\0":
        raise ValueError("不是有效 PE")
    opt = pe + 24
    nsec = struct.unpack_from("<H", buf, pe + 6)[0]
    optsz = struct.unpack_from("<H", buf, pe + 20)[0]
    sec = opt + optsz
    cli_rva = struct.unpack_from("<I", buf, opt + 96 + 8 * 14)[0]  # COM_DESCRIPTOR
    cli_off = rva_to_off(buf, cli_rva, nsec, sec)
    if cli_off is None:
        raise ValueError("找不到 CLI header")
    md_rva, md_sz, flags, _ep = struct.unpack_from("<IIII", buf, cli_off + 8)
    sn_rva, sn_sz = struct.unpack_from("<II", buf, cli_off + 32)
    return dict(pe=pe, opt=opt, nsec=nsec, sec=sec, cli_off=cli_off,
                md_rva=md_rva, md_sz=md_sz, flags=flags,
                sn_rva=sn_rva, sn_sz=sn_sz)


def rva_to_off(buf, rva, nsec, sec):
    if not rva:
        return None
    for i in range(nsec):
        s = sec + 40 * i
        vs, va, rs, ro = struct.unpack_from("<IIII", buf, s + 8)
        if va <= rva < va + max(vs, rs):
            return ro + (rva - va)
    return None


def main():
    if len(sys.argv) != 4:
        print("用法: pe_restore.py <官方原版> <Cecil补丁版> <输出>", file=sys.stderr)
        return 2
    orig_path, cecil_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    a = bytearray(open(orig_path, "rb").read())   # 官方原版（PE 骨架）
    b = bytearray(open(cecil_path, "rb").read())  # Cecil 语义补丁版
    A, B = pe_info(a), pe_info(b)

    if A["md_rva"] != B["md_rva"]:
        print("R88 ✗ 两版 metadata RVA 不一致 (0x%x vs 0x%x)，拒绝合成"
              % (A["md_rva"], B["md_rva"]), file=sys.stderr)
        return 1
    if B["md_sz"] <= A["md_sz"]:
        print("R88 ✗ Cecil 版 metadata 没有变大，可能没补上 Marshal 转发，拒绝合成",
              file=sys.stderr)
        return 1

    md_off = rva_to_off(a, A["md_rva"], A["nsec"], A["sec"])
    cmd_off = rva_to_off(b, B["md_rva"], B["nsec"], B["sec"])
    sn_off = rva_to_off(a, A["sn_rva"], A["nsec"], A["sec"])
    if md_off is None or cmd_off is None or sn_off is None:
        print("R88 ✗ RVA->文件偏移换算失败", file=sys.stderr)
        return 1
    if md_off + B["md_sz"] > len(a) or cmd_off + B["md_sz"] > len(b):
        print("R88 ✗ metadata 越界", file=sys.stderr)
        return 1

    r = bytearray(a)
    # 1) 覆盖 metadata（含新增的 Marshal ExportedType），官方 PE 其余字节原样保留
    r[md_off:md_off + B["md_sz"]] = b[cmd_off:cmd_off + B["md_sz"]]
    # 2) CLI header 的 MetaData.Size 同步
    struct.pack_into("<I", r, A["cli_off"] + 12, B["md_sz"])
    # 3) 清 STRONGNAMESIGNED(0x8)：不能声称已签名却不给签名
    new_flags = A["flags"] & ~0x8
    struct.pack_into("<I", r, A["cli_off"] + 16, new_flags)
    # 4) 强名签名区清零
    r[sn_off:sn_off + A["sn_sz"]] = b"\0" * A["sn_sz"]

    open(out_path, "wb").write(r)
    print("   [R88] ✓ PE 保真: %d bytes (官方 %d), flags 0x%x->0x%x, "
          "MetaData %d->%d, sha=%s"
          % (len(r), len(a), A["flags"], new_flags, A["md_sz"], B["md_sz"],
             hashlib.sha256(r).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
