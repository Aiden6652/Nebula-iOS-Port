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
    "swapper":      ("换票师",   "TEAM_NEUTRAL"),   # 汉化包真名
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


# ---- addons 职业（中文名取自 nos 的汉化包 SChinese.dat，
#      阵营取自 addons 源码的 RoleCategory / NebulaTeams）----
ADDON_EXTRA = {
    "almighty": ("全能大师", "TEAM_IMPOSTOR"),
    "aloneman": ("独行客", "TEAM_CREW"),
    "amon": ("错误", "TEAM_NEUTRAL"),
    "assistant": ("副手", "TEAM_IMPOSTOR"),
    "attacker": ("奇袭者", "TEAM_IMPOSTOR"),
    "aurora": ("极光之女", "TEAM_IMPOSTOR"),
    "authorfairytale": ("童话作家", "TEAM_NEUTRAL"),
    "awake": ("清醒者", "TEAM_NEUTRAL"),
    "baitm": ("诱饵", "TEAM_NEUTRAL"),
    "bizarrosorcerer": ("诡法师", "TEAM_CREW"),
    "bomber": ("炸弹王", "TEAM_IMPOSTOR"),
    "bombertwins": ("双子爆破者", "TEAM_NEUTRAL"),
    "bountyhuntermod": ("赏金猎手", "TEAM_IMPOSTOR"),
    "buttonbarry": ("执钮人", "TEAM_NEUTRAL"),
    "celestialdog": ("天狗", "TEAM_CREW"),
    "chainkiller": ("连环杀手", "TEAM_IMPOSTOR"),
    "chameleon": ("变色龙", "TEAM_NEUTRAL"),
    "channeladmin": ("频道管理员", "TEAM_NEUTRAL"),
    "connoisseur": ("鉴赏狂人", "TEAM_NEUTRAL"),
    "crawlerengineer": ("爬虫工程师", "TEAM_IMPOSTOR"),
    "criminalistwriter": ("犯罪学作家", "TEAM_CREW"),
    "critic": ("评论家", "TEAM_NEUTRAL"),
    "crow": ("黑鸦", "TEAM_IMPOSTOR"),
    "deductionist": ("推演师", "TEAM_NEUTRAL"),
    "disperser": ("分散者", "TEAM_IMPOSTOR"),
    "disposer": ("肢解师", "TEAM_IMPOSTOR"),
    "dissociation": ("解离症", "TEAM_NEUTRAL"),
    "doomsayer": ("末日赌怪", "TEAM_NEUTRAL"),
    "doomsayer2": ("末日预言家", "TEAM_NEUTRAL"),
    "doublekiller": ("双刀开挂王", "TEAM_IMPOSTOR"),
    "dreamfall": ("坠梦者", "TEAM_NEUTRAL"),
    "dreamstealer": ("窃梦家", "TEAM_CREW"),
    "dreamweaver": ("织梦人", "TEAM_IMPOSTOR"),
    "dreamweaverm": ("织梦人", "TEAM_IMPOSTOR"),
    "dunner": ("讨债人", "TEAM_IMPOSTOR"),
    "eclipseprinciple": ("日食主教", "TEAM_NEUTRAL"),
    "engineer": ("工程师", "TEAM_CREW"),
    "escapist": ("逃逸者", "TEAM_IMPOSTOR"),
    "evilace": ("密命王牌", "TEAM_IMPOSTOR"),
    "evilbusker": ("亡灵爆破师", "TEAM_IMPOSTOR"),
    "evilnecromancer": ("亡灵巫师", "TEAM_IMPOSTOR"),
    "evilsplicer": ("处决者", "TEAM_NEUTRAL"),
    "exileadmin": ("放逐管理员", "TEAM_NEUTRAL"),
    "falsecharges": ("伪证师", "TEAM_NEUTRAL"),
    "fancy": ("空想症", "TEAM_NEUTRAL"),
    "fantast": ("空想家", "TEAM_NEUTRAL"),
    "fatalscale": ("逆鳞", "TEAM_NEUTRAL"),
    "fatepeeper": ("窥命者", "TEAM_NEUTRAL"),
    "ferryman": ("摆渡人", "TEAM_CREW"),
    "firstdead": ("首刀保护", "TEAM_NEUTRAL"),
    "flowerburial": ("花葬师", "TEAM_CREW"),
    "flyhead": ("超级面筋人", "TEAM_NEUTRAL"),
    "forensicdoctor": ("法医", "TEAM_CREW"),
    "gamemaster": ("管理员", "TEAM_NEUTRAL"),
    "grenadier": ("掷弹兵", "TEAM_IMPOSTOR"),
    "hadmark": ("命定之人", "TEAM_IMPOSTOR"),
    "haslove": ("被传递爱心", "TEAM_CREW"),
    "hitman": ("雇佣杀手", "TEAM_NEUTRAL"),
    "holmes": ("福尔摩斯", "TEAM_NEUTRAL"),
    "immersionspectator": ("观战席", "TEAM_NEUTRAL"),
    "indomitable": ("不屈者", "TEAM_NEUTRAL"),
    "infected": ("叛离者", "TEAM_IMPOSTOR"),
    "insomniacs": ("失眠症患者", "TEAM_CREW"),
    "insomniacsm": ("失眠症患者", "TEAM_CREW"),
    "intuition": ("第六感", "TEAM_CREW"),
    "jailor": ("典狱长", "TEAM_CREW"),
    "knight": ("侠客", "TEAM_CREW"),
    "knighted": ("骑士", "TEAM_NEUTRAL"),
    "knightedtarget": ("主公", "TEAM_NEUTRAL"),
    "lastimpostor": ("绝境者", "TEAM_IMPOSTOR"),
    "lawyer": ("律师", "TEAM_NEUTRAL"),
    "lawyerclient": ("客户", "TEAM_NEUTRAL"),
    "lighting": ("执灯人", "TEAM_NEUTRAL"),
    "lovemesseger": ("爱心使", "TEAM_CREW"),
    "luciddreamer": ("清醒梦", "TEAM_CREW"),
    "madmateplus": ("背信者", "TEAM_IMPOSTOR"),
    "marksman": ("快枪手", "TEAM_IMPOSTOR"),
    "mayfly": ("蜉蝣", "TEAM_CREW"),
    "meetingadmin": ("会议管理员", "TEAM_NEUTRAL"),
    "mimicassistant": ("模仿者（助手）", "TEAM_IMPOSTOR"),
    "mimickiller": ("模仿者（杀手）", "TEAM_IMPOSTOR"),
    "mini": ("迷你船员", "TEAM_CREW"),
    "miraclemerchant": ("奇迹商人", "TEAM_NEUTRAL"),
    "misplaced": ("错位者", "TEAM_NEUTRAL"),
    "moran": ("莫兰上校", "TEAM_NEUTRAL"),
    "moresetting": ("更多设置", "TEAM_NEUTRAL"),
    "moriarty": ("莫里亚蒂", "TEAM_NEUTRAL"),
    "nariefox": ("那列狐", "TEAM_NEUTRAL"),
    "neet": ("躺平者", "TEAM_NEUTRAL"),
    "newsanchor": ("新闻主持人", "TEAM_IMPOSTOR"),
    "nicesplicer": ("折跃者", "TEAM_NEUTRAL"),
    "nightjar": ("夜鹰", "TEAM_NEUTRAL"),
    "ninja": ("忍者", "TEAM_IMPOSTOR"),
    "nocturnal": ("夜行", "TEAM_IMPOSTOR"),
    "notarized": ("被公证", "TEAM_CREW"),
    "notary": ("公证人", "TEAM_CREW"),
    "oblivion": ("遗忘", "TEAM_NEUTRAL"),
    "oblivioncrew": ("遗忘船员", "TEAM_CREW"),
    "oblivionwolf": ("遗忘伪装者", "TEAM_IMPOSTOR"),
    "obsession": ("执念", "TEAM_NEUTRAL"),
    "ouroboros": ("衔尾蛇", "TEAM_NEUTRAL"),
    "outlawleader": ("教父", "TEAM_IMPOSTOR"),
    "pasteraser": ("往抹师", "TEAM_NEUTRAL"),
    "pastguesser": ("往赌师", "TEAM_NEUTRAL"),
    "pelican": ("鹈鹕", "TEAM_NEUTRAL"),
    "pessimist": ("残念先生", "TEAM_IMPOSTOR"),
    "piggy": ("猪猪", "TEAM_NEUTRAL"),
    "pollyanna": ("幸福小姐", "TEAM_IMPOSTOR"),
    "predestined": ("命运少女", "TEAM_IMPOSTOR"),
    "pursuer": ("起诉人", "TEAM_NEUTRAL"),
    "pyromaniac": ("纵火狂", "TEAM_NEUTRAL"),
    "ranger": ("巡林者", "TEAM_IMPOSTOR"),
    "raven": ("渡鸦", "TEAM_NEUTRAL"),
    "recall": ("回忆", "TEAM_NEUTRAL"),
    "reconnecttester": ("断线测试", "TEAM_NEUTRAL"),
    "reindeer": ("驯鹿", "TEAM_IMPOSTOR"),
    "remix": ("混响", "TEAM_NEUTRAL"),
    "remixannouncer": ("混响播音员", "TEAM_NEUTRAL"),
    "remixecho": ("混音回响", "TEAM_NEUTRAL"),
    "remixhost": ("混响主持人", "TEAM_NEUTRAL"),
    "remixpuller": ("混响牵引者", "TEAM_NEUTRAL"),
    "safetyadmin": ("安全操作管理员", "TEAM_NEUTRAL"),
    "samedream": ("同魇", "TEAM_NEUTRAL"),
    "schrodingercat": ("薛定谔的猫", "TEAM_NEUTRAL"),
    "scrambler": ("扰频", "TEAM_NEUTRAL"),
    "seluch": ("掘墓者", "TEAM_NEUTRAL"),
    "sending": ("揭示者", "TEAM_NEUTRAL"),
    "shifter": ("普通交换师", "TEAM_CREW"),
    "skinner": ("斯金纳", "TEAM_NEUTRAL"),
    "skinnerdog": ("斯金纳的狗", "TEAM_NEUTRAL"),
    "solarapostle": ("曦日使徒", "TEAM_NEUTRAL"),
    "sommelier": ("鉴酒师", "TEAM_CREW"),
    "soulchaser": ("追魂师", "TEAM_CREW"),
    "soulrefiner": ("炼魂师", "TEAM_IMPOSTOR"),
    "speecheater": ("噬言者", "TEAM_IMPOSTOR"),
    "speedster": ("闪电侠", "TEAM_NEUTRAL"),
    "spined": ("被缠绕", "TEAM_NEUTRAL"),
    "spinner": ("编织者", "TEAM_NEUTRAL"),
    "star": ("明星", "TEAM_NEUTRAL"),
    "stillheart": ("止心", "TEAM_NEUTRAL"),
    "summoned": ("恶灵", "TEAM_IMPOSTOR"),
    "sunglass": ("近视眼", "TEAM_NEUTRAL"),
    "testament": ("遗愿", "TEAM_NEUTRAL"),
    "trailecho": ("回声", "TEAM_NEUTRAL"),
    "traveler": ("旅行家", "TEAM_IMPOSTOR"),
    "tunny": ("金枪鱼", "TEAM_NEUTRAL"),
    "uniter": ("统票者", "TEAM_CREW"),
    "veteran": ("老兵", "TEAM_CREW"),
    "viewer": ("群演观众", "TEAM_NEUTRAL"),
    "vigilante": ("私刑官", "TEAM_CREW"),
    "violator": ("违规员", "TEAM_NEUTRAL"),
    "watching": ("窥视者", "TEAM_NEUTRAL"),
    "whisperweaver": ("呓语者", "TEAM_NEUTRAL"),
    "whitewolfking": ("白狼王", "TEAM_IMPOSTOR"),
    "yandere": ("病娇", "TEAM_NEUTRAL"),
    "yanderelover": ("病娇的宝贝", "TEAM_NEUTRAL"),
    "zombie": ("僵尸", "TEAM_NEUTRAL"),
}
ADDON_EXTRA_TEAM = {k: v[1] for k, v in ADDON_EXTRA.items()}

def team_of(rid):
    if rid in ADDON_TEAM:      return ADDON_TEAM[rid]       # addons 扩展职业优先
    if rid in SUPP_TEAM:       return SUPP_TEAM[rid]        # 汉化包未覆盖的内置职业
    if rid in ADDON_EXTRA_TEAM: return ADDON_EXTRA_TEAM[rid] # addons 职业
    if rid in IMPOSTOR: return "TEAM_IMPOSTOR"
    if rid in NEUTRAL:  return "TEAM_NEUTRAL"
    return "TEAM_CREW"

# 合并 addons 扩展职业（中文名）
for _k, _v in ADDON_ROLES.items():
    roles.setdefault(_k, _v[0])
# 合并汉化包未覆盖的内置职业（中文名暂译，可被插件词条覆盖）
for _k, _v in SUPPLEMENT.items():
    roles.setdefault(_k, _v[0])
# 合并 addons 职业（汉化包权威中文名）
for _k, _v in ADDON_EXTRA.items():
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
