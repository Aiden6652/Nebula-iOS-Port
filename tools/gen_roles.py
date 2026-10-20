#!/usr/bin/env python3
# 从官方汉化包提取职业 → 生成 src/core/roles.h / roles.c
import zipfile, io, re, os

INBOX = "/workspace/Nebula-iOS-Port/tools/uploader/inbox"
z = zipfile.ZipFile(INBOX + "/smve0v1fyy8mq0__Addons.zip")
tgt = None
for i in z.infolist():
    if i.filename.endswith("SCBaseLangPack.zip"):
        tgt = i; break
iz = zipfile.ZipFile(io.BytesIO(z.read(tgt)))
dat = None
for e in iz.infolist():
    if e.filename.endswith("Language/SChinese.dat"):
        dat = iz.read(e).decode("utf-8", "replace"); break

entries = {}
for line in dat.splitlines():
    s = line.strip()
    if not s or s.startswith("#"):
        continue
    sep = s.find(":")
    if sep < 0:
        sep = s.find("=")
    if sep < 0:
        continue
    k = s[:sep].strip().strip('"'); v = s[sep+1:].strip().strip('"')
    if k: entries[k] = v

roles = {}
for k, v in entries.items():
    m = re.match(r"^role\.([a-zA-Z0-9_]+)\.name$", k)
    if m: roles[m.group(1)] = v

# 阵营归属（按星云舰常见设定；未知默认船员阵营）
IMPOSTOR = {"impostor","sniper","bubblegun","morphing","camouflager","cleaner",
            "effacer","necromancer","evilGuesser","evilTracker","evilTrapper",
            "scarlet","raider","noiseghost","bountyHunter","destroyer","cannon",
            "damnedImpostor","clog","disturber","jailer"}
NEUTRAL  = {"jackal","sidekick","jester","arsonist","vulture","avenger",
            "chainShifter","lover","comet","grudge","lumine","alien","ember",
            "poltergeist","paparazzo","hallucination","confused","reaper",
            "scarletLover","obsessional","secret","secretInGame","metaRole",
            "extraMission","bloody"}


# ---- 职业颜色（用户指定 + 常见默认 + 阵营兜底）----
ROLE_COLOR = {
    # 用户明确要求
    "sheriff": "#FFA500",        # 警长 橙色
    "jackal":  "#1D95F9",        # 豺狼 蓝色
    "sidekick":"#1D95F9",        # 跟班 蓝色
    # 内鬼阵营：红色（用户要求"内鬼全部都是红色"）
    "impostor":"#FF1919", "sniper":"#FF1919", "bubblegun":"#FF1919",
    "morphing":"#FF1919", "camouflager":"#FF1919", "cleaner":"#FF1919",
    "effacer":"#FF1919", "necromancer":"#FF1919", "evilGuesser":"#FF1919",
    "evilTracker":"#FF1919", "evilTrapper":"#FF1919", "raider":"#FF1919",
    "noiseghost":"#FF1919", "destroyer":"#FF1919", "cannon":"#FF1919",
    "clog":"#FF1919", "disturber":"#FF1919", "jailer":"#FF1919",
    # 常见船员职业（各自颜色）
    "doctor":"#00FF7F", "seer":"#B48EFF", "mayor":"#FFD700",
    "bait":"#FF69B4", "crewmate":"#FFFFFF", "niceGuesser":"#FFFF99",
    "niceTrapper":"#99FF99", "niceTracker":"#CCFFFF", "psychic":"#E6E6FA",
    "busker":"#FFB6C1", "thurifer":"#FFE4B5", "provocateur":"#FF8C00",
    "justice":"#FFF8DC", "hadar":"#C0C0C0", "echo":"#DDA0DD",
    # 常见中立
    "jester":"#B0B0B0", "vulture":"#8B5A2B", "arsonist":"#FF4500",
    "avenger":"#808080", "chainShifter":"#7FFFD4", "lover":"#FFC0CB",
    "alien":"#98FB98", "comet":"#87CEEB", "grudge":"#696969",
    "chainshifter":"#7FFFD4", "confused":"#D3D3D3",
}
TEAM_COLOR = {"TEAM_IMPOSTOR":"#FF1919", "TEAM_NEUTRAL":"#C0C0C0", "TEAM_CREW":"#FFFFFF"}

def color_of(rid, team):
    if rid in ADDON_COLOR: return ADDON_COLOR[rid]    # addons 扩展职业的真实颜色
    if rid in ROLE_COLOR: return ROLE_COLOR[rid]
    return TEAM_COLOR.get(team, "#FFFFFF")


# ---- 扩展职业（来自 addons 的 Scripts/*.cs，非 NOS 内置）----
# 格式: key: (中文名, 阵营, 颜色)
# 颜色取自源码 base(...) 里的 RGB，阵营取自 RoleCategory/NebulaTeams
ADDON_ROLES = {
    "justicePlus": ("审判长", "TEAM_CREW", "#C2F2FF"),
}
ADDON_TEAM  = {k: v[1] for k, v in ADDON_ROLES.items()}
ADDON_COLOR = {k: v[2] for k, v in ADDON_ROLES.items()}

def is_addon(rid):
    return rid in ADDON_ROLES


# ---- 补充职业：NOS 内置但汉化包未覆盖的（阵营取自 Nebula.dll 的
#      Nebula.Roles.<Crewmate|Impostor|Neutral|Complex> 命名空间）----
# 中文名暂译（nos 里这些职业只有英文名），可用汉化包/插件词条覆盖
SUPPLEMENT = {
    # Complex（特殊阵营，按中立处理）
    "swapper":      ("交换者",   "TEAM_NEUTRAL"),
    "trapper":      ("陷阱师",   "TEAM_NEUTRAL"),
    "nexus":        ("枢纽",     "TEAM_NEUTRAL"),
    # Crewmate
    "climber":      ("攀登者",   "TEAM_CREW"),
    "doppelganger": ("二重身",   "TEAM_CREW"),
    "fixer":        ("修理工",   "TEAM_CREW"),
    "investigator": ("调查员",   "TEAM_CREW"),
    "jekyllandhyde":("双重人格", "TEAM_CREW"),
    "navvy":        ("杂役",     "TEAM_CREW"),
    "nicetracker":  ("正义追踪者","TEAM_CREW"),
    "oracle":       ("神谕者",   "TEAM_CREW"),
    # Impostor
    "amalgam":      ("融合体",   "TEAM_IMPOSTOR"),
    "creeping":     ("潜行者",   "TEAM_IMPOSTOR"),
    "cupid":        ("丘比特",   "TEAM_IMPOSTOR"),
    "eviltracker":  ("邪恶追踪者","TEAM_IMPOSTOR"),
    "gimlet":       ("钻头",     "TEAM_IMPOSTOR"),
    "graffiti":     ("涂鸦者",   "TEAM_IMPOSTOR"),
    "nightmare":    ("梦魇",     "TEAM_IMPOSTOR"),
    "quack":        ("庸医",     "TEAM_IMPOSTOR"),
    "rokurokubi":   ("辘轳首",   "TEAM_IMPOSTOR"),
    "sculptor":     ("雕塑家",   "TEAM_IMPOSTOR"),
    "snatcher":     ("掠夺者",   "TEAM_IMPOSTOR"),
    "whammy":       ("厄运师",   "TEAM_IMPOSTOR"),
    "zeal":         ("狂热者",   "TEAM_IMPOSTOR"),
    "balloonholder":("气球持有者","TEAM_IMPOSTOR"),
    # Neutral
    "opportunist":  ("机会主义者","TEAM_NEUTRAL"),
    "gambler":      ("赌徒",     "TEAM_NEUTRAL"),
    "plague":       ("瘟疫",     "TEAM_NEUTRAL"),
    "spectre":      ("幽灵",     "TEAM_NEUTRAL"),
    "vanity":       ("虚荣者",   "TEAM_NEUTRAL"),
    "chainshifter": ("链移者",   "TEAM_NEUTRAL"),
    "tyrant":       ("暴君",     "TEAM_NEUTRAL"),
}
SUPP_TEAM = {k: v[1] for k, v in SUPPLEMENT.items()}

def team_of(rid):
    if rid in ADDON_TEAM: return ADDON_TEAM[rid]      # addons 扩展职业优先
    if rid in SUPP_TEAM:  return SUPP_TEAM[rid]       # 汉化包未覆盖的内置职业
    if rid in IMPOSTOR: return "TEAM_IMPOSTOR"
    if rid in NEUTRAL:  return "TEAM_NEUTRAL"
    return "TEAM_CREW"

# 合并 addons 扩展职业（中文名）
for _k, _v in ADDON_ROLES.items():
    roles.setdefault(_k, _v[0])
# 合并汉化包未覆盖的内置职业（中文名暂译，可被插件词条覆盖）
for _k, _v in SUPPLEMENT.items():
    roles.setdefault(_k, _v[0])
_ = is_addon                      # 保留引用

ids = sorted(roles.keys())
enum_name = {}
for rid in ids:
    # camelCase -> UPPER_SNAKE
    s = re.sub(r"(?<!^)(?=[A-Z])", "_", rid).upper()
    enum_name[rid] = "ROLE_" + s

out_h = ["/* roles.h — 由官方汉化包自动生成，请勿手改（改请用 tools/ 下的生成器）",
         " * 数据来源：Addons/【汉化-基础】SCBaseLangPack.zip → Language/SChinese.dat",
         " * 共 %d 个官方职业，含官方中文名与阵营归属。 */" % len(ids),
         "#ifndef NEBULA_ROLES_H", "#define NEBULA_ROLES_H", "",
         "typedef enum {"]
for rid in ids:
    out_h.append("    %-32s /* %s */" % (enum_name[rid] + ",", roles[rid]))
out_h.append("    ROLE_COUNT")
out_h.append("} nebula_role_id_t;")
out_h.append("")
out_h.append("typedef enum { TEAM_CREW = 0, TEAM_IMPOSTOR = 1, TEAM_NEUTRAL = 2 } nebula_team_t;")
out_h.append("")
out_h.append("const char *nebula_role_name(int role);")
out_h.append("nebula_team_t nebula_role_team(int role);")
out_h.append("const char *nebula_role_key(int role);   /* 官方英文 id，如 sheriff */")
out_h.append("int  nebula_role_count(void);             /* 职业总数 */")
out_h.append("int  nebula_role_id_at(int idx);          /* 按序号取枚举值，便于遍历 */")
out_h.append("const char *nebula_role_enum_name(int role); /* 枚举名，如 ROLE_SHERIFF */")
out_h.append("const char *nebula_role_color(int role);  /* 职业颜色 #RRGGBB */")
out_h.append("int  nebula_role_id_by_key(const char *key);   /* 官方英文 id -> 枚举值，如 sheriff */")
out_h.append("")
out_h.append("#endif /* NEBULA_ROLES_H */")

out_c = ["/* roles.c — 官方职业表（自动生成） */",
         '#include "roles.h"',
         '#include <string.h>',
         "",
         "typedef struct { int id; const char *key; const char *name; nebula_team_t team; const char *color; } role_row_t;",
         "",
         "static const role_row_t ROLES[] = {"]
for rid in ids:
    out_c.append('    { %-30s "%s", "%s", %s, "%s" },' % (enum_name[rid] + ",", rid, roles[rid], team_of(rid), color_of(rid, team_of(rid))))
out_c.append("};")
out_c.append("")
out_c.append("static const int ROLE_N = (int)(sizeof(ROLES)/sizeof(ROLES[0]));")
out_c.append("")
out_c.append("const char *nebula_role_name(int role) {")
out_c.append("    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].name;")
out_c.append('    return "未知职业";')
out_c.append("}")
out_c.append("")
out_c.append("nebula_team_t nebula_role_team(int role) {")
out_c.append("    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].team;")
out_c.append("    return TEAM_CREW;")
out_c.append("}")
out_c.append("")
out_c.append("const char *nebula_role_key(int role) {")
out_c.append("    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].key;")
out_c.append('    return "";')
out_c.append("}")
out_c.append("")
out_c.append("static const char *ENUM_NAMES[] = {")
for rid in ids:
    out_c.append('    "%s",' % enum_name[rid])
out_c.append("};")
out_c.append("")
out_c.append("int nebula_role_count(void) { return ROLE_N; }")
out_c.append("int nebula_role_id_at(int idx) { return (idx>=0 && idx<ROLE_N) ? ROLES[idx].id : 0; }")
out_c.append("int nebula_role_id_by_key(const char *key) {")
out_c.append("    if (!key) return -1;")
out_c.append("    for (int i=0;i<ROLE_N;i++) if (ROLES[i].key && strcmp(ROLES[i].key, key)==0) return ROLES[i].id;")
out_c.append("    return -1;")
out_c.append("}")
out_c.append("")
out_c.append("const char *nebula_role_color(int role) {")
out_c.append("    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ROLES[i].color;")
out_c.append('    return "#FFFFFF";')
out_c.append("}")
out_c.append("")
out_c.append("const char *nebula_role_enum_name(int role) {")
out_c.append("    for (int i=0;i<ROLE_N;i++) if (ROLES[i].id==role) return ENUM_NAMES[i];")
out_c.append('    return "";')
out_c.append("}")

base = "/workspace/Nebula-iOS-Port/src/core"
open(base + "/roles.h", "w").write("\n".join(out_h) + "\n")
open(base + "/roles.c", "w").write("\n".join(out_c) + "\n")
print("已生成 roles.h / roles.c，职业数:", len(ids))
imp = sum(1 for r in ids if team_of(r) == "TEAM_IMPOSTOR")
neu = sum(1 for r in ids if team_of(r) == "TEAM_NEUTRAL")
cre = len(ids) - imp - neu
print("阵营分布: 船员 %d / 内鬼 %d / 中立 %d" % (cre, imp, neu))
