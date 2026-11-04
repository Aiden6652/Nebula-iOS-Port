#!/usr/bin/env bash
# 交叉编译宿主 dylib → iOS arm64
#
# 用法: bash src/mono_host/build_ios.sh <Mono-iOS-安装前缀>
#
# 产物: out-ios/libnebula_standard.dylib（单一编译器/JIT 版）
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
    libSystem.*.a) continue ;;  # [R60] 下面用 -Wl,-force_load 单独全量链，避免重复定义
    libicu*.a) continue ;;      # [R60] ICU 同样单独 -force_load
  esac
  [ -n "$line" ] && LIBS+=("$line")
done < <(find "$MONO_PREFIX" -name "*.a" 2>/dev/null)

# [R60 CI 教训 #3] 先判空再展开数组：macOS bash 3.2 + set -u 下，
# 对空数组 "${LIBS[@]}" 展开会直接 unbound variable。
if [ "${#LIBS[@]}" -eq 0 ]; then
  echo "!! 没找到 Mono 静态库，检查 $MONO_PREFIX" >&2
  exit 1
fi
echo "静态库 ${#LIBS[@]} 个:"
printf '  %s\n' "${LIBS[@]}"

# 定位 .NET 8 BCL 目录（解包后真实位置是 <prefix>/ios-monovm/lib/net8.0）
MONO_ROOT="$MONO_PREFIX"
if [ ! -d "$MONO_ROOT/lib/net8.0" ]; then
  found="$(find "$MONO_PREFIX" -maxdepth 4 -type d -name net8.0 -path "*/lib/*" 2>/dev/null | head -1)"
  [ -n "$found" ] && MONO_ROOT="$(dirname "$(dirname "$found")")"
fi
echo "MONO_ROOT: $MONO_ROOT"

echo "=== 生成嵌入式 blob（BCL + BepInEx5 Mono core + 游戏 managed，全部内存加载，无散文件）==="
BLOB="$OUT/.nebula_bcl.blob"
# [LEGACY_MONO] BepInEx5 Unity Mono core 默认取 vendored 目录；可用 BEPINEX_CORE_DIR 覆盖
if [ -z "${BEPINEX_CORE_DIR:-}" ] && [ -d "$ROOT/bepinex-mono-core" ]; then
  BEPINEX_CORE_DIR="$ROOT/bepinex-mono-core"
fi
echo "   BepInEx5 core 来源: ${BEPINEX_CORE_DIR:-无（仅嵌 BCL）}"
python3 - "$MONO_ROOT" "${BEPINEX_CORE_DIR:-}" "$BLOB" "$ROOT" <<'PY'
import sys, os, struct
mono_root = sys.argv[1]
bepinex_core = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else ""
blob_path = sys.argv[3]
root_dir = sys.argv[4] if len(sys.argv) > 4 else "."

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

# 3) [LEGACY_MONO] 老 AU Mono 版的游戏 managed 程序集（Assembly-CSharp.dll + UnityEngine.* 等）。
#    必须落成散文件：BepInEx5 Preloader.Main(args[0]) 按路径 Assembly.Load 游戏入口，
#    且 Mono 程序集解析器要能在磁盘上找到 UnityEngine.* 等依赖。
#    带 "game-managed/" 前缀，宿主 blob_find 跳过、由 extract_dir 释放到 $(root)/game-managed/。
game_managed = os.path.join(root_dir, "game-managed")
if os.path.isdir(game_managed):
    n = 0
    for f in sorted(os.listdir(game_managed)):
        p = os.path.join(game_managed, f)
        if not os.path.isfile(p): continue
        add("game-managed/" + f, open(p, "rb").read())
        n += 1
    print("   [legacy-mono] game-managed: %d 个文件已内嵌（启动释放到 game-managed/）" % n)
else:
    print("!! 没找到 game-managed/（需从老 AU .ipa 的 Data/Managed 提取游戏 managed）", file=sys.stderr)

# 5) [R60] iOS 原生 shim（libSystem.*.dylib）字节内嵌，作为 fallback：
#    主路径是把 libSystem.*.a 静态链进本 dylib（-force_load），完全无散文件；
#    若全静态链因 ICU/GSS 依赖失败（见下方自动降级），宿主启动时把这里的 dylib
#    字节释放到可写目录并 dllmap 过去 —— 就是用户要的"表面一个、启动自动分裂回来"。
#    同样带 "native/" 前缀，blob_find 跳过。
native_src = os.path.join(mono_root, "lib", "net8.0", "native")
if os.path.isdir(native_src):
    n = 0
    for f in sorted(os.listdir(native_src)):
        if f.startswith("libSystem.") and f.endswith(".dylib"):
            add("native/" + f, open(os.path.join(native_src, f), "rb").read())
            n += 1
    print("   [R60] native shim(fallback): %d 个已内嵌" % n)

buf = struct.pack("<I", 0x4E424C31)  # 'NBL1'
buf += struct.pack("<I", len(entries))
for name, data in entries:
    nb = name.encode("utf-8")
    buf += struct.pack("<H", len(nb)) + nb
    buf += struct.pack("<I", len(data)) + data
open(blob_path, "wb").write(buf)
print("blob: %d 个程序集, %d bytes" % (len(entries), len(buf)))
PY

echo "=== [R60] 原生 shim 处理：静态链进主 dylib（真·单一文件），dylib 字节内嵌作 fallback ==="
# 不再把 libSystem.*.dylib 平铺进产物目录 —— R60 只交付一个 libnebula_standard.dylib。
#   主路径：把 libSystem.*.a + ICU + GSS 用 -force_load / -framework 静态链进主 dylib，
#           宿主把 libSystem.Native 等 dllmap 指向【主 dylib 自身路径】，dlsym 命中
#           导出符号（.NET 8 CoreLib 的 P/Invoke 全部落到同一个 Mach-O 里）。
#   备选路径：CI 若无静态库 / 静态链失败，宿主从 __nebulabcl 段里 "native/" 前缀的
#           dylib 字节释放到可写目录再 dllmap 过去。
#   两条路都不需要在 app bundle 里放任何额外的 .dylib —— 用户重签只需签一个文件。
#
# CI 的 Mono prefix 只带 libSystem.*.dylib（build_mono_ios.sh 只提取了 dylib），
# 但 .NET 8 iOS runtime nupkg 里同时提供 5 个 libSystem.*.a 和 3 个 libicu*.a：
#   Globalization.Native.a 引用 ~120 个 _u* / _ucol_* / _ures_* ICU 符号，
#   Net.Security.Native.a 引用 _gss_* —— 官方 .dylib 分别把 ICU 静态链入、
#   链接 /System/Library/Frameworks/GSS.framework/GSS。静态链必须自己补这两组依赖，
#   否则 ld: symbol(s) not found for architecture arm64（run 41840513127 的失败原因）。
# 下载失败也不致命：宿主仍可从 __nebulabcl 的 "native/" 前缀释放 .dylib 走 fallback。
NATIVE_A_DIR="/tmp/nebula_native_a_$$"
rm -rf "$NATIVE_A_DIR"
mkdir -p "$NATIVE_A_DIR"
for a in "$MONO_PREFIX"/lib/libSystem.*.a "$MONO_PREFIX"/lib/libicu*.a; do
  if [ -f "$a" ]; then cp "$a" "$NATIVE_A_DIR/"; fi
done
# [R60 CI 教训 #2] 绝不能用 `ls dir/libSystem.*.a 2>/dev/null | wc -l` 计数：
# 无匹配时 ls 返回 2，set -o pipefail 让整条管道失败，赋值语句随即触发 set -e
# 静默退出（stderr 又被 /dev/null 吞掉，CI 日志只见 build 步骤莫名 exit 1、
# 无任何错误行，正是 run 41837901825 的失败现象）。改用 glob + [ -f ] 逐项计数。
N_A=0
for a in "$NATIVE_A_DIR"/libSystem.*.a; do
  if [ -f "$a" ]; then N_A=$((N_A+1)); fi
done
if [ "$N_A" -lt 5 ]; then
  VM="${MONOVM_VERSION:-8.0.32}"
  PKG_IOS="microsoft.netcore.app.runtime.mono.ios-arm64"
  URL_IOS="https://api.nuget.org/v3-flatcontainer/$PKG_IOS/$VM/$PKG_IOS.$VM.nupkg"
  echo "    下载 iOS runtime nupkg 提取 native .a ($VM) ..."
  if curl -sSL --max-time 240 -o "$NATIVE_A_DIR/iosrt.nupkg" "$URL_IOS" && [ -s "$NATIVE_A_DIR/iosrt.nupkg" ]; then
    python3 - "$NATIVE_A_DIR/iosrt.nupkg" "$NATIVE_A_DIR" <<'PY'
import zipfile, sys, os
try:
    z = zipfile.ZipFile(sys.argv[1])
except Exception as e:
    # [R60 CI 教训 #4] 下载到 302 HTML / 坏 zip 时不能让 python3 非零退出，
    # 否则 set -e 又静默杀掉脚本；静态链失败本来就允许走 blob fallback。
    print("       ⚠ nupkg 不是有效 zip（%s），跳过静态链走 blob fallback" % e)
    sys.exit(0)
out = sys.argv[2]
n = 0
for i in z.infolist():
    fn = os.path.basename(i.filename)
    if i.filename.startswith('runtimes/ios-arm64/native/') and fn.endswith('.a') \
       and (fn.startswith('libSystem.') or fn.startswith('libicu')):
        open(os.path.join(out, fn), 'wb').write(z.read(i)); n += 1
print("       nupkg 提取 %d 个 libSystem.*.a / libicu*.a" % n)
PY
  else
    echo "    ⚠ nupkg 下载失败 —— CoreLib shim 将走 blob 释放 fallback" >&2
  fi
  rm -f "$NATIVE_A_DIR/iosrt.nupkg"
fi
NATIVE_A=()   # 5 个 CoreLib P/Invoke shim
ICU_A=()      # 3 个 ICU 静态库（Globalization.Native 依赖）
for a in "$NATIVE_A_DIR"/libSystem.*.a; do
  if [ -f "$a" ]; then NATIVE_A+=("$a"); fi
done
for a in "$NATIVE_A_DIR"/libicu*.a; do
  if [ -f "$a" ]; then ICU_A+=("$a"); fi
done
echo "    可静态链: CoreLib shim ${#NATIVE_A[@]} 个 / ICU ${#ICU_A[@]} 个"
# macOS 自带 bash 3.2 + set -u：空数组 "${ARR[@]}" 展开会报 unbound variable
# （run 41834026635 的首次失败根因），所有数组展开都必须用长度判断包起来。
if [ ${#NATIVE_A[@]} -gt 0 ]; then
  for a in "${NATIVE_A[@]}"; do echo "      $(basename "$a")"; done
fi
if [ ${#ICU_A[@]} -gt 0 ]; then
  for a in "${ICU_A[@]}"; do echo "      $(basename "$a")"; done
fi

echo "=== 编译 iOS arm64 dylib（把 blob 嵌进 __DATA,__nebulabcl 段）==="

# [R60] 强制全量链入：
#   ① libdobby.a —— BepInEx 的 [DllImport("dobby")] 原本在设备上命中
#      core/dobby.dll（Windows x64 PE, 153088B），iOS arm64 dlopen 不了 →
#      DllNotFoundException: dobby → IL2CPPChainloader.Initialize 中断 →
#      宿主 R56 兜底强扫 plugins 时 SIGSEGV（真机 preloader.log / nebula_host.log 实锤）。
#      这里把 iOS arm64 版 Dobby 静态链进来，宿主把 "dobby" dllmap 到主 dylib
#      自身路径，dlopen 自身 + dlsym("DobbyHook") 即可命中，彻底修掉崩溃根因。
#   ② libSystem.*.a —— CoreLib 的 P/Invoke shim，同样静态链进主 dylib，实现单文件。
#   ③ libicu*.a —— Globalization.Native 的 ICU 符号来源（nupkg 自带，无版本后缀）。
#   ④ -framework GSS —— Net.Security.Native 的 _gss_* 符号来源。
# 用 -force_load（而非普通列出 .a）：归档里未被 C 代码直接引用的目标文件也必须
# 全部链入并导出，否则 dlsym 找不到 SystemNative_* / DobbyHook。
#
# [R60 CI 教训 #5] ICU / GSS 属于系统依赖，一旦某版 SDK 缺库或符号对不上，
# 全量链接会失败。这里做【自动降级】：先试全静态链，失败就只静态链 dobby
# （自包含、必成功），CoreLib shim 改由宿主启动时从 blob 的 "native/" 前缀
# 释放到 Documents/nebula_native 并 dllmap 过去。两条路产物目录都只有 1 个 dylib，
# CI 必定能产出 artifact，不会再出现"编译失败→空包→又白等一轮"。
DOBBY_A="$ROOT/dobby/libdobby.a"
if [ ! -f "$DOBBY_A" ]; then
  echo "!! 缺 dobby/libdobby.a —— R60 无法修 DllNotFoundException: dobby" >&2
  exit 1
fi
# FORCE_DOBBY 恒有 1 个元素，永不为空 —— 避免在 bash 3.2 + set -u 下展开空数组。
FORCE_DOBBY=("-Wl,-force_load,$DOBBY_A")
FORCE_NATIVE=()
if [ ${#NATIVE_A[@]} -gt 0 ]; then
  for a in "${NATIVE_A[@]}"; do FORCE_NATIVE+=("-Wl,-force_load,$a"); done
fi
if [ ${#ICU_A[@]} -gt 0 ]; then
  for a in "${ICU_A[@]}"; do FORCE_NATIVE+=("-Wl,-force_load,$a"); done
fi
echo "    [R60] dobby 静态链: $(basename "$DOBBY_A") ($(wc -c < "$DOBBY_A") bytes)"
echo "    [R60] 全量 force_load 条目: $(( 1 + ${#FORCE_NATIVE[@]} )) 个"

# 单一编译器（JIT）版：解释器已废除，standard / pro 合并为一份。
# 宿主代码始终走 JIT（见 nebula_mono_host.c 的 mono_setmmapjit(1)），
# 不再区分 NEBULA_PRO；仅产出一个包 libnebula_standard.dylib。
do_link() {
  "$CCBIN" -arch arm64 \
    -isysroot "$SDK" \
    -miphoneos-version-min="$MIN_IOS" \
    -dynamiclib -fPIC \
    -DBUILD_AS_LIB \
    -DLEGACY_MONO \
    -I"$MONO_INC" \
    -o "$OUT/libnebula_standard.dylib" \
    "$ROOT/nebula_mono_legacy.c" \
    "$ROOT/case_insensitive_fs.c" \
    "$ROOT/fishhook.c" \
    "$ROOT/boot_overlay.m" \
    "$ROOT/nebula_dobby_shim.c" \
    "${LIBS[@]}" \
    "$@" \
    -Wl,-sectcreate,__DATA,__nebulabcl,"$BLOB" \
    -framework Foundation \
    -framework CoreFoundation \
    -framework Security \
    -framework UIKit \
    -lobjc -lc++ -lpthread -lz -lm \
    -Wl,-U,_pthread_jit_write_protect_np
}

R60_SHIM_MODE="static"
if [ ${#FORCE_NATIVE[@]} -gt 0 ]; then
  if do_link "${FORCE_DOBBY[@]}" "${FORCE_NATIVE[@]}" -framework GSS; then
    echo "    [R60] 全静态链成功：dobby + 5 shim + ICU + GSS 全部并进主 dylib（真·单文件）"
  else
    echo "    [R60] ⚠ 全静态链失败（ICU/GSS 依赖），自动降级：只静态链 dobby；CoreLib shim 走 blob 释放 fallback" >&2
    rm -f "$OUT/libnebula_standard.dylib"
    R60_SHIM_MODE="blob-fallback"
    do_link "${FORCE_DOBBY[@]}"
  fi
else
  echo "    [R60] 无 shim/ICU 静态库，只静态链 dobby；CoreLib shim 走 blob 释放 fallback"
  R60_SHIM_MODE="blob-fallback"
  do_link "${FORCE_DOBBY[@]}"
fi
echo "    [R60] CoreLib shim 模式: $R60_SHIM_MODE"

# 注：BCL 已嵌入 dylib 的 __nebulabcl 段（见上方 blob 生成），不再散落 bcl/ 目录。
# 运行时由 nebula_mono_host.c 的 Mono 预加载钩子从内存加载。
BLOBSZ="$(wc -c < "$BLOB" 2>/dev/null || echo 0)"
echo "BCL 状态: 已内嵌（${BLOBSZ} bytes 烧进 dylib 的 __nebulabcl 段）"
rm -f "$BLOB"   # 临时 blob 已烧进段，删掉避免被打进 artifact

echo "=== 校验 ==="
lipo -info "$OUT/libnebula_standard.dylib"
file "$OUT/libnebula_standard.dylib" 2>/dev/null || true

echo "--- 关键符号检查（证明 MonoVM 真的链进来了）---"
SYMS="$(nm -gU "$OUT/libnebula_standard.dylib" 2>/dev/null || true)"
# 用 case 做纯字符串匹配，不用 echo|grep —— grep -q 会提前关管道，
# 在 pipefail 下会让 echo 报 Broken pipe 而把脚本判失败（产物本身没问题）。
for s in mono_jit_init mono_domain_assembly_open mono_runtime_invoke mono_gc_collect; do
  case "$SYMS" in
    *"$s"*) echo "    ✓ $s" ;;
    *)      echo "    ✗ $s 缺失！"; MISSING=1 ;;
  esac
done
[ -n "${MISSING:-}" ] && { echo "!! 缺少 Mono 符号，链接有问题" >&2; exit 1; }

# [R60] dobby 必须真的并进主 dylib（否则 chainloader 仍会 DllNotFoundException）
case "$SYMS" in
  *DobbyHook*) echo "    ✓ DobbyHook（iOS arm64 dobby 已并入主 dylib）" ;;
  *) echo "    ✗ DobbyHook 缺失！dobby 静态链失败" >&2; exit 1 ;;
esac
# [R61] BepInEx6 IL2CPP 的 DobbyDetour 走两段式 DobbyPrepare/DobbyCommit，
# 上游 jmpews/dobby 只有 DobbyHook；nebula_dobby_shim.c 已用 DobbyHook 包出这俩，
# 这里校验它们真的并进主 dylib（legacy 走 BepInEx5 Mono 不调 Dobby，纯防御性对齐）。
case "$SYMS" in
  *DobbyPrepare*) echo "    ✓ DobbyPrepare/DobbyCommit（shim 已并入，dobby API 与 IL2CPP 构建对齐）" ;;
  *) echo "    ✗ DobbyPrepare 缺失！nebula_dobby_shim.c 未链入主 dylib" >&2; exit 1 ;;
esac
# [R60] 静态链 CoreLib shim 的证明（blob-fallback 模式不要求主 dylib 含 SystemNative_*）
if [ "$R60_SHIM_MODE" = "static" ]; then
  case "$SYMS" in
    *SystemNative_*) echo "    ✓ SystemNative_*（CoreLib P/Invoke shim 已并入主 dylib）" ;;
    *) echo "    ⚠ 静态链模式下未导出 SystemNative_*，宿主将按符号探测自动走 fallback" >&2 ;;
  esac
else
  echo "    - CoreLib shim 走 blob 释放 fallback（宿主 dllmap 到 Documents/nebula_native）"
fi

echo "--- [R60] 单一文件校验：产物目录里只应有 1 个 .dylib ---"
find "$OUT" -name '*.dylib' -print | sed 's|^|    |'
NDYLIB="$(find "$OUT" -name '*.dylib' | wc -l | tr -d ' ' || true)"
[ "$NDYLIB" = "1" ] || { echo "!! 产物目录有 $NDYLIB 个 dylib，R60 要求只交付一个" >&2; exit 1; }

echo "--- 产物 ---"
for f in libnebula_standard.dylib; do
  [ -f "$OUT/$f" ] && printf "    %-30s %8s  sha256=%s\n" "$f" \
     "$(du -h "$OUT/$f" | cut -f1)" "$(shasum -a 256 "$OUT/$f" | cut -c1-16)"
done
echo "    构建时间: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"

echo "OK -> $OUT"
