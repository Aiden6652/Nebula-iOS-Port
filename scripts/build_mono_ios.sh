#!/usr/bin/env bash
# 取 Microsoft 官方预编译的 iOS arm64 Mono 运行时（MonoVM / .NET 8）
#
# 为什么不再从源码编 Mono 6.12（godot-mono-builds 那套）：
#   1) 跑不动目标框架 —— 实测 BepInEx.Unity.IL2CPP.dll / Il2CppInterop.Runtime.dll /
#      NebulaLoader.dll 都是 .NETCoreApp 6.0，Nebula.dll 是 net6.0/net8.0。
#      Mono 6.12 只支持到 netstandard2.0，加载 net6 程序集会直接炸。
#   2) 从源码编要 15 分钟，还一堆 iOS 平台坑（例如 system() 在 iOS SDK 标为
#      unavailable，Mono 里多处调用，得逐个打桩）。
#
# 官方包 Microsoft.NETCore.App.Runtime.Mono.ios-arm64 直接给齐：
#   - runtimes/ios-arm64/native/libmonosgen-2.0.a      （静态，20MB）
#   - runtimes/ios-arm64/native/libmonosgen-2.0.dylib   （动态，5.2MB）
#   - runtimes/ios-arm64/native/include/mono-2.0/       （头文件，88 个）
#   - runtimes/ios-arm64/native/System.Private.CoreLib.dll
#   - runtimes/ios-arm64/lib/net8.0/*.dll               （完整 .NET 8 BCL）
set -euo pipefail

MONOVM_VERSION="${MONOVM_VERSION:-8.0.32}"
WORK="${WORK:-$HOME/mono-ios-build}"
INSTALL="${INSTALL:-$HOME/mono-installs}"
DEST="$INSTALL/ios-monovm"
MANIFEST="${MANIFEST:-$HOME/mono_ios_manifest.txt}"

PKG="microsoft.netcore.app.runtime.mono.ios-arm64"
URL="https://api.nuget.org/v3-flatcontainer/$PKG/$MONOVM_VERSION/$PKG.$MONOVM_VERSION.nupkg"

echo "=== [1/3] 下载 $PKG $MONOVM_VERSION ==="
mkdir -p "$WORK"
curl -sL --max-time 300 -o "$WORK/monoios.nupkg" "$URL"
echo "    $(du -h "$WORK/monoios.nupkg" | cut -f1)  $WORK/monoios.nupkg"

echo "=== [2/3] 解包 runtimes/ios-arm64/ ==="
rm -rf "$DEST"; mkdir -p "$DEST"
python3 - "$WORK/monoios.nupkg" "$DEST" <<'PY'
import zipfile, sys, os
z = zipfile.ZipFile(sys.argv[1]); dest = sys.argv[2]
prefix = 'runtimes/ios-arm64/'
n = 0
for i in z.infolist():
    if i.is_dir() or not i.filename.startswith(prefix):
        continue
    rel = i.filename[len(prefix):]
    out = os.path.join(dest, rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(z.read(i))
    n += 1
print("    解出 %d 个文件 -> %s" % (n, dest))
PY

echo "=== [3/3] 产物清单 ==="
{
  echo "MONOVM_VERSION=$MONOVM_VERSION"
  echo "DEST=$DEST"
  echo "--- include ---"; find "$DEST" -type d -name mono -path "*include*" 2>/dev/null | head -3
  echo "--- 运行时库 ---"; find "$DEST/native" -maxdepth 1 -name "libmonosgen*" 2>/dev/null
  echo "--- CoreLib ---"; ls -la "$DEST/native/System.Private.CoreLib.dll" 2>/dev/null | awk '{print $5, $9}'
  echo "--- BCL 数量 ---"; ls "$DEST/lib/net8.0" 2>/dev/null | wc -l
} | tee "$MANIFEST"

echo "OK: MonoVM(ios-arm64) 就位 -> $DEST"
