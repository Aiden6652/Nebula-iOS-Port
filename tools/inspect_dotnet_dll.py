#!/usr/bin/env python3
"""
inspect_dotnet_dll.py —— 解析 .NET 程序集（如 Nebula.dll）提取职业与技能信息

为什么可行：Nebula.dll 是标准 .NET IL 程序集（未经过 IL2CPP），
其【元数据字符串堆 #Strings / #US】里存着全部类型名、成员名与字符串常量，
无需执行、无需反编译器即可读出职业 key、技能名、语言 key。

用法:
    python3 tools/inspect_dotnet_dll.py <Nebula.dll> [--grep 关键词]

输出:
    - 是否 .NET 程序集
    - 类型名/成员名(#Strings, ASCII)里与角色/技能相关的候选
    - 字符串常量(#US, UTF-16)里与角色/技能相关的候选
    - 命中 role.<key>.name 形式的语言 key
"""
import sys
import re
import struct


def read_u16_strings(data, min_len=3):
    """提取 UTF-16LE 字符串（.NET #US 堆即此编码）"""
    out = []
    pat = re.compile(rb"(?:[\x20-\x7e]\x00){%d,}" % min_len)
    for m in pat.finditer(data):
        try:
            s = m.group().decode("utf-16-le")
        except Exception:
            continue
        out.append(s)
    return out


def read_ascii_strings(data, min_len=4):
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, data):
        out.append(m.group().decode("ascii", "replace"))
    return out


def is_dotnet(data):
    """检查 PE 的 COM/CLI 描述符（数据目录第 14 项）"""
    try:
        if data[:2] != b"MZ":
            return False
        pe_off = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_off:pe_off + 4] != b"PE\x00\x00":
            return False
        opt_off = pe_off + 24
        magic = struct.unpack_from("<H", data, opt_off)[0]
        # 0x10b = PE32, 0x20b = PE32+
        dd_off = opt_off + (96 if magic == 0x10B else 112)
        # 数据目录每项 8 字节；第 14 项 = CLI Header
        cli_rva, cli_size = struct.unpack_from("<II", data, dd_off + 14 * 8)
        return cli_rva != 0 and cli_size != 0
    except Exception:
        return False


ROLE_HINT = re.compile(
    r"role|judge|sheriff|jackal|jester|sniper|doctor|seer|mayor|bait|arsonist|"
    r"vulture|necromancer|bubble|ninja|avenger|madmate|bodyguard|outsider",
    re.I,
)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    grep = None
    if "--grep" in sys.argv:
        i = sys.argv.index("--grep")
        if i + 1 < len(sys.argv):
            grep = sys.argv[i + 1]

    data = open(path, "rb").read()
    print("文件: %s  (%.1f KB)" % (path, len(data) / 1024))
    print("是否 .NET 程序集: %s" % ("✅ 是" if is_dotnet(data) else "❌ 否（可能是原生 DLL）"))

    # ① ASCII 字符串（#Strings：类型名/成员名/字段名）
    ascii_strs = read_ascii_strings(data, 4)
    cand = sorted({s for s in ascii_strs if ROLE_HINT.search(s)})
    print("\n=== 类型/成员名里的职业相关候选 (%d) ===" % len(cand))
    for s in cand[:80]:
        if grep and grep.lower() not in s.lower():
            continue
        print("   ", s)

    # ② UTF-16 字符串（#US：代码里的字符串常量，含语言 key）
    u16 = read_u16_strings(data, 3)
    uni = sorted({s for s in u16 if ROLE_HINT.search(s)})
    print("\n=== 字符串常量里的职业相关候选 (%d) ===" % len(uni))
    for s in uni[:80]:
        if grep and grep.lower() not in s.lower():
            continue
        print("   ", s)

    # ③ 语言 key：role.xxx.name 形式
    keys = {}
    for s in set(u16) | set(ascii_strs):
        for m in re.finditer(r"role\.([A-Za-z0-9_]+)\.(name|description|abilities)", s):
            keys.setdefault(m.group(1), set()).add(m.group(2))
    print("\n=== 语言 key 形式的职业 (role.<key>.name) ===")
    if keys:
        print("   共 %d 个职业 key:" % len(keys))
        for k in sorted(keys):
            print("     - %s  (%s)" % (k, ",".join(sorted(keys[k]))))
    else:
        print("   （未直接命中；可看上面的候选）")


if __name__ == "__main__":
    main()
