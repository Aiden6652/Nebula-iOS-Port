#!/usr/bin/env python3
"""快速勘察 Nebula on the Ship (NOS) 工程结构。
目标：确定把它跑进 iOS AU 需要的集成工作（BepInEx/Harmony/Il2CppInterop 依赖、
插件入口、NebulaAPI/Interstellar 引用、顶层模块划分）。
用法: python3 inspect_nos.py <NOS根目录>
"""
import os, re, sys

SKIP = ('/bin/', '/obj/', '/.git/', '/packages/', '/.vs/')

def walk(root):
    for dp, _, fns in os.walk(root):
        if any(s in dp for s in SKIP):
            continue
        for fn in fns:
            yield os.path.join(dp, fn)

_cache = {}
def read(f, limit=200000):
    if f in _cache:
        return _cache[f]
    try:
        with open(f, 'r', errors='ignore') as fh:
            t = fh.read(limit)
    except Exception:
        t = ''
    _cache[f] = t
    return t

PATTERNS = {
    'BepInEx':                 r'BepInEx',
    'Harmony/HarmonyX':        r'Harmony(?:X|Lib)?',
    'Il2CppInterop':           r'Il2CppInterop|Il2CppInternals',
    'Unhollower':              r'Unhollower',
    'Roslyn(CodeAnalysis)':    r'Microsoft\.CodeAnalysis',
    'Il2Cpp(泛型)':            r'Il2Cpp',
}

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    cs      = [f for f in walk(root) if f.endswith('.cs')]
    csprojs = [f for f in walk(root) if f.endswith('.csproj')]
    slns    = [f for f in walk(root) if f.endswith('.sln')]
    dlls    = [f for f in walk(root)
               if f.lower().endswith('.dll') and '/bin/' not in f and '/obj/' not in f]
    print("== 概览 ==")
    print(f"  .sln={len(slns)}  .csproj={len(csprojs)}  .cs={len(cs)}  .dll(非bin/obj)={len(dlls)}")

    csproj_hit = {k: 0 for k in PATTERNS}
    cs_hit     = {k: 0 for k in PATTERNS}
    for p in csprojs:
        t = read(p)
        for k, pat in PATTERNS.items():
            if re.search(pat, t):
                csproj_hit[k] += 1
    for f in cs:
        t = read(f)
        for k, pat in PATTERNS.items():
            if re.search(pat, t):
                cs_hit[k] += 1
    print("\n== 依赖特征（命中文件数）==")
    for k in PATTERNS:
        print(f"  {k:24s} csproj={csproj_hit[k]:3d}  cs={cs_hit[k]:4d}")

    print("\n== 可能的插件入口 ([BepInPlugin] / : BasePlugin) ==")
    n = 0
    for f in cs:
        t = read(f)
        if re.search(r'\[BepInPlugin\(', t) or re.search(r'class\s+\w+\s*:\s*BasePlugin', t):
            print(f"  {os.path.relpath(f, root)}")
            n += 1
    if n == 0:
        print("  (无，可能不是标准 BepInEx 插件形态)")

    print("\n== NebulaAPI / Interstellar 引用 (.csproj) ==")
    m = 0
    for f in csprojs:
        t = read(f)
        if 'NebulaAPI' in t or 'Interstellar' in t:
            print(f"  {os.path.relpath(f, root)}")
            m += 1
    if m == 0:
        print("  (csproj 中未直接引用，可能通过 dll 引用)")

    print("\n== 顶层目录 ==")
    for d in sorted(os.listdir(root)):
        if os.path.isdir(os.path.join(root, d)):
            print(f"  {d}/")

if __name__ == '__main__':
    main()
