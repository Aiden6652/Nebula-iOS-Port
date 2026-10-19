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

def team_of(rid):
    if rid in IMPOSTOR: return "TEAM_IMPOSTOR"
    if rid in NEUTRAL:  return "TEAM_NEUTRAL"
    return "TEAM_CREW"

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
out_h.append("int  nebula_role_id_by_key(const char *key);   /* 官方英文 id -> 枚举值，如 sheriff */")
out_h.append("")
out_h.append("#endif /* NEBULA_ROLES_H */")

out_c = ["/* roles.c — 官方职业表（自动生成） */",
         '#include "roles.h"',
         '#include <string.h>',
         "",
         "typedef struct { int id; const char *key; const char *name; nebula_team_t team; } role_row_t;",
         "",
         "static const role_row_t ROLES[] = {"]
for rid in ids:
    out_c.append('    { %-30s "%s", "%s", %s },' % (enum_name[rid] + ",", rid, roles[rid], team_of(rid)))
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
