#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scan_metadata_integrity.py —— 校验 .NET 程序集 #~ 元数据表区是否与流长度精确吻合。

严格复刻 dotnet/runtime release/8.0 的
    src/mono/mono/metadata/metadata.c: mono_metadata_compute_size()
    src/mono/mono/metadata/tabledefs.h: MONO_TABLE_* 编号

【R91 重要更正】此前版本把表结构写错了，导致把 169 个**完全合法**的程序集
误判成「损坏 / 缺失 256 字节 size_bitfield」，白走了三小时弯路。错误清单：
  1. 表 35 是 AssemblyRef（9 列 / 20 字节），旧版写成 [U16,BLOB,STR,STR]（8 字节），
     每行少算 12 字节 —— BepInEx.Core 里 6 行正好凑出「尾区缺 72 字节」的假象。
  2. 表 38 File / 39 ExportedType / 40 ManifestResource / 42 GenericParam 的列布局全错，
     且完全漏掉 30/31/33/34/36/37 号表。
  3. HasCustomAttribute 编码索引的目标表被多写了（把 CustomAttribute 表自身算进去），
     行数一旦 >= 2048 就把列宽从 2 误判成 4（Cpp2IL.Core 的 +4118 即由此而来）。
  4. 编码索引宽度必须用 MAX(目标表行数)，旧版 docstring 里写的 SUM 是错的。

另：**ECMA-335 的 #~ 流里根本不存在「256 字节 size_bitfield」这回事**。
size_bitfield 是 mono 在内存里由 mono_metadata_compute_size() 从内置 TableSchemas
算出来的，不存于文件。因此「给 DLL 插入 256 字节」既无效又会把合法元数据改坏。

判定标准（与 mono 实际读表行为一致）：
  - 表区起点 = #~ 流起点 + 24(头) + 4*N(N = valid 置位的表数)
  - 逐表累加 stride * rows，末尾应等于 #~ 流末尾，或相差 0~4 字节且这些字节全为 0
    （部分生成器按 4 字节对齐补零，属合法填充）。
  - 逐列校验 HEAP / 表索引 / 编码索引是否落在合法范围（可选，--refs）。

用法:
    python3 tools/scan_metadata_integrity.py <dir-or-dll> [<more>...] [--refs]
"""
import sys, struct, os, glob

# 编码索引：(tag 位数, 按 tag 值索引的表号数组，-1 = 该 tag 保留未用)。
#   - 宽度 = 2 if MAX(所有有效目标表行数) < (1 << (16-tagbits)) else 4（照 mono rtsize/MAX）
#   - 逐列校验时 tag 直接当数组下标用，故数组顺序必须是 ECMA-335 II.24.2.6 的 tag 定义，
#     而不是任意的「目标表集合」顺序（这正是旧版 --refs 误报 tag3 越界的原因）。
CODED = {
    'ResolutionScope':     (2, [0, 26, 35, 1]),                          # 0 Module,1 ModuleRef,2 AssemblyRef,3 TypeRef
    'TypeDefOrRef':        (2, [2, 1, 27, -1]),                          # 0 TypeDef,1 TypeRef,2 TypeSpec,3 保留
    'HasConstant':         (2, [4, 8, 23, -1]),                          # 0 Field,1 Param,2 Property,3 保留
    'HasCustomAttribute':  (5, [6, 4, 1, 2, 8, 9, 10, 0, 14, 23, 20, 17,  # tag 0..21
                                 26, 27, 32, 35, 38, 39, 40, 42, 44, 43]),  # tag>=22 保留
    'CustomAttributeType': (3, [-1, -1, 6, 10, -1, -1, -1, -1]),        # 2 MethodDef,3 MemberRef，其余保留
    'HasDeclSecurity':     (2, [2, 6, 32, -1]),                          # 0 TypeDef,1 MethodDef,2 Assembly
    'MemberRefParent':     (3, [2, 1, 26, 6, 27, -1, -1, -1]),           # 0..4 有效，5..7 保留
    'HasSemantics':        (1, [20, 23]),                                # 0 Event,1 Property
    'MethodDefOrRef':      (1, [6, 10]),                                 # 0 MethodDef,1 MemberRef
    'MemberForwarded':     (1, [4, 6]),                                  # 0 Field,1 MethodDef
    'Implementation':      (2, [38, 35, 39, -1]),                        # 0 File,1 AssemblyRef,2 ExportedType
    'HasFieldMarshal':     (1, [4, 8]),                                  # 0 Field,1 Param
    'TypeOrMethodDef':     (1, [2, 6]),                                  # 0 TypeDef,1 MethodDef
}

# 列类型：'u8' 'u16' 'u32' 'str' 'guid' 'blob' / ('idx', 表号) / ('cis', 编码索引名)
SCHEMA = {
    0:  ['u16', 'str', 'guid', 'guid', 'guid'],                                   # Module          10
    1:  [('cis', 'ResolutionScope'), 'str', 'str'],                              # TypeRef          6
    2:  ['u32', 'str', 'str', ('cis', 'TypeDefOrRef'), ('idx', 4), ('idx', 6)],  # TypeDef         14
    3:  [('idx', 4)],                                                            # FieldPtr         2
    4:  ['u16', 'str', 'blob'],                                                   # Field            6
    5:  [('idx', 6)],                                                            # MethodPtr        2
    6:  ['u32', 'u16', 'u16', 'str', 'blob', ('idx', 8)],                        # Method          14
    7:  [('idx', 8)],                                                            # ParamPtr         2
    8:  ['u16', 'u16', 'str'],                                                    # Param            6
    9:  [('idx', 2), ('cis', 'TypeDefOrRef')],                                    # InterfaceImpl    4
    10: [('cis', 'MemberRefParent'), 'str', 'blob'],                             # MemberRef        6
    11: ['u8', 'u8', ('cis', 'HasConstant'), 'blob'],                            # Constant         6
    12: [('cis', 'HasCustomAttribute'), ('cis', 'CustomAttributeType'), 'blob'], # CustomAttribute  6
    13: [('cis', 'HasFieldMarshal'), 'blob'],                                    # FieldMarshal     4
    14: ['u16', ('cis', 'HasDeclSecurity'), 'blob'],                             # DeclSecurity     6
    15: ['u16', 'u32', ('idx', 2)],                                              # ClassLayout      8
    16: ['u32', ('idx', 4)],                                                     # FieldLayout      6
    17: ['blob'],                                                                # StandAloneSig    2
    18: [('idx', 2), ('idx', 20)],                                               # EventMap         4
    19: [('idx', 20)],                                                           # EventPtr         2
    20: ['u16', 'str', ('cis', 'TypeDefOrRef')],                                # Event            6
    21: [('idx', 2), ('idx', 23)],                                               # PropertyMap      4
    22: [('idx', 23)],                                                           # PropertyPtr      2
    23: ['u16', 'str', 'blob'],                                                   # Property         6
    24: ['u16', ('idx', 6), ('cis', 'HasSemantics')],                            # MethodSemantics  6
    25: [('idx', 2), ('cis', 'MethodDefOrRef'), ('cis', 'MethodDefOrRef')],      # MethodImpl       6
    26: ['str'],                                                                 # ModuleRef        2
    27: ['blob'],                                                                # TypeSpec         2
    28: ['u16', ('cis', 'MemberForwarded'), 'str', ('idx', 26)],                 # ImplMap          8
    29: ['u32', ('idx', 4)],                                                     # FieldRVA         6
    30: ['u32', 'u32'],                                                          # ENCLog           8
    31: ['u32'],                                                                 # ENCMap           4
    32: ['u32', 'u16', 'u16', 'u16', 'u16', 'u32', 'blob', 'str', 'str'],        # Assembly        22
    33: ['u32'],                                                                 # AssemblyProc     4
    34: ['u32', 'u32', 'u32'],                                                    # AssemblyOS      12
    35: ['u16', 'u16', 'u16', 'u16', 'u32', 'blob', 'str', 'str', 'blob'],       # AssemblyRef     20
    36: ['u32', ('idx', 35)],                                                    # AssemblyRefProc  6
    37: ['u32', 'u32', 'u32', ('idx', 35)],                                      # AssemblyRefOS   14
    38: ['u32', 'str', 'blob'],                                                   # File             8
    39: ['u32', 'u32', 'str', 'str', ('cis', 'Implementation')],                 # ExportedType    14
    40: ['u32', 'u32', 'str', ('cis', 'Implementation')],                        # ManifestRes     12
    41: [('idx', 2), ('idx', 2)],                                                # NestedClass      4
    42: ['u16', 'u16', ('cis', 'TypeOrMethodDef'), 'str'],                       # GenericParam     8
    43: [('cis', 'MethodDefOrRef'), 'blob'],                                     # MethodSpec       4
    44: [('idx', 42), ('cis', 'TypeDefOrRef')],                                  # GenericParamConstraint 4
}


def parse_pe(path):
    data = open(path, 'rb').read()
    if data[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\x00\x00':
        return None
    magic = struct.unpack_from('<H', data, pe + 24)[0]
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    num_sec = struct.unpack_from('<H', data, pe + 6)[0]
    sec = pe + 24 + opt_size

    def rva2raw(rva):
        for i in range(num_sec):
            s = sec + i * 40
            vaddr = struct.unpack_from('<I', data, s + 12)[0]
            rawsz = struct.unpack_from('<I', data, s + 16)[0]
            raw = struct.unpack_from('<I', data, s + 20)[0]
            vsize = struct.unpack_from('<I', data, s + 8)[0]
            if vaddr <= rva < vaddr + max(vsize, rawsz):
                return raw + (rva - vaddr)
        return None

    dd_off = pe + 24 + (96 if magic == 0x10B else 112)
    cli_rva, _ = struct.unpack_from('<II', data, dd_off + 14 * 8)
    return data, cli_rva, rva2raw


def get_streams(data, meta):
    """返回 {流名: (文件内绝对偏移, 流长度)}"""
    vlen = struct.unpack_from('<I', data, meta + 12)[0]
    pos = meta + 16 + vlen
    pos = (pos + 3) & ~3
    pos += 2                       # Flags(2)
    nstreams = struct.unpack_from('<H', data, pos)[0]
    pos += 2
    out = {}
    for _ in range(nstreams):
        soff, sz = struct.unpack_from('<I', data, pos)[0], struct.unpack_from('<I', data, pos + 4)[0]
        name = data[pos + 8:pos + 8 + 32].split(b'\x00')[0].decode('ascii', 'replace')
        out[name] = (meta + soff, sz)
        pos += 8 + ((len(name) + 1 + 3) & ~3)
    return out


def col_widths(cols, rows, hf):
    """复刻 mono_metadata_compute_size()：返回每个列的字节宽度列表"""
    sw, bw, gw = bool(hf & 1), bool(hf & 4), bool(hf & 2)

    def one(c):
        if c == 'u8':
            return 1
        if c == 'u16':
            return 2
        if c == 'u32':
            return 4
        if c == 'str':
            return 4 if sw else 2
        if c == 'guid':
            return 4 if gw else 2
        if c == 'blob':
            return 4 if bw else 2
        if isinstance(c, tuple):
            if c[0] == 'idx':                      # idx_size()
                return 4 if rows.get(c[1], 0) >= 65536 else 2
            tagbits, targets = CODED[c[1]]         # rtsize(MAX(nrows), 16-tagbits)
            n = max((rows.get(t, 0) for t in targets if t >= 0), default=0)
            return 2 if n < (1 << (16 - tagbits)) else 4
        raise ValueError('未知列类型 %r' % (c,))

    return [one(c) for c in cols]


def scan_one(path, check_refs=False):
    pe = parse_pe(path)
    if pe is None:
        return '非 PE/.NET 程序集'
    data, cli_rva, rva2raw = pe
    cli_raw = rva2raw(cli_rva)
    if cli_raw is None:
        return '无法定位 CLI 头'
    md_rva, _ = struct.unpack_from('<II', data, cli_raw + 8)
    md_raw = rva2raw(md_rva)
    if md_raw is None:
        return '无法定位元数据根'
    streams = get_streams(data, md_raw)
    if '#~' not in streams:
        return '无 #~ 流（#- 压缩流，跳过）'
    sb_off, sb_size = streams['#~']
    sb_end = sb_off + sb_size

    # #~ 头：Reserved(4) Major(1) Minor(1) HeapSizes(1) Reserved(1) Valid(8) Sorted(8) = 24
    hf = data[sb_off + 6]
    valid = struct.unpack_from('<Q', data, sb_off + 8)[0]
    present = [t for t in range(64) if valid & (1 << t)]
    p = sb_off + 24
    rows = {}
    for t in present:
        rows[t] = struct.unpack_from('<I', data, p)[0]
        p += 4

    heaps = {k: streams[k][1] for k in ('#Strings', '#Blob', '#GUID', '#US') if k in streams}
    cur = p
    for t in present:
        cols = SCHEMA.get(t)
        if cols is None:
            return f'表#{t} 无 schema（valid 置了 mono 不认识的表，或含 portable-PDB 表）'
        w = col_widths(cols, rows, hf)
        stride = sum(w)
        if cur + stride * rows[t] > sb_end + 4:
            return f'表#{t} 越界: 需 {stride * rows[t]} 字节 @ {cur:#x}, #~ 大小 {sb_size}'
        if check_refs:
            off = 0
            bad = None
            for c, cw_ in zip(cols, w):
                off += cw_
            # 逐行抽验索引/堆偏移是否在合法范围
            for r in range(rows[t]):
                row, o = cur + r * stride, 0
                for c, cw_ in zip(cols, w):
                    v = int.from_bytes(data[row + o:row + o + cw_], 'little')
                    o += cw_
                    if isinstance(c, tuple) and c[0] == 'idx':
                        # ECMA-335：list 类索引（FieldList/MethodList/ParamList/EventList 等）
                        # 允许取值 0(nil) 或 1..rows，且【最后一行可用 rows+1 作末尾哨兵】。
                        # IL2CPP stub 里 Field 表 0 行时 fieldlist=1 也合法。故上限为 rows+1。
                        if v > rows.get(c[1], 0) + 1:
                            bad = f'表#{t} 行{r} 表索引 ->{c[1]} = {v} 越界(上限 {rows.get(c[1],0)+1})'
                            break
                    elif isinstance(c, tuple) and c[0] == 'cis':
                        tb, tg = CODED[c[1]]
                        tag, idx = v & ((1 << tb) - 1), v >> tb
                        # 编码索引里 0 = nil（合法）；tag 按数组下标取表，
                        # -1/越界 = 保留 tag（个别 stub 会写，只提示不算结构性损坏）。
                        # 行号上限 rows+1 放宽末尾哨兵。
                        if tag >= len(tg):
                            bad = f'表#{t} 行{r} 编码索引 {c[1]} 保留tag{tag} idx{idx}'
                            break
                        tgt = tg[tag]
                        if tgt < 0 or idx > rows.get(tgt, 0) + 1:
                            bad = f'表#{t} 行{r} 编码索引 {c[1]} tag{tag} idx{idx} 越界'
                            break
                    elif c in ('str', 'blob', 'guid'):
                        hk = {'str': '#Strings', 'blob': '#Blob', 'guid': '#GUID'}[c]
                        if v >= heaps.get(hk, 0):
                            bad = f'表#{t} 行{r} {hk} 偏移 {v} 越界(堆 {heaps.get(hk, 0)})'
                            break
                if bad:
                    break
            if bad:
                return bad
        cur += stride * rows[t]

    # 流末尾允许 0~4 字节全零填充（生成器按 4 字节对齐）
    gap = sb_end - cur
    if gap < 0 or gap > 4:
        return f'表区末尾偏差 {gap:+d} 字节（超出合法填充范围），疑似表结构不匹配'
    if gap and any(data[cur:sb_end]):
        return f'表区末尾 {gap} 字节非零，不是纯填充'
    return None


def main():
    args = [a for a in sys.argv[1:] if a != '--refs']
    check_refs = '--refs' in sys.argv[1:]
    paths = []
    for a in args:
        if os.path.isdir(a):
            paths += sorted(glob.glob(os.path.join(a, '*.dll')))
        else:
            paths.append(a)
    paths = [p for p in paths if not p.endswith('.orig')]
    bad, skipped = [], []
    for p in paths:
        r = scan_one(p, check_refs)
        if r and r.startswith('无 #~'):
            skipped.append((os.path.basename(p), r))
        elif r:
            bad.append((os.path.basename(p), r))
            print('[损坏] %s: %s' % (os.path.basename(p), r))
    total = len(paths) - len(skipped)
    print('\n=== 扫描 %d 个 dll（跳过 %d 个 #- 流），异常 %d 个 ===' % (total, len(skipped), len(bad)))
    for name, r in bad:
        print('  %s: %s' % (name, r))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
