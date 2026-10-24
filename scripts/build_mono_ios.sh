#!/usr/bin/env bash
# 从源码编译 mono for iOS arm64 —— 替换原「下载预编译包」方案。
#
# 崩溃根因（真机 .ips 栈 + mono 源码核对）：
#   mono_jit_init -> mono_arch_init -> mini_get_breakpoint_trampoline
#     -> mono_arch_create_sdb_trampoline -> mono_assertion_message_unreachable
#
#   mono/mini/mini-arm64.c 的 mono_arch_init 里是：
#       if (!mono_aot_only)
#           bp_trampoline = mini_get_breakpoint_trampoline ();
#   而 mini-trampolines.c 里：
#       #ifdef MONO_ARCH_HAVE_SDB_TRAMPOLINES
#           tramp = mono_arch_create_sdb_trampoline (...);
#       #else
#           g_assert_not_reached ();      ← iOS 官方包走这里，必崩
#       #endif
#   iOS 不允许运行时造可执行 trampoline，官方预编译包没定义
#   MONO_ARCH_HAVE_SDB_TRAMPOLINES，且条件是 !mono_aot_only（不是 debugger 开关），
#   所以任何 JIT/interp 模式都必崩，关 debug agent 也救不了。
#
# 修法：自编译 mono，并在 mono_arch_init 里让 iOS 跳过 bp_trampoline 创建。
#   代价：仅 soft-debug 断点失效，不影响 JIT/interpreter 正常执行 IL。
#
# 构建源：mono 现在在 dotnet/runtime 仓库（mono/mono 的 sdks 已不适用于 iOS 构建）。
#   ./build.sh -s mono -os ios -arch arm64 -c Release
#   产物：artifacts/bin/mono/iOS.arm64.Release/
#   BCL(.NET 8 托管程序集) 仍从官方 nuget 取，不自己编。
#
# 产出（映射到 build_ios.sh 期望的 prefix 结构）：
#   $INSTALL/include/mono-2.0/...    头文件
#   $INSTALL/lib/*.a                 静态运行时（libmonosgen-2.0 等）
#   $INSTALL/lib/net8.0/*.dll        .NET 8 BCL（来自 nuget）
set -euo pipefail

MONO_REF="${MONO_REF:-v8.0.32}"          # dotnet/runtime tag，与 MONOVM_VERSION 对齐
MONOVM_VERSION="${MONOVM_VERSION:-8.0.32}"
WORK="${WORK:-$HOME/mono-src}"
INSTALL="${INSTALL:-$HOME/mono-installs}"
MANIFEST="${MANIFEST:-$HOME/mono_ios_manifest.txt}"
# 产物结构变更时 bump 这个版本号：旧缓存即使 key 命中也会因版本不符被判定残缺、强制重编
CACHE_VERSION="${CACHE_VERSION:-4}"
# iOS JIT 总开关：1=放出 ARM64 JIT（pro 模式需要；standard 仍走 aot_only 不受影响）；
# 设 0 回到标准 DISABLE_JIT 构建（standard 安全，不编 JIT）。CI 默认 1。
MONO_IOS_JIT="${MONO_IOS_JIT:-1}"

# 缓存复用：CI 用 actions/cache 恢复 ~/mono-installs；产物完整就跳过全量编译
if [ -f "$INSTALL/lib/libmonosgen-2.0.a" ] && [ -d "$INSTALL/include/mono-2.0" ] && [ -n "$(ls -A "$INSTALL/lib/net8.0" 2>/dev/null)" ] && [ "$(cat "$INSTALL/.cache_version" 2>/dev/null)" = "$CACHE_VERSION" ]; then
  echo "=== [缓存命中] 复用已有 mono 产物，跳过全量编译 ==="
  echo "    prefix: $INSTALL"
  echo "    静态库: $(ls "$INSTALL/lib" | grep -c '\.a$') 个 / BCL: $(ls "$INSTALL/lib/net8.0" | wc -l) 个"
  {
    echo "REUSED_CACHE=yes"
    echo "INSTALL=$INSTALL"
    echo "--- 静态库 ---"; ls "$INSTALL/lib" | head
    echo "--- BCL 数量 ---"; ls "$INSTALL/lib/net8.0" | wc -l
  } | tee "$MANIFEST"
  exit 0
fi

echo "=== [1/5] 环境 ==="
echo "Xcode: $(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null || echo 'NO iOS SDK')"
echo "clang: $(clang --version 2>/dev/null | head -1)"
echo "dotnet: $(dotnet --version 2>/dev/null || echo 'NO dotnet')"

echo "=== [2/5] 克隆 dotnet/runtime ($MONO_REF) ==="
rm -rf "$WORK"; mkdir -p "$WORK"
git clone --depth 1 --branch "$MONO_REF" https://github.com/dotnet/runtime "$WORK/runtime"
echo "    源码就位: $(du -sh "$WORK/runtime" 2>/dev/null | cut -f1)"

echo "=== [3/5] 打补丁：iOS 跳过 sdb breakpoint trampoline ==="
F="$WORK/runtime/src/mono/mono/mini/mini-arm64.c"
[ -f "$F" ] || { echo "!! 找不到 $F" >&2; exit 1; }
python3 - "$F" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='replace').read()
new = ("\t/* [Nebula iOS patch] iOS 无可执行内存可造 sdb trampoline（官方预编译包在此\n"
       "\t   assert_not_reached）。跳过 breakpoint trampoline 创建：仅 soft-debug 断点失效，\n"
       "\t   不影响 JIT / interpreter 执行 IL。 */\n"
       "\tbp_trampoline = NULL;\n")
m = re.search(r"[ \t]*if[ \t]*\(!mono_aot_only\)[ \t]*\n[ \t]*bp_trampoline[ \t]*=[ \t]*mini_get_breakpoint_trampoline[ \t]*\([ \t]*\)[ \t]*;", s)
if not m:
    # 可能已被上游改写成单行
    m2 = re.search(r"[ \t]*if[ \t]*\(!mono_aot_only\)[ \t]*\{?[ \t]*bp_trampoline[ \t]*=[ \t]*mini_get_breakpoint_trampoline[ \t]*\([ \t]*\)[ \t]*;", s)
    if not m2:
        print("!! 补丁点未匹配（mono 源码可能已改结构）", file=sys.stderr)
        sys.exit(1)
    m = m2
s = s[:m.start()] + new + s[m.end():]
open(p, 'w', encoding='utf-8').write(s)
print("    补丁已应用到 mini-arm64.c")
PY

echo "=== [3.5/5] iOS JIT 支持（参考 OpenJDK/Pojav 的 W^X 处理）==="
if [ "${MONO_IOS_JIT:-1}" = "1" ]; then
  RT="$WORK/runtime"
  python3 - "$RT" <<'PY'
import os, re, sys
rt = sys.argv[1]
mono = os.path.join(rt, 'src/mono')

# (a) 放出 JIT：在 tramp-arm64.c / mini-arm64.c 的 #include 段之后取消 DISABLE_JIT 定义，
#     让真正 的 ARM64 JIT 实现（generic trampoline + emit 后端）参与编译，而不是 g_assert_not_reached 占位。
for fn in ('mini/mini-arm64.c', 'mini/tramp-arm64.c'):
    p = os.path.join(mono, 'mono', fn)
    if not os.path.exists(p):
        print("!! 找不到", p, file=sys.stderr); continue
    s = open(p, encoding='utf-8', errors='replace').read()
    last = -1
    for m in re.finditer(r'^\s*#\s*include\b.*$', s, re.M):
        last = m.end()  # 匹配整行（含换行），插在最后一个 #include 行之后，避免拼到同行
    if last < 0: last = 0
    # 注释独占一行，#undef 独占一行且后不跟注释，规避“#undef 后跟注释”被某些预处理实现误判
    ins = ("\n"
           "/* [Nebula iOS JIT] 放出 ARM64 JIT 实现：原本被 iOS 构建的 DISABLE_JIT 排除，只剩 g_assert_not_reached 占位 */\n"
           "#undef DISABLE_JIT\n")
    s = s[:last] + ins + s[last:]
    open(p, 'w', encoding='utf-8').write(s)
    print("    #undef DISABLE_JIT 已插入:", fn)

# (a1) 根治：直接中和 config.h.in 的 #cmakedefine DISABLE_JIT，使生成的 config.h 不再
#      #define DISABLE_JIT。这是 DISABLE_JIT 的真正源头：编译期由 cmake 把 config.h.in
#      生成 config.h 并 #define DISABLE_JIT，所有 #include config.h 的文件都受影响
#      （含 arch/arm64/arm64-codegen.c 的 emit helper，之前只给 mini/tramp 注入 #undef 会漏）。
#      去掉后整个 iOS mono 放出完整 JIT 实现，不必逐文件 #undef。逐文件 #undef 仅作冗余安全网。
cfg = os.path.join(mono, 'cmake', 'config.h.in')
if os.path.exists(cfg):
    s = open(cfg, encoding='utf-8', errors='replace').read()
    if '#cmakedefine DISABLE_JIT' in s:
        s2 = s.replace('#cmakedefine DISABLE_JIT 1',
                       '/* [Nebula iOS JIT] 不在 iOS 定义 DISABLE_JIT，放出完整 JIT 实现 */')
        open(cfg, 'w', encoding='utf-8').write(s2)
        print("    config.h.in: 已移除 DISABLE_JIT 的 cmakedefine（iOS 放出完整 JIT）")
    else:
        print("    !! config.h.in 未匹配到 #cmakedefine DISABLE_JIT", file=sys.stderr)

# (a3) iOS 强制定义 HAVE_PTHREAD_JIT_WRITE_PROTECT_NP：
#      cross-compile 下 cmake 的 check_symbol_exists 默认 no，导致 mono-codeman.c 的
#      W^X 翻转守卫（#if HAVE_... && TARGET_OSX）不编译 → iOS 上写 MAP_JIT 内存
#      从不翻转写保护，mono_jit_init 生成 trampoline 即 SIGSEGV（standard/pro 同崩）。
#      iOS 14+ 的 libsystem_pthread 已导出该符号（配 StikDebug 的 CS_DEBUGGED JIT 能力可用）。
cfg = os.path.join(mono, 'cmake', 'config.h.in')
if os.path.exists(cfg):
    s = open(cfg, encoding='utf-8', errors='replace').read()
    if '#cmakedefine HAVE_PTHREAD_JIT_WRITE_PROTECT_NP' in s:
        s2 = s.replace('#cmakedefine HAVE_PTHREAD_JIT_WRITE_PROTECT_NP',
                       '#define HAVE_PTHREAD_JIT_WRITE_PROTECT_NP 1 /* [Nebula iOS] iOS 14+ 已导出，配 StikDebug 可用 */')
        open(cfg, 'w', encoding='utf-8').write(s2)
        print("    config.h.in: HAVE_PTHREAD_JIT_WRITE_PROTECT_NP → 强制 #define（iOS）")
    elif 'HAVE_PTHREAD_JIT_WRITE_PROTECT_NP' not in s:
        s += "\n/* [Nebula iOS] 强制定义：iOS 14+ libsystem_pthread 已导出，配 StikDebug JIT 能力可用 */\n"
        s += "#define HAVE_PTHREAD_JIT_WRITE_PROTECT_NP 1\n"
        open(cfg, 'w', encoding='utf-8').write(s)
        print("    config.h.in: 末尾追加 HAVE_PTHREAD_JIT_WRITE_PROTECT_NP=1（iOS）")
    else:
        print("    config.h.in: HAVE_PTHREAD_JIT_WRITE_PROTECT_NP 已存在，跳过")

# (a2) 兜底：中和构建系统里可能存在的 -DDISABLE_JIT / add_compile_definitions(DISABLE_JIT) / add_definitions(...DISABLE_JIT...)
for root, dirs, files in os.walk(os.path.join(mono, 'mono')):
    for f in files:
        if not (f == 'CMakeLists.txt' or f.endswith('.cmake') or f.endswith('.txt')):
            continue
        p = os.path.join(root, f)
        try: s = open(p, encoding='utf-8', errors='replace').read()
        except: continue
        if 'DISABLE_JIT' not in s: continue
        s2 = re.sub(r'-DDISABLE_JIT\b', '', s)
        s2 = re.sub(r'\badd_compile_definitions\s*\(\s*DISABLE_JIT\s*\)', '', s2)
        s2 = re.sub(r'\badd_definitions\s*\([^)]*DISABLE_JIT[^)]*\)', '', s2)
        if s2 != s:
            open(p,'w').write(s2)
            print("    中和 DISABLE_JIT 定义:", os.path.relpath(p, mono))

# (b) 把 MAP_JIT 分配从 macOS-only 扩展到 iOS（mono-mmap.c）。
#     原守卫 #if ... && defined(TARGET_OSX) 让 iOS 完全跳过 MAP_JIT，导致 JIT 内存非法。
mp = os.path.join(mono, 'mono', 'utils', 'mono-mmap.c')
s = open(mp, encoding='utf-8', errors='replace').read()
s2 = s.replace('#if defined(__APPLE__) && defined(MAP_JIT) && defined(TARGET_OSX)',
               '#if defined(__APPLE__) && defined(MAP_JIT) && (defined(TARGET_OSX) || defined(TARGET_IOS))')
s2 = s2.replace('if (__builtin_available (macOS 11, *))',
                'if (__builtin_available (macOS 11, iOS 14, *))')
if s2 != s:
    open(mp,'w').write(s2)
    print("    mono-mmap.c: MAP_JIT 已扩展到 iOS + 可用性修正为 iOS 14")
else:
    print("    !! mono-mmap.c 未匹配到 MAP_JIT 守卫（mono 版本可能已改结构）", file=sys.stderr)

# (c) iOS W^X 翻转核心补丁：mono_codeman_enable_write/disable_write 的守卫原本是
#     #if defined(HAVE_PTHREAD_JIT_WRITE_PROTECT_NP) && defined(TARGET_OSX)
#     TARGET_OSX 直接排除 iOS，mono 在 iOS 上写 MAP_JIT 内存（trampoline/JIT 代码）
#     从不翻转写保护，mono_jit_init 即 SIGSEGV（standard/pro 同崩）。
#     扩到 TARGET_IOS，并把可用性检查 (macOS 11, *) 改成 (macOS 11, iOS 14, *)。
cm = os.path.join(mono, 'mono', 'metadata', 'mono-codeman.c')
if os.path.exists(cm):
    s = open(cm, encoding='utf-8', errors='replace').read()
    s2 = s.replace(
        'defined(HAVE_PTHREAD_JIT_WRITE_PROTECT_NP) && defined(TARGET_OSX)',
        'defined(HAVE_PTHREAD_JIT_WRITE_PROTECT_NP) && (defined(TARGET_OSX) || defined(TARGET_IOS))')
    s2 = re.sub(r'__builtin_available\s*\(\s*macOS\s+11[^)]*\)',
                '__builtin_available (macOS 11, iOS 14, *)', s2)
    if s2 != s:
        open(cm, 'w', encoding='utf-8').write(s2)
        print("    mono-codeman.c: W^X 翻转守卫已扩展到 TARGET_IOS + 可用性修正为 iOS 14")
    else:
        print("    !! mono-codeman.c 未匹配到 TARGET_OSX 守卫（mono 版本可能已改结构）", file=sys.stderr)
else:
    print("    !! 找不到 mono-codeman.c", file=sys.stderr)
PY
# (c) iOS W^X 翻转核心补丁：mono_codeman_enable_write/disable_write 的守卫原本是
#     `#if defined(HAVE_PTHREAD_JIT_WRITE_PROTECT_NP) && defined(TARGET_OSX)`
#     —— TARGET_OSX 直接排除 iOS，mono 在 iOS 上写 MAP_JIT 内存（trampoline/JIT 代码）
#     从不翻转写保护，mono_jit_init 即 SIGSEGV（standard/pro 同崩）。
#     扩到 TARGET_IOS，并把可用性检查 (macOS 11, *) 改成 (macOS 11, iOS 14, *)。
  echo "    [JIT] DISABLE_JIT 已放出 + MAP_JIT 已扩展到 iOS + W^X 翻转已扩展到 iOS"
else
  echo "    MONO_IOS_JIT 未开启，保留标准 DISABLE_JIT 构建（standard 安全）"
fi

echo "=== [4/5] 编译 iOS arm64 mono 运行时 ==="
cd "$WORK/runtime"
./build.sh -s mono -os ios -arch arm64 -c Release 2>&1 | tail -60

echo "=== [5/5] 整理产物（头文件 + 静态库）==="
rm -rf "$INSTALL"; mkdir -p "$INSTALL/include" "$INSTALL/lib"
# 公共嵌入 API 头：dotnet/runtime 放在 src/native/public/mono（布局 mono/jit/jit.h 等）
INC="$WORK/runtime/src/native/public/mono"
if [ ! -d "$INC" ]; then
  # fallback：构建产物里找生成好的 mono-2.0 目录
  INC="$(find "$WORK/runtime/artifacts" -type d -name 'mono-2.0' 2>/dev/null | head -1 || true)"
fi
if [ -n "$INC" ] && [ -d "$INC" ]; then
  if [ -f "$INC/mono/jit/jit.h" ]; then
    cp -R "$INC" "$INSTALL/include/mono-2.0"
  elif [ -f "$INC/jit/jit.h" ]; then
    mkdir -p "$INSTALL/include/mono-2.0"
    cp -R "$INC" "$INSTALL/include/mono-2.0/mono"
  else
    echo "!! $INC 里没有 jit.h" >&2; exit 1
  fi
  echo "    头文件: $INC"
else
  echo "!! 未找到 mono 头文件目录（public 与 artifacts 都没有）" >&2
  exit 1
fi
# 校验只告警不阻断：编译已成功，别因为校验没过让整轮白跑、缓存也不落盘
if [ ! -f "$INSTALL/include/mono-2.0/mono/jit/jit.h" ]; then
  echo "    ⚠ 警告：头文件布局缺 mono/jit/jit.h（宿主编译会失败，但不阻断 mono 构建）" >&2
  HDR_WARN=1
fi
find "$WORK/runtime/artifacts/bin/mono" -name 'libmonosgen-2.0.a' 2>/dev/null | head -1 | while read -r f; do
  cp "$f" "$INSTALL/lib/"; echo "    $(basename "$f")"
done
# 全部 iOS 目标静态库（排除 cross/ 里的 host 工具）。
# 必须带上 libSystem.Globalization.Native.a：mono 的 native-library.c 里
# lookup_pinvoke_call_impl 会调 GlobalizationResolveDllImport（定义在
# src/native/libs/System.Globalization.Native/entrypoints.c），漏了链接期必缺符号。
find "$WORK/runtime/artifacts/bin/mono" -name '*.a' -not -path '*/cross/*' 2>/dev/null | while read -r f; do
  cp -n "$f" "$INSTALL/lib/" 2>/dev/null || true
done
echo "    静态库: $(ls "$INSTALL/lib" 2>/dev/null | wc -l) 个"
[ -n "$(ls -A "$INSTALL/lib" 2>/dev/null)" ] || { echo "!! 未产出任何静态库" >&2; exit 1; }

# 校验：GlobalizationResolveDllImport 必须能在收集到的库里找到，否则宿主链接必挂
OK=0
for f in "$INSTALL"/lib/*.a; do
  if nm "$f" 2>/dev/null | grep -q 'GlobalizationResolveDllImport'; then
    echo "    ✓ GlobalizationResolveDllImport 来自 $(basename "$f")"
    OK=1; break
  fi
done
if [ "$OK" != "1" ]; then
  # 同样只告警：真正缺符号会在宿主链接期暴露，这里不扼杀已编译好的产物
  echo "    ⚠ 警告：收集到的静态库里没找到 GlobalizationResolveDllImport（宿主链接可能缺符号）" >&2
  ls "$INSTALL/lib" >&2 || true
fi

echo "=== [6/6] 取 .NET 8 BCL（托管程序集，不自己编）==="
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
  echo "--- 头文件 ---"; ls "$INSTALL/include/mono-2.0" 2>/dev/null | head -3 || true
  echo "--- 静态库 ---"; ls "$INSTALL/lib" 2>/dev/null | head
  echo "--- BCL 数量 ---"; ls "$INSTALL/lib/net8.0" 2>/dev/null | wc -l
  echo "--- 校验（非阻断）---"
  echo "GLOBALIZATION_SYM_FOUND=$OK"
  echo "HDR_JIT_H_MISSING=${HDR_WARN:-0}"
} | tee "$MANIFEST"

echo "$CACHE_VERSION" > "$INSTALL/.cache_version"
echo "OK: 自编译 mono iOS 就位 -> $INSTALL"
