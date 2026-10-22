#!/usr/bin/env bash
# 打包最终成品：一套可以直接塞进 iOS 版 Among Us 根目录的东西
#
# 用法:
#   bash scripts/package_ios.sh <宿主产物目录 out-ios> <NOS改装包.zip> <输出目录> [standard|pro]
#
# 产物结构（放进 AU 根目录后，dylib 被注入即生效）:
#   libnebula_mono_host.dylib       宿主（内含 MonoVM）
#   bcl/                            .NET 8 基础类库
#   BepInEx/                        框架 + 插件 + 配置 + unity-libs
#   NebulaLibs/                     星云舰依赖
#   Addons/                         全部附加组件（职业在里面，必须全带）
#   nebula_host.<profile>.cfg       运行时配置
#   entitlements.plist              重签名时用（Pro 版要开 JIT）
set -euo pipefail

ART="${1:?用法: package_ios.sh <out-ios目录> <NOS改装包.zip> <输出目录> [standard|pro]}"
NOS="${2:?缺 NOS 改装包 zip}"
OUT="${3:?缺输出目录}"
PROFILE="${4:-standard}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== 打包 profile=$profile -> $OUT ===" 2>/dev/null
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== [1/5] 宿主 dylib（BCL + BepInEx/core 已内嵌，无散文件）==="
cp "$ART/libnebula_mono_host.dylib" "$OUT/"
echo "    dylib $(du -h "$OUT/libnebula_mono_host.dylib" | cut -f1)（BCL + BepInEx.core 已烧进 __nebulabcl 段）"

echo "=== [2/5] 从改装包提取模组文件 ==="
python3 - "$NOS" "$OUT" <<'PY'
import zipfile, os, sys, collections
z = zipfile.ZipFile(sys.argv[1]); out = sys.argv[2]
names = z.namelist()
# 顶层前缀（改装包一般套一层目录，如 星云3413/）
pref = ""
if names and names[0].count('/') >= 1:
    cand = names[0].split('/')[0] + '/'
    if sum(1 for n in names if n.startswith(cand)) > len(names) * 0.8:
        pref = cand
print("    包内前缀:", repr(pref))

KEEP_DIRS = ['BepInEx/', 'NebulaLibs/', 'Addons/', 'MoreCosmic/', 'Cache/', 'LobbyRules/', 'Replace/']
# PC 专用、iOS 用不到或本就属于游戏的，一律不带
SKIP_NAMES = {'winhttp.dll', 'doorstop_config.ini', 'GameAssembly.dll',
              'UnityPlayer.dll', 'UnityCrashHandler32.exe', 'Among Us.exe',
              'baselib.dll', 'opus.dll', 'msvcp140.dll', 'vcruntime140.dll',
              'dobby.dll'}
SKIP_DIRS = ['dotnet/', 'MonoBleedingEdge/', 'UnityPlayer_Data/', 'BepInEx/core/', 'BepInEx/interop/']

kept = collections.Counter()
for n in names:
    if not n.startswith(pref):
        continue
    rel = n[len(pref):]
    if not rel or not any(rel.startswith(d) for d in KEEP_DIRS):
        continue
    if any(rel.startswith(d) or ('/' + d) in rel for d in SKIP_DIRS):
        continue
    base = os.path.basename(rel)
    if base in SKIP_NAMES or base.endswith('.exe'):
        continue
    dst = os.path.join(out, rel)
    if rel.endswith('/'):
        os.makedirs(dst, exist_ok=True); continue
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, 'wb') as f:
        f.write(z.read(n))
    kept[rel.split('/')[0]] += 1

for k, v in kept.items():
    print(f"    {k}: {v} 个文件")
PY

echo "=== [3/5] 配置 ==="
cp "$ROOT/src/mono_host/nebula_host.standard.cfg" "$OUT/"
cp "$ROOT/src/mono_host/nebula_host.pro.cfg" "$OUT/"
cp "$OUT/nebula_host.$PROFILE.cfg" "$OUT/nebula_host.cfg"
echo "    默认生效: nebula_host.cfg (= $PROFILE)"

echo "=== [3.5/5] 关掉星云舰联网自动更新 ==="
# 加载器默认会联网拉最新版，iOS 上要么没网要么被墙，会卡住/失败。
CFG="$OUT/BepInEx/config/jp.dreamingpig.amongus.nebula.loader.cfg"
if [ -f "$CFG" ]; then
python3 - "$CFG" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
for k in ['AutoUpdate', 'AutoUpdateIfVersionMismatching', 'AllowHttpCommunication']:
    s = re.sub(r'(?m)^%s\s*=\s*\S+' % k, '%s = false' % k, s)
open(p, 'w', encoding='utf-8').write(s)
print("    AutoUpdate / AutoUpdateIfVersionMismatching / AllowHttpCommunication = false")
PY
else
  echo "    (未找到加载器配置，跳过)"
fi

echo "=== [4/5] entitlements ==="
cp "$ROOT/tools/entitlements-jit.plist" "$OUT/entitlements.plist"
echo "    已写入 entitlements.plist"

echo "=== [5/5] 安装说明 ==="
cat > "$OUT/INSTALL.md" <<EOF
# 安装（iOS 版 Among Us）

1. 把本目录**整个内容**放进 Among Us 的可写根目录（与 \`AmongUs.app\` 同级或你 dylib 注入点同级）。
2. 用你的注入方式加载 \`libnebula_mono_host.dylib\`。它会：
   - 起 MonoVM → 加载 \`BepInEx/core/BepInEx.Unity.IL2CPP.dll\` → 调 \`Doorstop.Entrypoint.Start()\`
   - 之后由 BepInEx 接管，加载 \`BepInEx/plugins/\` 里的插件（含星云舰）
3. 版本选择：
   - \`standard\`（默认）：纯解释器，不需要任何权限
   - \`pro\`：强制 JIT，需要 \`dynamic-codesigning\` + \`increased-memory-limit\`
   切换：设置环境变量 \`NEBULA_PROFILE=pro\`，或直接改 \`nebula_host.cfg\`。

## 目录
| 目录/文件 | 作用 |
|---|---|
| \`libnebula_mono_host.dylib\` | 宿主（内含 MonoVM + BCL + BepInEx.core，自包含，无散 dll） |
| \`BepInEx/plugins/\` | BepInEx 插件（星云舰加载器等）—— **新模组扔这里** |
| \`BepInEx/interop/\` | Il2CppInterop 包装层（**首次启动自动生成 iOS 版**；不要带 PC 版） |
| \`BepInEx/config/\` | 配置 |
| \`NebulaLibs/\` | 星云舰依赖 |
| \`Addons/\` | 附加组件（职业定义在里面，默认全加载） |
| \`Mods/\` | 非 BepInEx 的普通 C# 模组扔这里（可选） |

## 环境变量
- \`NEBULA_GAME_DIR\`  游戏根目录（BepInEx 所在处）
- \`NEBULA_BOOT\`      bepinex / mods / engine / auto（默认 auto）
- \`NEBULA_PROFILE\`   standard / pro
EOF
echo "    已写入 INSTALL.md"

echo
echo "=== 完成 ==="
du -sh "$OUT" 2>/dev/null
find "$OUT" -maxdepth 1 | sed 's|^|    |'
