#!/usr/bin/env bash
# 交叉编译宿主 dylib → iOS arm64
#
# 用法: bash src/mono_host/build_ios.sh <Mono-iOS-安装前缀>
#
# 产物: out-ios/libnebula_mono_host.dylib
#   带 BUILD_AS_LIB → 挂 __attribute__((constructor))，被游戏 dlopen 后
#   自动 pthread 异步 boot Mono（绝不能在 constructor 里同步初始化，会撞加载器锁死锁）。
#
# 注意: macOS 自带 /bin/bash 是 3.2，没有 mapfile/readarray，所以用 while-read 建数组。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MONO_PREFIX="${1:-$HOME/mono-installs}"
OUT="${OUT:-$ROOT/out-ios}"
MIN_IOS="${MIN_IOS:-13.0}"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CCBIN="$(xcrun --sdk iphoneos -f clang)"

echo "SDK: $SDK"
echo "CC : $CCBIN"
mkdir -p "$OUT"

# 定位 Mono 头文件目录（找到 .../include/mono，再往上取 include）
MONO_INC_DIR="$(find "$MONO_PREFIX" -type d -name mono -path "*include*" 2>/dev/null | head -1 || true)"
if [ -z "$MONO_INC_DIR" ]; then
  echo "!! 没找到 Mono include 目录，检查 $MONO_PREFIX" >&2
  find "$MONO_PREFIX" -maxdepth 3 2>/dev/null | head -30 >&2 || true
  exit 1
fi
MONO_INC="$(dirname "$MONO_INC_DIR")"
echo "MONO_INC: $MONO_INC"

# 收集全部静态库。Mono 不会只出一个 .a，而是拆成好几个：
#   libmonosgen-2.0 / libmono-native / libmono-ee-interp / libmono-ilgen / libmono-icall-table ...
# 少链任何一个都会在链接期报缺符号，所以全链上。
# 注意：不能把 libmono-component-* 全排除。libmonosgen 的 components.c 会引用
#   mono_component_debugger_init / diagnostics_server_init / event_pipe_init /
#   hot_reload_init / marshal_ilgen_init
# 少一个就 Undefined symbol。
# 唯一可以省的是 debugger：有 stub 版（0.06MB）顶替完整版（1.32MB），二选一。
LIBS=()
while IFS= read -r line; do
  case "$(basename "$line")" in
    libmono-component-debugger-static.a) continue ;;  # 用 stub 版
  esac
  [ -n "$line" ] && LIBS+=("$line")
done < <(find "$MONO_PREFIX" -name "*.a" 2>/dev/null)

echo "静态库 ${#LIBS[@]} 个:"
printf '  %s\n' "${LIBS[@]}"

if [ "${#LIBS[@]}" -eq 0 ]; then
  echo "!! 没找到 Mono 静态库，检查 $MONO_PREFIX" >&2
  exit 1
fi

echo "=== 编译 iOS arm64 dylib ==="
"$CCBIN" -arch arm64 \
  -isysroot "$SDK" \
  -miphoneos-version-min="$MIN_IOS" \
  -dynamiclib -fPIC \
  -DBUILD_AS_LIB \
  -I"$MONO_INC" \
  -o "$OUT/libnebula_mono_host.dylib" \
  "$ROOT/nebula_mono_host.c" \
  "${LIBS[@]}" \
  -framework Foundation \
  -framework CoreFoundation \
  -framework Security \
  -lobjc -lc++ -lpthread -lz -lm

# 把 MonoVM 的 BCL 一并拷到产物目录 —— 没有 BCL 这个 dylib 跑不了任何 .NET 代码。
# 注意路径：解包后真实位置是 <prefix>/ios-monovm/lib/net8.0，不是 <prefix>/lib/net8.0，
# 所以要先定位含 lib/net8.0 的那一层。
MONO_ROOT="$MONO_PREFIX"
if [ ! -d "$MONO_ROOT/lib/net8.0" ]; then
  found="$(find "$MONO_PREFIX" -maxdepth 4 -type d -name net8.0 -path "*/lib/*" 2>/dev/null | head -1)"
  [ -n "$found" ] && MONO_ROOT="$(dirname "$(dirname "$found")")"
fi
echo "MONO_ROOT: $MONO_ROOT"

if [ -d "$MONO_ROOT/lib/net8.0" ]; then
  echo "=== 拷贝 .NET 8 BCL ==="
  mkdir -p "$OUT/bcl"
  cp -R "$MONO_ROOT/lib/net8.0/." "$OUT/bcl/"
  [ -f "$MONO_ROOT/native/System.Private.CoreLib.dll" ] \
    && cp "$MONO_ROOT/native/System.Private.CoreLib.dll" "$OUT/bcl/"
  # ICU 数据（全球化/中文相关），有就带上
  [ -f "$MONO_ROOT/native/icudt.dat" ] && cp "$MONO_ROOT/native/icudt.dat" "$OUT/bcl/"
  echo "    BCL 程序集 $(ls "$OUT/bcl" | wc -l) 个"
else
  echo "!! 没找到 lib/net8.0，BCL 未拷贝（dylib 会跑不起来）" >&2
fi

echo "=== 校验 ==="
lipo -info "$OUT/libnebula_mono_host.dylib"
file "$OUT/libnebula_mono_host.dylib" 2>/dev/null || true
echo "--- 导出符号(前 20) ---"
nm -gU "$OUT/libnebula_mono_host.dylib" 2>/dev/null | head -20 || true

echo "OK -> $OUT/libnebula_mono_host.dylib"
