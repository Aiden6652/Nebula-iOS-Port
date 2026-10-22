#!/usr/bin/env bash
# 从源码编译 mono for iOS arm64 —— 替换原「下载预编译包」方案。
#
# 为什么换（根因见真机 .ips 崩溃栈）：
#   原 Microsoft.NETCore.App.Runtime.Mono.ios-arm64 包里的 libmonosgen 在
#     mono_jit_init -> mono_arch_init -> mini_get_breakpoint_trampoline
#       -> mono_arch_create_sdb_trampoline -> mono_assertion_message_unreachable
#   触发断言崩溃。iOS 不允许运行时 mmap 可执行 trampoline，该官方包把 sdb/breakpoint
#   trampoline 编成了 unreachable stub。自编译走 mono 官方 iOS 构建（信号 trampoline
#   路径），mono_arch_init 不会走到 unreachable，mono_jit_init 能正常初始化。
#
# 注意：BCL(.NET 8 托管程序集) 仍然从官方 nuget 取，不自己编（版本匹配即可）；
#       本脚本只从源码编译「原生运行时」(libmonosgen + 头文件)。
#
# 产出（映射到 build_ios.sh 期望的 prefix 结构）：
#   $INSTALL/include/mono-2.0/...    头文件
#   $INSTALL/lib/*.a                 静态运行时（libmonosgen-2.0 等）
#   $INSTALL/lib/net8.0/*.dll        .NET 8 BCL（来自 nuget）
set -euo pipefail

MONO_REF="${MONO_REF:-dotnet/main}"
MONOVM_VERSION="${MONOVM_VERSION:-8.0.32}"
WORK="${WORK:-$HOME/mono-src}"
INSTALL="${INSTALL:-$HOME/mono-installs}"
MANIFEST="${MANIFEST:-$HOME/mono_ios_manifest.txt}"
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

echo "=== [1/5] 环境 ==="
echo "Xcode: $(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null || echo 'NO iOS SDK')"
echo "clang: $(clang --version 2>/dev/null | head -1)"

echo "=== [2/5] 克隆 mono 源码 ($MONO_REF) ==="
rm -rf "$WORK"; mkdir -p "$(dirname "$WORK")"
# 分支回退：dotnet/main 是 mono 合入 .NET 后的开发分支；若上游不存在则退 main
if ! git clone --recursive --depth 1 --branch "$MONO_REF" https://github.com/mono/mono "$WORK/mono"; then
  for fb in main master; do
    echo "    '$MONO_REF' 不存在，回退尝试 '$fb' ..."
    git clone --recursive --depth 1 --branch "$fb" https://github.com/mono/mono "$WORK/mono" && MONO_REF="$fb" && break
  done
fi
[ -d "$WORK/mono" ] || { echo "!! mono 源码克隆全部失败" >&2; exit 1; }
echo "    mono 源码就位: $(du -sh "$WORK/mono" 2>/dev/null | cut -f1)"

echo "=== [3/5] 编译 iOS arm64（官方 sdks 构建，信号 trampoline 路径）==="
cd "$WORK/mono"
# sdks 系统是 mono 官方对 iOS 的构建入口，已处理 iOS 的 unavailable API 打桩与
# trampoline/信号路径。只构建 ios-arm64 device runtime，避免 simulator/多 config 拖时长。
export MONO_BUILD_DIR="$WORK/build"
set +e
make -C sdks build-ios 2>&1 | tail -40
RC=${PIPESTATUS[0]}
set -e
if [ "$RC" -ne 0 ]; then
  echo "!! sdks build-ios 失败，尝试根 Makefile 的 ios target ..."
  make build-ios 2>&1 | tail -40
fi

echo "=== [4/5] 定位并拷贝 头文件 + 静态库 到 $INSTALL ==="
rm -rf "$INSTALL"; mkdir -p "$INSTALL/include" "$INSTALL/lib"

# 头文件：include/mono-2.0（可能嵌套在 sdks 产出里）
INC="$(find "$WORK/build" -type d -name 'mono-2.0' -path '*include*' 2>/dev/null | head -1)"
if [ -z "$INC" ]; then
  # fallback：mono 源码自带头文件
  INC="$(find "$WORK/mono" -type d -name 'mono-2.0' -path '*include*' 2>/dev/null | head -1)"
fi
if [ -n "$INC" ]; then
  cp -R "$INC" "$INSTALL/include/"
  echo "    头文件: $INC -> $INSTALL/include/mono-2.0"
else
  echo "!! 未找到 mono-2.0 头文件目录"
fi

# 静态库：libmonosgen-2.0.a 等
find "$WORK/build" -name 'libmonosgen-2.0.a' 2>/dev/null | head -1 | xargs -I{} cp {} "$INSTALL/lib/" 2>/dev/null || true
find "$WORK/build" -name 'libmono-*.a' 2>/dev/null | while read -r f; do cp "$f" "$INSTALL/lib/"; done
echo "    静态库: $(ls "$INSTALL/lib" 2>/dev/null | wc -l) 个"

echo "=== [5/5] 取 .NET 8 BCL + CoreLib（托管程序集，不自己编）==="
mkdir -p "$INSTALL/lib/net8.0"
PKG="microsoft.netcore.app.runtime.mono.ios-arm64"
URL="https://api.nuget.org/v3-flatcontainer/$PKG/$MONOVM_VERSION/$PKG.$MONOVM_VERSION.nupkg"
curl -sL --max-time 300 -o "$WORK/monoios.nupkg" "$URL"
python3 - "$WORK/monoios.nupkg" "$INSTALL" <<'PY'
import zipfile, sys, os
z = zipfile.ZipFile(sys.argv[1]); inst = sys.argv[2]
n = 0
for i in z.infolist():
    if i.is_dir(): continue
    if i.filename.startswith('runtimes/ios-arm64/lib/net8.0/'):
        rel = i.filename[len('runtimes/ios-arm64/lib/net8.0/'):]
        out = os.path.join(inst, 'lib', 'net8.0', rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, 'wb').write(z.read(i)); n += 1
    if i.filename.endswith('runtimes/ios-arm64/native/System.Private.CoreLib.dll'):
        open(os.path.join(inst, 'lib', 'net8.0', 'System.Private.CoreLib.dll'), 'wb').write(z.read(i)); n += 1
print("    BCL 提取 %d 个文件" % n)
PY

{
  echo "MONO_REF=$MONO_REF"
  echo "MONOVM_VERSION=$MONOVM_VERSION"
  echo "INSTALL=$INSTALL"
  echo "--- include/mono ---"; find "$INSTALL/include" -maxdepth 2 -name 'jit.h' 2>/dev/null | head -1
  echo "--- 静态库 ---"; ls "$INSTALL/lib" 2>/dev/null | head
  echo "--- BCL 数量 ---"; ls "$INSTALL/lib/net8.0" 2>/dev/null | wc -l
} | tee "$MANIFEST"

echo "OK: 自编译 mono iOS 就位 -> $INSTALL"
