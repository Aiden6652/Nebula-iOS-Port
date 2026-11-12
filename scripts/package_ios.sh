#!/usr/bin/env bash
# 打包最终成品：一套可以直接塞进 iOS 版 Among Us 根目录的东西
#
# 用法:
#   bash scripts/package_ios.sh <宿主产物目录 out-ios> <NOS改装包.zip> <输出目录>
#
# 产物结构（放进 AU 根目录后，dylib 被注入即生效）:
#   libnebula_standard.dylib        宿主（单一编译器/JIT 版，内含 MonoVM）
#   bcl/                            .NET 8 基础类库
#   BepInEx/                        框架 + 插件 + 配置 + unity-libs
#   NebulaLibs/                     星云舰依赖
#   Addons/                         全部附加组件（职业在里面，必须全带）
#   nebula_host.cfg                 运行时配置（统一编译器版）
#   entitlements.plist              重签名时用（要开 JIT）
set -euo pipefail

ART="${1:?用法: package_ios.sh <out-ios目录> <NOS改装包.zip> <输出目录>}"
NOS="${2:?缺 NOS 改装包 zip}"
OUT="${3:?缺输出目录}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== 打包 -> $OUT ==="
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== [1/5] 宿主 dylib（BCL + BepInEx/core 已内嵌，无散文件）==="
cp "$ART/libnebula_standard.dylib" "$OUT/"
echo "    dylib $(du -h "$OUT/libnebula_standard.dylib" | cut -f1)（BCL + BepInEx.core 已烧进 __nebulabcl 段）"

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
# 注意：BepInEx/interop 必须 SKIP —— NOS 改装包自带的是【PC/Windows 版】interop
#（含 Rewired_Windows.dll 等 Windows 专属桥接），绝不能上 iOS 设备。iOS 版 interop
# 由下面的 [2.5/5] 从仓库 src/mono_host/prebuilt-interop/ 专门拷贝覆盖。
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

# [R95c] 不再拷 nebula_corelibs 进 NebulaLibs：宿主 R80 启动时已把 blob 里的
#   全套 BCL（含 System.Private.CoreLib.dll）释放到 <HOME>/Documents/BepInEx/core，
#   注入的 Cecil resolver 指的就是那个目录。交付保持单一 dylib，无额外散文件。

echo "=== [2.5/5] 预生成 iOS 版 interop（随包发布，设备端不再现场生成）==="
# R58 教训：iPad 现场跑 Cpp2IL 分析 ~96MB UnityFramework 会卡死/整夜跑不完 → 永久黑屏。
# 这里直接拷入 PC 端针对 17.3.1o 预生成的完整 interop（与 dylib 内 baked 的
# BepInEx/core 同版本 Il2CppInterop 产出），设备端只加载复用、绝不重新生成。
PREBUILT="$ROOT/src/mono_host/prebuilt-interop"
if [ -d "$PREBUILT" ]; then
  # 兜底：先清空可能从 NOS 包漏入的 PC 版 interop（NOS 包自带 Windows 版，绝不能上 iOS）
  rm -rf "$OUT/BepInEx/interop"
  mkdir -p "$OUT/BepInEx/interop"
  cp -a "$PREBUILT/." "$OUT/BepInEx/interop/"
  N_DLL=$(ls "$OUT/BepInEx/interop/" | grep -c '\.dll$' || true)
  echo "    已植入预生成 interop: $N_DLL 个 dll（版本锁定 17.3.1o，勿替换为 PC 版）"
else
  echo "    ⚠ 未找到 $PREBUILT —— 未随包发布 interop（R60 dylib 内已内嵌，仍可运行）"
fi

echo "=== [2.6/5] 服务器列表滑动补丁 + 单一 dylib 校验 ==="
# [R60] 用户私服太多，原版下拉一屏只放得下 3 个、其余飞出屏幕。
#   插件用 Harmony patch ServerDropdown.OpenServerDropdown，拉长 viewport/content 并
#   强制竖向滚动；全 try/catch，失败也不影响 NOS。
PLUGIN="$ROOT/src/plugins/ServerScroll/AUI.ServerScroll.dll"
if [ -f "$PLUGIN" ]; then
  mkdir -p "$OUT/BepInEx/plugins"
  cp "$PLUGIN" "$OUT/BepInEx/plugins/"
  echo "    已植入 AUI.ServerScroll.dll（服务器列表滑动/拉长）"
else
  echo "    ⚠ 未找到 $PLUGIN（可跑 bash src/plugins/ServerScroll/build.sh 重新编译）"
fi
NDYLIB="$(find "$OUT" -name '*.dylib' | wc -l | tr -d ' ')"
echo "    包内原生 dylib 数: $NDYLIB（R60 必须 = 1：dobby + 5 个 libSystem shim 全静态并入主 dylib）"
[ "$NDYLIB" = "1" ] || { echo "!! R60 要求只交付一个 dylib，实测 $NDYLIB 个" >&2; exit 1; }

echo "=== [3/5] 配置 ==="
cp "$ROOT/src/mono_host/nebula_host.standard.cfg" "$OUT/nebula_host.cfg"
echo "    默认生效: nebula_host.cfg（统一编译器/JIT 版配置）"

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
# 安装（iOS 版 Among Us）— R60

1. 把本目录**整个内容**放进 Among Us 的可写根目录（与 \`AmongUs.app\` 同级或你 dylib 注入点同级）。
2. 用你的注入方式加载 \`libnebula_standard.dylib\`。它会：
   - 起 MonoVM → 加载 \`BepInEx/core/BepInEx.Unity.IL2CPP.dll\` → 调 \`Doorstop.Entrypoint.Start()\`
   - 之后由 BepInEx 接管，加载 \`BepInEx/plugins/\` 里的插件（含星云舰）
   - **启动强制播放 \`AUI Mod Loader by Aiden\` 满 3 秒**（品牌页，播完自动消失）
3. 单一编译器（JIT）版：解释器已废除，始终走 JIT，需要 \`dynamic-codesigning\` 权限
   （由 StikDebug / 全能签 entitlements 提供），否则 mono 初始化时会被内核秒杀。

## R60 相对 R59 的变化
- **只有一个 dylib**：iOS arm64 \`Dobby\` 与 5 个 \`libSystem.*\` CoreLib shim 全部
  静态并入 \`libnebula_standard.dylib\`。修掉了真机 \`DllNotFoundException: dobby\`
  （原 \`core/dobby.dll\` 是 Windows x64 PE）导致的 chainloader 中断与播完 Innersloth 后 SIGSEGV。
- **interop 不用手动放**：131 个 iOS 桥接 dll 已烤进主 dylib，启动时自动释放到
  \`BepInEx/interop/\`，目录恒非空 → 设备端**永不现场生成**（R58 整夜卡死黑屏的根因）。
- **服务器列表滑动**：\`BepInEx/plugins/AUI.ServerScroll.dll\` 把原本一屏只放 3 个的
  服务器下拉列表拉长并启用竖向滚动（全程 try/catch，不影响 NOS）。
- 启动时若发现 Documents 里的 Windows 版 \`core/dobby.dll\`，会自动改名为 \`.win.bak\`。

## 目录
| 目录/文件 | 作用 |
|---|---|
| \`libnebula_standard.dylib\` | **唯一**原生库：内含 MonoVM + BCL + BepInEx.core + 预生成 interop + arm64 Dobby + CoreLib shim |
| \`BepInEx/plugins/\` | BepInEx 插件（星云舰加载器 + AUI.ServerScroll.dll）—— **新模组扔这里** |
| \`BepInEx/interop/\` | Il2CppInterop 桥接层（PC 针对 17.3.1o 预生成的 iOS 版；**dylib 内已内嵌，此份为双保险，无需手动放**；勿替换为 PC 版） |
| \`BepInEx/config/\` | 配置 |
| \`NebulaLibs/\` | 星云舰依赖 |
| \`Addons/\` | 附加组件（职业定义在里面，默认全加载） |
| \`Mods/\` | 非 BepInEx 的普通 C# 模组扔这里（可选） |

## 环境变量
- \`NEBULA_GAME_DIR\`  游戏根目录（BepInEx 所在处）
- \`NEBULA_BOOT\`      bepinex / mods / engine / auto（默认 auto）
- \`NEBULA_BOOT_OVERLAY_MIN_MS\`  品牌页最短展示毫秒数（默认 3000）
- \`NEBULA_FORCE_MANUAL_EXECUTE=1\`  设备端 dobby inline hook 失败时，恢复 R56 兜底触发 Execute（对照用）
EOF
echo "    已写入 INSTALL.md"

echo
echo "=== 完成 ==="
du -sh "$OUT" 2>/dev/null
find "$OUT" -maxdepth 1 | sed 's|^|    |'
