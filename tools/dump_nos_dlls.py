#!/usr/bin/env python3
"""从 Nebula 的 .NET 程序集提取：
  - 原始字节中的中文串（UTF-8 + UTF-16LE 双扫，覆盖 #Strings/#US/#Blob）
  - TypeDef 命名空间与类型数（看 API 表面与职业类结构）
用法: sudo python3 dump_nos_dlls.py <dll1> [dll2 ...]
"""
import sys, re
import dnfile

CJK = re.compile(r'[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]+')

def cjk_runs_from_bytes(data):
    out = set()
    for enc in ('utf-8', 'utf-16-le'):
        txt = data.decode(enc, errors='ignore')
        for m in CJK.finditer(txt):
            s = m.group(0).strip()
            if 1 <= len(s) <= 40:   # 职业/附加能力名长度合理范围
                out.add(s)
    return out

def dump(path, max_cjk=500):
    print("=" * 72)
    print("FILE:", path)
    data = open(path, 'rb').read()
    print(f"  size={len(data)} bytes")

    runs = cjk_runs_from_bytes(data)
    print(f"  中文串唯一值: {len(runs)}")
    print("  --- 中文串（前 %d）---" % max_cjk)
    for s in sorted(runs)[:max_cjk]:
        print("    ", s)

    # TypeDef 命名空间
    try:
        pe = dnfile.dnPE(path)
        td = pe.net.mdtables.TypeDef
        ns = {}
        for t in td:
            try:
                n = str(t.TypeName)
                nsp = str(t.TypeNamespace or "")
            except Exception:
                continue
            if nsp:
                ns[nsp] = ns.get(nsp, 0) + 1
        print(f"  TypeDef 总数: {len(td)}")
        print("  --- 命名空间 (类型数) ---")
        for k in sorted(ns, key=lambda x: -ns[x]):
            print(f"    {k:55s} {ns[k]}")
    except Exception as e:
        print("  [!] TypeDef 读取失败:", e)

if __name__ == "__main__":
    for p in sys.argv[1:]:
        dump(p)
