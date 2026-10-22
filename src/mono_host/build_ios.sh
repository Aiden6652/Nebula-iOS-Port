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

# 强制清干净：先删掉整个产物目录再重建。
# 目的：确保下面的 dylib 一定是这次真编译出来的，不可能是上一轮残留 / 缓存命中的东西。
rm -rf "$OUT"
mkdir -p "$OUT"
echo "已清空并重建产物目录: $OUT"

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

# 定位 .NET 8 BCL 目录（解包后真实位置是 <prefix>/ios-monovm/lib/net8.0）
MONO_ROOT="$MONO_PREFIX"
if [ ! -d "$MONO_ROOT/lib/net8.0" ]; then
  found="$(find "$MONO_PREFIX" -maxdepth 4 -type d -name net8.0 -path "*/lib/*" 2>/dev/null | head -1)"
  [ -n "$found" ] && MONO_ROOT="$(dirname "$(dirname "$found")")"
fi
echo "MONO_ROOT: $MONO_ROOT"

echo "=== 生成嵌入式 blob（BCL + 可选 BepInEx/core，全部内存加载，无散文件）==="
BLOB="$OUT/.nebula_bcl.blob"
# BepInEx/core 默认取 vendored 目录（与星云包锁版本）；可用 BEPINEX_CORE_DIR 覆盖
if [ -z "${BEPINEX_CORE_DIR:-}" ] && [ -d "$ROOT/bepinex-core" ]; then
  BEPINEX_CORE_DIR="$ROOT/bepinex-core"
fi
echo "   BepInEx/core 来源: ${BEPINEX_CORE_DIR:-无（仅嵌 BCL）}"
python3 - "$MONO_ROOT" "${BEPINEX_CORE_DIR:-}" "$BLOB" <<'PY'
import sys, os, struct
mono_root = sys.argv[1]
bepinex_core = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else ""
blob_path = sys.argv[3]

entries = []; seen = set()
def add(name, data):
    if name in seen: return
    seen.add(name); entries.append((name, data))

# 1) .NET 8 BCL（托管程序集，含 System.Private.CoreLib）
bcl_dir = os.path.join(mono_root, "lib", "net8.0")
if os.path.isdir(bcl_dir):
    for f in sorted(os.listdir(bcl_dir)):
        if f.endswith(".dll"):
            add(f, open(os.path.join(bcl_dir, f), "rb").read())
else:
    print("!! 没找到 lib/net8.0，BCL 未嵌入", file=sys.stderr)

# 2) BepInEx/core（托管 dll，排除原生 dobby.dll）
if os.path.isdir(bepinex_core):
    for f in sorted(os.listdir(bepinex_core)):
        if not f.endswith(".dll"): continue
        if f.lower() == "dobby.dll": continue   # 原生库，不是托管，不能当 assembly 加载
        add("BepInEx/core/" + f, open(os.path.join(bepinex_core, f), "rb").read())

buf = struct.pack("<I", 0x4E424C31)  # 'NBL1'
buf += struct.pack("<I", len(entries))
for name, data in entries:
    nb = name.encode("utf-8")
    buf += struct.pack("<H", len(nb)) + nb
    buf += struct.pack("<I", len(data)) + data
open(blob_path, "wb").write(buf)
print("blob: %d 个程序集, %d bytes" % (len(entries), len(buf)))
PY

echo "=== 编译 iOS arm64 dylib（把 blob 嵌进 __DATA,__nebulabcl 段）==="
"$CCBIN" -arch arm64 \
  -isysroot "$SDK" \
  -miphoneos-version-min="$MIN_IOS" \
  -dynamiclib -fPIC \
  -DBUILD_AS_LIB \
  -I"$MONO_INC" \
  -o "$OUT/libnebula_mono_host.dylib" \
  "$ROOT/nebula_mono_host.c" \
  "$ROOT/case_insensitive_fs.c" \
  "$ROOT/fishhook.c" \
  "${LIBS[@]}" \
  -Wl,-sectcreate,__DATA,__nebulabcl,"$BLOB" \
  -framework Foundation \
  -framework CoreFoundation \
  -framework Security \
  -lobjc -lc++ -lpthread -lz -lm

# 注：BCL 已嵌入 dylib 的 __nebulabcl 段（见上方 blob 生成），不再散落 bcl/ 目录。
# 运行时由 nebula_mono_host.c 的 Mono 预加载钩子从内存加载。
BLOBSZ="$(wc -c < "$BLOB" 2>/dev/null || echo 0)"
echo "BCL 状态: 已内嵌（${BLOBSZ} bytes 烧进 dylib 的 __nebulabcl 段）"
rm -f "$BLOB"   # 临时 blob 已烧进段，删掉避免被打进 artifact

# Standard / Pro 是同一份二进制，区别只在 nebula_host.<profile>.cfg 里的
# runtime.mode（interp / jit）。这里各出一份带名字的副本，方便直接分发、不用改配置。
cp "$OUT/libnebula_mono_host.dylib" "$OUT/libnebula_standard.dylib"
cp "$OUT/libnebula_mono_host.dylib" "$OUT/libnebula_pro.dylib"

echo "=== 校验 ==="
lipo -info "$OUT/libnebula_mono_host.dylib"
file "$OUT/libnebula_mono_host.dylib" 2>/dev/null || true

echo "--- 关键符号检查（证明 MonoVM 真的链进来了）---"
SYMS="$(nm -gU "$OUT/libnebula_mono_host.dylib" 2>/dev/null || true)"
# 用 case 做纯字符串匹配，不用 echo|grep —— grep -q 会提前关管道，
# 在 pipefail 下会让 echo 报 Broken pipe 而把脚本判失败（产物本身没问题）。
for s in mono_jit_init mono_domain_assembly_open mono_runtime_invoke mono_gc_collect; do
  case "$SYMS" in
    *"$s"*) echo "    ✓ $s" ;;
    *)      echo "    ✗ $s 缺失！"; MISSING=1 ;;
  esac
done
[ -n "${MISSING:-}" ] && { echo "!! 缺少 Mono 符号，链接有问题" >&2; exit 1; }

echo "--- 产物 ---"
for f in libnebula_mono_host.dylib libnebula_standard.dylib libnebula_pro.dylib; do
  [ -f "$OUT/$f" ] && printf "    %-30s %8s  sha256=%s\n" "$f" \
     "$(du -h "$OUT/$f" | cut -f1)" "$(shasum -a 256 "$OUT/$f" | cut -c1-16)"
done
echo "    构建时间: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"

echo "OK -> $OUT"
