#!/usr/bin/env python3
"""
parse_dump.py — 从 dump.js 的输出里提取“与移植相关的”类与方法地址。

用法:
  python3 tools/parse_dump.py <nebula_dump.json>            # 按默认关键词过滤
  python3 tools/parse_dump.py <file> --grep Role            # 只看名字含 Role 的类
  python3 tools/parse_dump.py <file> --all                  # 打印全部

输出: 类名 + 其方法(名称, 偏移地址)，这正是后续写 Hook / 重写逻辑的入口清单。
"""
import json
import re
import sys

# 与玩法/联机/UI 最相关的高频关键词（移植时优先看这些类）
DEFAULT_KEYWORDS = [
    "Player", "GameManager", "ShipStatus", "Role", "Meeting", "Hud",
    "PlayerPhysics", "Kill", "Vent", "Task", "Minigame", "Custom",
    "Rpc", "Message", "Network", "Lobby", "Spawn", "Vote",
]


def extract_json(text):
    m = re.search(r"===DUMP_START===\s*(.*?)\s*===DUMP_END===", text, re.S)
    if not m:
        try:
            return json.loads(text)
        except Exception:
            return None
    return json.loads(m.group(1))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    grep = None
    show_all = "--all" in sys.argv
    for i, a in enumerate(sys.argv):
        if a == "--grep" and i + 1 < len(sys.argv):
            grep = sys.argv[i + 1]

    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        data = extract_json(f.read())
    if not data or "classes" not in data:
        print("[!] 未能从文件中解析出 dump JSON（确认是用 dump.js 的 ===DUMP_START=== 包裹的输出）")
        sys.exit(1)

    classes = data["classes"]
    print(f"# 共解析 {len(classes)} 个类")
    shown = 0
    for c in classes:
        name = c.get("name", "?")
        hit = show_all or (grep and grep.lower() in name.lower()) or \
              any(k.lower() in name.lower() for k in DEFAULT_KEYWORDS)
        if not hit:
            continue
        shown += 1
        print(f"\n## {name}  ({len(c.get('methods', []))} 方法)")
        for m in c.get("methods", []):
            print(f"   - {m['name']:<40} {m.get('ptr', '?')}")
    print(f"\n# 命中 {shown} 个类")


