#!/usr/bin/env bash
# 打包最终成品（LEGACY MONO 版）：可直接塞进【老版纯 Mono Among Us】根目录的东西
#
# 用法:
#   bash scripts/package_ios_mono.sh <宿主产物目录 out-ios> [<NOS-Mono改装包.zip>] <输出目录>
#
# 产物结构（放进老 AU Mono 版根目录后，dylib 被注入即生效）:
#   libnebula_standard.dylib        宿主（内含 MonoVM + BCL + BepInEx5 Mono core + 游戏 managed）
#   bcl/                            .NET 8 基础类库（散文件兜底，dylib 已内嵌，可选）
#   BepInEx/                        框架 + 插件（NOS Mono 版放 plugins/）
#   nebula_host.cfg                 运行时配置
#   entitlements.plist              重签名时用（要开 JIT）
#
# 与 IL2CPP 版(package_ios.sh)的区别：
#   - 不需要预生成 interop（Mono AU 是真实托管 IL，无 Il2CppInterop）
#   - 不需要 unity-libs（Il2CppInterop 生成器参考库）
#   - 游戏 managed 程序集(Assembly-CSharp.dll 等)已内嵌进 dylib 的 game-managed/ 段
#   - 入口是 BepInEx5 Preloader.Main(string[]) 而非 IL2CPP 的 Doorstop.Entrypoint.Start
set -euo pipefail

ART="${1:?用法: package_ios_mono.sh <out-ios目录> [<NOS-Mono改装包.zip>] <输出目录>}"
# 第二个参数若以 .zip 结尾视为 NOS 包，否则当作输出目录（NOS 可缺省）
if [[ "${2:-}" == *.zip ]]; then
  NOS="${2}"; OUT="${3:?缺输出目录}"
else
  NOS=""; OUT="${2:?缺输出目录}"
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== 打包(LEGACY MONO) -> $OUT ==="
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== [1/5] 宿主 dylib（BCL + BepInEx5 core + 游戏 managed 已内嵌，无散文件）==="
cp "$ART/libnebula_standard.dylib" "$OUT/"
echo "    dylib $(du -h "$OUT/libnebula_standard.dylib" | cut -f1)（__nebulabcl 段已烧入 BepInEx5 + game-managed）"

echo "=== [2/5] 从 NOS-Mono 改装包提取插件（可选）==="
if [ -n "$NOS" ] && [ -f "$NOS" ]; then
python3 - "$NOS" "$OUT" <<'PY'
import zipfile, os, sys, collections
z = zipfile.ZipFile(sys.argv[1]); out = sys.argv[2]
names = z.namelist()
pref = ""
if names and names[0].count('/') >= 1:
    cand = names[0].split('/')[0] + '/'
    if sum(1 for n in names if n.startswith(cand)) > len(names) * 0.8:
        pref = cand
print("    包内前缀:", repr(pref))
# Mono 版只收 BepInEx/plugins 与依赖数据；PC/Windows 原生库一律不要
KEEP_DIRS = ['BepInEx/plugins/', 'BepInEx/config/', 'BepInEx/patchers/', 'NebulaLibs/', 'Addons/']
SKIP_NAMES = {'winhttp.dll', 'GameAssembly.dll', 'UnityPlayer.dll', 'UnityCrashHandler32.exe',
              'Among Us.exe', 'baselib.dll', 'opus.dll', 'msvcp140.dll', 'vcruntime140.dll',
              'dobby.dll', 'doorstop_config.ini', 'UnityPlayer_Data'}
kept = collections.Counter()
for n in names:
    if not n.startswith(pref): continue
    rel = n[len(pref):]
    if not rel or not any(rel.startswith(d) for d in KEEP_DIRS): continue
    base = os.path.basename(rel)
    if base in SKIP_NAMES or base.endswith('.exe'): continue
    dst = os.path.join(out, rel)
    if rel.endswith('/'):
        os.makedirs(dst, exist_ok=True); continue
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(z.read(n))
    kept[rel.split('/')[0]] += 1
for k, v in kept.items():
    print(f"    {k}: {v} 个文件")
PY
else
  echo "    （未提供 NOS-Mono 改装包，跳过；插件可后续手动放进 BepInEx/plugins/）"
fi

echo "=== [3/5] 配置 + 关掉联网自动更新 ==="
cp "$ROOT/src/mono_host/nebula_host.standard.cfg" "$OUT/nebula_host.cfg"
echo "    默认生效: nebula_host.cfg"
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

echo "=== [3.5/5] 单一 dylib 校验 ==="
NDYLIB="$(find "$OUT" -name '*.dylib' | wc -l | tr -d ' ')"
echo "    包内原生 dylib 数: $NDYLIB（LEGACY MONO 必须 = 1）"
[ "$NDYLIB" = "1" ] || { echo "!! 要求只交付一个 dylib，实测 $NDYLIB 个" >&2; exit 1; }

echo "=== [4/5] entitlements ==="
cp "$ROOT/src/mono_host_mono/nebula-entitlements.plist" "$OUT/entitlements.plist"
echo "    已写入 entitlements.plist"

echo "=== [5/5] 安装说明 ==="
cat > "$OUT/INSTALL.md" <<EOF
# 安装（老版纯 Mono Among Us，iOS）— LEGACY MONO 版

1. 把本目录**整个内容**放进老 AU Mono 版的可写根目录（dylib 注入点同级）。
2. 用你的注入方式加载 \`libnebula_standard.dylib\`。它会：
   - 起 MonoVM（iOS JIT，需 \`dynamic-codesigning\` 权限）→ 加载
     \`BepInEx/core/BepInEx.Preloader.dll\` → 调 \`BepInEx.Preloader.Preloader.Main(string[])`
   - 按 \`NEBULA_GAME_ENTRY\`（默认 \`game-managed/Assembly-CSharp.dll\`）加载**游戏托管入口**
   - 之后由 BepInEx5 接管，加载 \`BepInEx/plugins/\` 里的插件（含 NOS Mono 版）
   - **启动强制播放 \`AUI Mod Loader by Aiden\` 满 3 秒**（品牌页，播完自动消失）
3. 老 AU 必须是 **Mono 后端**（~2021 之前）；IL2CPP 版不适用本包。

## 与 IL2CPP 版的区别
- 游戏是真实托管 IL：\`typeof(GameClass)\` 直接可用，Harmony 在真类型上 patch，
  无需 Il2CppInterop 代理层。
- **没有 interop / unity-libs**（那是 IL2CPP 专属）。
- 游戏 managed 程序集（Assembly-CSharp.dll 等）已内嵌进 dylib 的 \`game-managed/\` 段，
  启动时自动释放到 \`\$(root)/game-managed/\`。
- 单一编译器（JIT）版：需 \`dynamic-codesigning\` 权限，否则 mono 初始化被内核秒杀。

## 环境变量
- \`NEBULA_GAME_ENTRY\`  游戏托管入口 dll 路径（默认 \`\$(root)/game-managed/Assembly-CSharp.dll\`）
- \`NEBULA_BOOT_OVERLAY_MIN_MS\`  品牌页最短展示毫秒数（默认 3000）
- \`NEBULA_BLOCK_BOOT=0\`  退回异步 boot（仅对照用）
EOF
echo "    已写入 INSTALL.md"

echo
echo "=== 完成 ==="
du -sh "$OUT" 2>/dev/null
find "$OUT" -maxdepth 1 | sed 's|^|    |'
