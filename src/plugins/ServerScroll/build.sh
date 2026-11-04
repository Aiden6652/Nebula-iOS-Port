#!/usr/bin/env bash
# 重新编译 AUI.ServerScroll.dll（[R60] 服务器列表滑动/拉长补丁）
#
# 依赖：本机 dotnet 6 SDK（csc.dll） + 仓库内 src/mono_host/prebuilt-interop/(131 个
#       17.3.1o iOS 桥接 dll) + src/mono_host/bepinex-core/(BepInEx 6 + HarmonyX)。
# 说明：这些 interop 引用必须与设备上随包发布的 BepInEx/interop 同版本，否则插件
#       加载会 TypeLoadException。产物是纯 IL 托管 dll，随包放进 BepInEx/plugins/。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MONO="$ROOT/src/mono_host"

CSC="$(find /usr/share/dotnet /usr/lib/dotnet -name csc.dll -path '*Roslyn/bincore*' 2>/dev/null | head -1 || true)"
if [ -z "$CSC" ]; then echo "!! 没找到 Roslyn csc.dll（需 dotnet 6 SDK）" >&2; exit 1; fi
REFDIR="$(ls -d /usr/share/dotnet/packs/Microsoft.NETCore.App.Ref/*/ref/net6.0 2>/dev/null | head -1 || true)"
if [ -z "$REFDIR" ]; then echo "!! 没找到 net6.0 参考程序集" >&2; exit 1; fi

REFS=()
for f in "$MONO"/prebuilt-interop/*.dll "$MONO"/bepinex-core/*.dll "$REFDIR"/*.dll; do
  REFS+=("-r:$f")
done
echo "引用 ${#REFS[@]} 个程序集"
dotnet "$CSC" -nologo -target:library -langversion:latest \
  -out:"$HERE/AUI.ServerScroll.dll" "${REFS[@]}" "$HERE/ServerScroll.cs"
echo "OK -> $HERE/AUI.ServerScroll.dll ($(wc -c < "$HERE/AUI.ServerScroll.dll") bytes)"
