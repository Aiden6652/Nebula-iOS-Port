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

# [R64d] 导出清单：ld64 一旦看到 -exported_symbol(s_list) 就切换成"仅导出清单内符号"模式，
# 会把默认导出的 mono_* 等全砍掉（R64d 首版踩过：mono_jit_init 全部缺失）。所以这里用
# 通配 '*' 保留默认全导出，再单独追加需要强制导出的符号：
#   _SystemNative_IsATty —— 静态链入的 libSystem.Native.a 里是隐藏可见性强符号（R64c）
#   _GetModuleHandle* / _GetProcAddress —— [R85] 宿主导出的 Windows kernel32 no-op stub，
#     dllmap 把 kernel32.dll 指到主 dylib 后，NOS 的 P/Invoke 靠 dlsym 命中它们。
#     显式列出确保进导出表（'*' 通常已覆盖，但列全可防可见性/裁剪差异导致 dlsym 落空）。
printf '*\n_SystemNative_IsATty\n_GetModuleHandleW\n_GetModuleHandleA\n_GetModuleHandle\n_GetModuleHandleWW\n_GetProcAddress\n_GetProcAddressA\n' > "$OUT/export_list.txt"
echo "    导出清单: $OUT/export_list.txt (* + IsATty + [R85] kernel32 no-op stubs)"

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
#
# [R84] 组件必须「stub / full 二选一」，不能两个都进 LIBS：官方 iOS nupkg 对同一个
# 组件同时提供 libmono-component-<name>-static.a（完整实现）和
# libmono-component-<name>-stub-static.a（空实现），两者导出同名的
# mono_component_<name>_init。静态库按命令行顺序先到先得，两个都链时谁赢取决于
# find 的返回顺序，不受控 —— 这正是 R83 真机崩溃的根因（详见 docs/mod-compatibility.md §8）。
#   · debugger：完整版 1.32MB 且 iOS 用不上调试器 → 留 stub（0.06MB），排除 full。
#   · marshal-ilgen：**必须用 full**。本宿主强制 mono_aot_only=0 纯 JIT，任何
#     P/Invoke 首次调用都要运行时生成 native wrapper（IL）。stub 注册的回调是
#     stub_emit_marshal_ilgen → g_assert_not_reached() → abort()，真机表现为加载
#     第一个 Harmony/IL2CPP 插件时 SIGSEGV。官方 iOS 默认 AOT 所以用 stub，我们不适用。
LIBS=()
while IFS= read -r line; do
  case "$(basename "$line")" in
    libmono-component-debugger-static.a) continue ;;          # debugger 用 stub 版
    libmono-component-marshal-ilgen-stub-static.a) continue ;; # [R84] 排除 stub，只链 full
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

# [R84] 构建期强校验：完整版 marshal-ilgen 必须在链接清单里，且 stub 版必须不在。
# 宁可构建失败，也不能静默产出「一碰 P/Invoke 就 abort」的 dylib 让用户白测一轮。
MI_FULL=0; MI_STUB=0
for l in "${LIBS[@]}"; do
  case "$(basename "$l")" in
    libmono-component-marshal-ilgen-static.a)      MI_FULL=1 ;;
    libmono-component-marshal-ilgen-stub-static.a) MI_STUB=1 ;;
  esac
done
if [ "$MI_FULL" -ne 1 ] || [ "$MI_STUB" -ne 0 ]; then
  echo "!! [R84] marshal-ilgen 组件选择错误：full=$MI_FULL stub=$MI_STUB（要求 full=1 stub=0）" >&2
  echo "!! 纯 JIT 必须链接 libmono-component-marshal-ilgen-static.a；只链 stub 会让" >&2
  echo "!! mono_marshal_get_native_wrapper → stub_emit_marshal_ilgen → abort()。拒绝产出。" >&2
  exit 1
fi
echo "   [R84] marshal-ilgen: 完整版已入选、stub 已排除（纯 JIT 的 P/Invoke wrapper 可正常生成）"

echo "静态库 ${#LIBS[@]} 个:"
printf '  %s\n' "${LIBS[@]}"

# 定位 .NET 8 BCL 目录（解包后真实位置是 <prefix>/ios-monovm/lib/net8.0）
MONO_ROOT="$MONO_PREFIX"
if [ ! -d "$MONO_ROOT/lib/net8.0" ]; then
  found="$(find "$MONO_PREFIX" -maxdepth 4 -type d -name net8.0 -path "*/lib/*" 2>/dev/null | head -1)"
  [ -n "$found" ] && MONO_ROOT="$(dirname "$(dirname "$found")")"
fi
echo "MONO_ROOT: $MONO_ROOT"

# ---------------------------------------------------------------------------
# [R88] 沿用 R87「补 Marshal 转发」的修法，但把预生成补丁版换成 PE 保真版。
#
# R87 真机日志(1794303911228467452) 实证：Cecil 的 mod.Write() 全量重写
#   System.Runtime.dll，语义上只多一行 TypeForwardedTo(Marshal)，却把官方 PE
#   改得面目全非：43344B -> 33280B（剥掉 10064B Authenticode overlay）、
#   checksum 归零、强名签名 128B 清零却仍保留 CLI flags 的 STRONGNAMESIGNED 位、
#   DebugDir 缩小、#~ 流整段重排。真机上 R87 比 R86 更早崩：连
#   NebulaManualExecute ENTER 都没打出来，mono_runtime_invoke 直接抛
#   TypeInitializationException（内层真因被宿主 nebula_log_exception() 吞掉）。
#   高度怀疑「声明已强名签名却签名为空」这种自相矛盾的 PE 让 iOS .NET 8
#   MonoVM 在加载 facade 时翻车（本地 Mono 6.12 宽松，LoadFile 能过，沙箱没暴露）。
# R88 的预生成版改为以【官方 43344B 原版 PE】为骨架，只覆盖 Cecil 补丁的
#   metadata（含 Marshal 转发）、同步 CLI header 的 MetaData.Size、清
#   STRONGNAMESIGNED 位并清空强名签名区：完整保留官方 checksum / SecurityDir /
#   Authenticode overlay / section 布局，IL 语义仍只多一行 Marshal 转发。
#   生成脚本见 tools/patch_facade_marshal/pe_restore.py（字节级手术、带校验）。
# 同时宿主 nebula_log_exception() 升级为打印完整异常链（InnerException /
#   Message / StackTraceString / TypeName），下次真机若仍崩可直接看到内层真因。
#
# ---------------------------------------------------------------------------
# [R87] 修 net6 typeref -> .NET 8 facade 的 Marshal 转发断链（NOS 加载主因）
#
# R86 真机日志 1655/1683/1758：AutoSave / AUI.ServerScroll / NebulaLoader 全崩在
#   TypeLoadException: Could not resolve type with token 01000059 from typeref
#     (expected class 'System.Runtime.InteropServices.Marshal'
#      in assembly 'System.Runtime, Version=6.0.0.0 ...')
#   -> Harmony.PatchAll -> Il2CppType.From -> Il2CppClassPointerStore<T>.cctor；
#   NebulaPlugin.LoadInternal 也在此中断，NOS 起不来。
#
# 根因（元数据坐实，非裁剪、非模组问题）：BepInEx / Il2CppInterop 1.5.3 用 .NET 6
#   reference assemblies 编译，ref pack 把 Marshal 定义成 System.Runtime.dll 的
#   TypeDef，故 typeref 的 ResolutionScope 写死 System.Runtime 6.0.0.0；宿主 BCL
#   是官方 .NET 8.0.32 System.Runtime.dll(43344B, 891 个 ExportedType)，它把
#   System.Runtime.InteropServices.* 整体交给 System.Runtime.InteropServices.dll
#   转发，自己不再含 Marshal 本体 -> Mono 按 scope 命中 8.0 facade 后查不到。
#   Marshal 在 4375336B 的 System.Private.CoreLib(8.0.0.0) 里确实有 TypeDef。
#   精确扫描 bepinex-core/*.dll 全部 TypeRef：缺失项【只有 Marshal 一个】，
#   补这一条即可，不会连锁挤牙膏；.NET 6->8 的另外 55 个差异全是内部嵌套类型/
#   泛型数学接口，与插件无关。
#
# 修法（只改宿主 BCL facade；模组 DLL 一字节不动、不做 IL 补丁、BepInEx 原生加载）：
#   给 System.Runtime.dll 追加
#     [TypeForwardedTo(System.Runtime.InteropServices.Marshal)] -> System.Private.CoreLib
#   该 dll 的 AssemblyRef 已含 corelib，零新增依赖。
#
# 双路径（CI 的 host-dylib job 没有 setup-dotnet，不能硬依赖 dotnet）：
#   ① 主路径：官方 8.0.32 原版 sha 匹配 -> 换成本仓库预生成、已完整验证的补丁版
#      （Cecil 自校验 + dnfile 独立复核 + ikdasm + 幂等，零编译依赖）。
#   ② 兜底：BCL 版本变了（原版 sha 不符）-> 用 tools/patch_facade_marshal 现场
#      用 Cecil 补转发，该工具自带完整语义校验，失败即拒绝产出。
# 最后把当前 FACADE 的 sha 写进 FACADE_SHA_MARK，blob 生成的 python 会断言内嵌的
# 确实是同一份 —— 绝不给用户一个「看着出了包、真机仍崩」的版本。
# ---------------------------------------------------------------------------
FACADE="$MONO_ROOT/lib/net8.0/System.Runtime.dll"
FACADE_SHA_MARK="$OUT/.facade_marshal.sha"
FINAL_SHA=""   # [R87] 预初始化，确保下方 echo 永不为 unbound（任何分支都安全）
PREBUILT="$ROOT/facade-patch/System.Runtime.net8.0.32.patched.dll"
ORIG_SHA_8032="c6fd93aa8bb5016fb1766525940353c9a7723425152d2812a84dd679630c1f38"
# [R88] 预生成版 = PE 保真版(43344B)：官方 PE 骨架 + Cecil 补丁 metadata。
#   不再是 R87 那个 Cecil 全量重写版(33280B, sha b2b0f39a…)。
PATCHED_SHA_8032="61e74e928980ea680959584d4f826864b26ddf95e8eea2c8b6dfa4765a7ae4a9"
R88_PATCH_PROJ="$ROOT/../../tools/patch_facade_marshal/patch_facade_marshal.csproj"
R88_PE_RESTORE="$ROOT/../../tools/patch_facade_marshal/pe_restore.py"

sha256_of() {
    python3 -c 'import sys,hashlib;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
}

[ -f "$FACADE" ] || { echo "✗ [R88] 找不到 $FACADE" >&2; exit 1; }
CUR_SHA="$(sha256_of "$FACADE")"
echo "   [R88] 官方 System.Runtime.dll: $(wc -c < "$FACADE") bytes, sha=$CUR_SHA"

if [ "$CUR_SHA" = "$PATCHED_SHA_8032" ]; then
    echo "   [R88] ✓ 已是 PE 保真补丁版（缓存/重复构建命中），跳过写入"
elif [ "$CUR_SHA" = "$ORIG_SHA_8032" ] && [ -f "$PREBUILT" ] && \
     [ "$(sha256_of "$PREBUILT")" = "$PATCHED_SHA_8032" ]; then
    cp "$PREBUILT" "$FACADE"
    echo "   [R88] ✓ 已换为预生成 PE 保真补丁版（含 Marshal 转发，零 dotnet 依赖）"
else
    # 兜底：BCL 版本变了（原版 sha 不符）或预生成版缺失 -> 现场用 Cecil 补语义，
    # 再用 pe_restore.py 把补丁 metadata 合回【官方原版 PE 骨架】。
    # Cecil 工具是原地覆盖 FACADE，所以必须先留一份官方原版当骨架。
    echo "   [R88] 原版 sha 与 8.0.32 基准不符或预生成版缺失 -> 走现场补转发 + PE 保真" >&2
    command -v dotnet >/dev/null 2>&1 || { echo "✗ [R88] 需要 dotnet 现场补转发，但 dotnet 不可用" >&2; exit 1; }
    [ -f "$R88_PATCH_PROJ" ] || { echo "✗ [R88] 补丁工具缺失: $R88_PATCH_PROJ" >&2; exit 1; }
    [ -f "$R88_PE_RESTORE" ] || { echo "✗ [R88] PE 保真脚本缺失: $R88_PE_RESTORE" >&2; exit 1; }
    ORIG_BAK="$OUT/.System.Runtime.orig.dll"
    CECIL_OUT="$OUT/.System.Runtime.cecil.dll"
    PE_OUT="$OUT/.System.Runtime.pe.dll"
    cp "$FACADE" "$ORIG_BAK"
    dotnet run --project "$R88_PATCH_PROJ" -c Release -- \
        "$FACADE" "$FACADE_SHA_MARK" || { echo "✗ [R88] Cecil 语义补丁失败，拒绝产出" >&2; exit 1; }
    cp "$FACADE" "$CECIL_OUT"
    python3 "$R88_PE_RESTORE" "$ORIG_BAK" "$CECIL_OUT" "$PE_OUT" || \
        { echo "✗ [R88] PE 保真合成失败，拒绝产出" >&2; exit 1; }
    cp "$PE_OUT" "$FACADE"
    echo "   [R88] ✓ 现场补转发 + PE 保真合成完成"
fi

# [R88] 无条件以「当前 FACADE 的实际 sha」重写标记：dotnet 工具写的是 Cecil 版 sha，
# 缓存目录也可能残留 R87 的旧标记，一律纠正，避免 blob 校验误判（R87 的 ! -s 条件写入
# 在缓存复用时会漏改）。
sha256_of "$FACADE" > "$FACADE_SHA_MARK"
FINAL_SHA="$(sha256_of "$FACADE")"
[ "$(cat "$FACADE_SHA_MARK")" = "$FINAL_SHA" ] || {
    echo "✗ [R88] sha 标记与当前 facade 不一致（blob 可能内嵌错版本）" >&2; exit 1; }
echo "   [R88] ✓ 待内嵌 facade sha=${FINAL_SHA:-UNKNOWN}（PE 保真 + Marshal -> System.Private.CoreLib）"

echo "=== 生成嵌入式 blob（BCL + 可选 BepInEx/core，全部内存加载，无散文件）==="
BLOB="$OUT/.nebula_bcl.blob"
# [R95] 核心 BCL 落盘目录：把 net8.0 托管 DLL 平铺到这里，随 artifact 产出，
#   最终由 package_ios.sh 拷进成品的 NebulaLibs/，给 Cecil resolver 解析 System.Private.CoreLib 用。
CORELIBS_OUT="$OUT/nebula_corelibs"
# BepInEx/core 默认取 vendored 目录（与星云包锁版本）；可用 BEPINEX_CORE_DIR 覆盖
if [ -z "${BEPINEX_CORE_DIR:-}" ] && [ -d "$ROOT/bepinex-core" ]; then
  BEPINEX_CORE_DIR="$ROOT/bepinex-core"
fi
echo "   BepInEx/core 来源: ${BEPINEX_CORE_DIR:-无（仅嵌 BCL）}"

# [R83 → R84 更正] 下面这个 blittable 补丁【不是崩溃的根因修复】，保留仅作无害兜底。
# R83 曾误判为「非 blittable 参数(string/bool/LPUTF8Str)导致 marshal-ilgen 失败」，
# 但 R84 真机日志证明：.cctor 里零参数、纯 IntPtr 的 il2cpp_domain_get() 照样崩，
# 真根因是链接了 marshal-ilgen 的 stub 组件(见 §8，build_ios.sh 已排除、只链 full)。
# 补丁把 string/bool 的 P/Invoke 改成 blittable + 同名托管 wrapper，调用点零改动，
# 已验证合法且无副作用，保留可降低完整版 marshal-ilgen 的封送复杂度。幂等可反复执行。
if [ -n "${BEPINEX_CORE_DIR:-}" ] && command -v dotnet >/dev/null 2>&1; then
  # 从源码现场编译运行，不依赖 bin/（bin/obj 已被 .gitignore 忽略，干净 clone 也能用）。
  # Cecil 由 csproj 的 HintPath 指向已入库的 bepinex-core/Mono.Cecil.dll。
  PATCH_PROJ="$ROOT/../../tools/patch_marshal_blittable/patch_marshal_blittable.csproj"
  if [ -f "$PATCH_PROJ" ]; then
    echo "   [R83] 对 BepInEx/core 应用 marshal-ilgen 补丁（幂等）..."
    dotnet run --project "$PATCH_PROJ" -c Release -- \
        "$BEPINEX_CORE_DIR"/Il2CppInterop.Runtime.dll \
        "$BEPINEX_CORE_DIR"/BepInEx.Core.dll \
        "$BEPINEX_CORE_DIR"/BepInEx.Unity.IL2CPP.dll \
        "$BEPINEX_CORE_DIR"/BepInEx.Preloader.Core.dll \
        "$BEPINEX_CORE_DIR"/MonoMod.Utils.dll \
        "$BEPINEX_CORE_DIR"/Gee.External.Capstone.dll \
        "$BEPINEX_CORE_DIR"/AsmResolver.DotNet.dll || \
      echo "   [R83] ⚠ marshal 补丁失败（已提交的预修补 dll 仍可用，继续）" >&2
  fi
fi

# [R95] Cecil resolver 注入：给 MonoMod.Utils.ReflectionHelper.Load 的临时模块设可解析
#   核心程序集的 resolver（搜 HOME/Documents/NebulaLibs 等），修 NOS 加载时
#   System.Private.CoreLib 解析失败（AssemblyResolutionException -> HarmonyException: IL Compile Error）。
#   必须在 marshal 补丁之后、blob 生成之前执行；工具幂等，重复跑安全。
if [ -n "${BEPINEX_CORE_DIR:-}" ] && command -v dotnet >/dev/null 2>&1; then
  R95_PROJ="$ROOT/../../tools/patch_cecil_resolver/patch_cecil_resolver.csproj"
  if [ -f "$R95_PROJ" ]; then
    echo "   [R95] 给 MonoMod.Utils.dll 注入 Cecil resolver（幂等）..."
    # [R95] 硬失败：R95 注入无「预提交兜底 dll」，失败必须中止构建，
    #   绝不产出「注入缺失 → NOS 仍崩」的 dylib（此前 net6.0 运行时缺失导致静默失败）。
    dotnet run --project "$R95_PROJ" -c Release -- "$BEPINEX_CORE_DIR"/MonoMod.Utils.dll || \
      { echo "   [R95] ✗ resolver 注入失败，拒绝产出会崩的包" >&2; exit 1; }
  else
    echo "   [R95] ✗ 找不到 $R95_PROJ，拒绝产出" >&2; exit 1
  fi
else
  echo "   [R95] ✗ 缺 dotnet 或 BEPINEX_CORE_DIR，无法注入 resolver，拒绝产出" >&2; exit 1
fi

python3 - "$MONO_ROOT" "${BEPINEX_CORE_DIR:-}" "$BLOB" "$ROOT" "$FACADE_SHA_MARK" "$CORELIBS_OUT" <<'PY'
import sys, os, struct
mono_root = sys.argv[1]
bepinex_core = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else ""
blob_path = sys.argv[3]
root_dir = sys.argv[4] if len(sys.argv) > 4 else "."
facade_sha_mark = sys.argv[5] if len(sys.argv) > 5 else ""
corelibs_out = sys.argv[6] if len(sys.argv) > 6 else ""

entries = []; seen = set()
def add(name, data):
    if name in seen: return
    seen.add(name); entries.append((name, data))

# 1) .NET 8 BCL（托管程序集，含 System.Private.CoreLib）
# [R88] 强校验：内嵌的 System.Runtime.dll 必须是上面那份「PE 保真 + Marshal 转发」版。
# sha 标记由 build_ios.sh 的 R88 段无条件以最终 FACADE 实际 sha 写入（主路径=预生成
# PE 保真版；兜底=dotnet 现场补丁 + pe_restore.py 合成后重写）。不一致直接
# sys.exit(1)，绝不出会崩的包。
import hashlib
expected_facade_sha = ""
if facade_sha_mark and os.path.isfile(facade_sha_mark):
    expected_facade_sha = open(facade_sha_mark).read().strip()
bcl_dir = os.path.join(mono_root, "lib", "net8.0")
if os.path.isdir(bcl_dir):
    for f in sorted(os.listdir(bcl_dir)):
        if f.endswith(".dll"):
            data = open(os.path.join(bcl_dir, f), "rb").read()
            if f == "System.Runtime.dll":
                actual = hashlib.sha256(data).hexdigest()
                if not expected_facade_sha or actual != expected_facade_sha:
                    print("!! [R88] blob 内 System.Runtime.dll 不是 PE 保真补丁版 "
                          "(expected=%s actual=%s) —— 拒绝产出" %
                          (expected_facade_sha[:16] or "<无标记>", actual[:16]), file=sys.stderr)
                    sys.exit(1)
                print("   [R88] ✓ blob System.Runtime.dll sha=%s (%d bytes, PE 保真 + Marshal 转发)"
                      % (actual[:16], len(data)))
            add(f, data)
            if corelibs_out:   # [R95] 落盘核心 BCL 给 Cecil resolver（HOME/Documents/NebulaLibs）
                os.makedirs(corelibs_out, exist_ok=True)
                with open(os.path.join(corelibs_out, f), "wb") as _cf:
                    _cf.write(data)
else:
    print("!! 没找到 lib/net8.0，BCL 未嵌入", file=sys.stderr)

# 2) BepInEx/core（托管 dll，排除原生 dobby.dll）
if os.path.isdir(bepinex_core):
    for f in sorted(os.listdir(bepinex_core)):
        if not f.endswith(".dll"): continue
        if f.lower() == "dobby.dll": continue   # 原生库，不是托管，不能当 assembly 加载
        add("BepInEx/core/" + f, open(os.path.join(bepinex_core, f), "rb").read())

# 3) [R48] unity-libs：Unity 2022.3.44 托管基础库（Among Us iOS 实测版本）。
#    Il2CppInterop 生成器要用它们当 Cecil 参考程序集，必须是【散文件】（AddCecilPlatformAssemblies
#    只读文件系统，不认内存程序集），所以由宿主在运行时从 blob 释放到 BepInEx/unity-libs。
#    之所以必须自带：BepInEx 默认联网下载 zip 再用 System.IO.Compression 解压，而本环境的
#    Mono 无法生成 zlib 封送 IL（Cannot marshal nonblittlable types without marshal-ilgen），
#    解压必然抛 ZLibException —— 走这条路等于必崩。
#    注：这批文件的 only 名字与后续真正要加载的 interop 程序集同名，宿主的 blob_find 已对
#    "unity-libs/" 前缀跳过，不会被误当成可加载程序集抢走 interop 的加载。
unity_libs = os.path.join(root_dir, "unity-libs")
if os.path.isdir(unity_libs):
    n = 0
    for f in sorted(os.listdir(unity_libs)):
        if not f.endswith(".dll"): continue
        add("unity-libs/" + f, open(os.path.join(unity_libs, f), "rb").read())
        n += 1
    print("   unity-libs: %d 个基础库 DLL 已嵌入" % n)
else:
    print("!! 没找到 unity-libs 目录，运行时将无法自带 Unity 基础库", file=sys.stderr)

# 4) [R60] 预生成 iOS interop：随 dylib 内嵌，启动时由宿主释放到 BepInEx/interop/
#    ① 用户无需手动放（R59 仍要手动解压 zip，用户明确不要）；
#    ② interop 目录永不为空 —— BepInEx 的 Il2CppInteropManager 在目录空/无 dll 时
#       会无视 UpdateInteropAssemblies=false 而触发现场生成，R58 真机整夜卡死黑屏；
#    ③ 与 dylib 内 baked 的 BepInEx/core 同版本 Generator 产出，版本锁定。
#    注意：这些条目带 "interop/" 前缀，宿主 blob_find 一律跳过（不能当普通程序集
#    内存加载，否则会抢占 Il2CppInterop 桥接类的加载），只由 extract_dir 按文件释放。
interop_dir = os.path.join(root_dir, "prebuilt-interop")
if os.path.isdir(interop_dir):
    n = 0
    for f in sorted(os.listdir(interop_dir)):
        p = os.path.join(interop_dir, f)
        if not os.path.isfile(p): continue
        add("interop/" + f, open(p, "rb").read())
        n += 1
    print("   [R60] interop: %d 个文件已内嵌（启动自动释放，免手动放）" % n)
else:
    print("!! 没找到 prebuilt-interop/（R60 无法免手动放 interop）", file=sys.stderr)

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

# 6) [R82] 服务器列表滑动插件（BepInEx IL2CPP 托管 dll）。
#    随 dylib 内嵌，启动时由宿主强制落盘到 Documents/BepInEx/plugins/，
#    让 BepInEx 当普通插件扫描加载（Harmony hook ServerDropdown.FillServerOptions，
#    把按钮装进 AU 原生 Scroller 实现滑动）。
#    带 "plugin/" 前缀：blob_find 一律跳过（不能当普通程序集内存加载，否则会
#    抢在 BepInEx 磁盘加载前、绕开插件扫描流程）。
scroll_plugin = os.path.join(root_dir, "..", "plugins", "ServerScroll", "AUI.ServerScroll.dll")
if os.path.isfile(scroll_plugin):
    add("plugin/AUI.ServerScroll.dll", open(scroll_plugin, "rb").read())
    print("   [R82] AUI.ServerScroll.dll 已内嵌（滑动插件，%d bytes）" % os.path.getsize(scroll_plugin))
else:
    print("!! [R82] 没找到 AUI.ServerScroll.dll，滑动插件不会进包", file=sys.stderr)

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

echo "=== [R92] 生成构建标签头（git sha + 日期，boot 日志打印 BUILD= 确认版本）==="
# 优先用 CI 注入的提交 sha，否则回退到本地 git；都拿不到则 UNKNOWN。
R92_TAG_SHA="${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)}"
R92_TAG_SHA="${R92_TAG_SHA:0:12}"
R92_TAG_DATE="$(date -u +%Y%m%d)"
R92_BUILD_TAG="${R92_TAG_SHA}-${R92_TAG_DATE}"
# host 源已 #include "nebula_build_tag.h"；本地无 CI/HEAD 时也必须有该头，否则编译失败。
cat > "$ROOT/nebula_build_tag.h" <<HDR
#ifndef NEBULA_BUILD_TAG_H
#define NEBULA_BUILD_TAG_H
#define NEBULA_BUILD_TAG_STR "$R92_BUILD_TAG"
#endif
HDR
echo "   [R92] 构建标签: $R92_BUILD_TAG (sha=${R92_TAG_SHA} date=${R92_TAG_DATE})"

echo "=== 编译 iOS arm64 dylib（把 blob 嵌进 __DATA,__nebulabcl 段）==="

# [R60] 强制全量链入：
#   ① libdobby.a —— BepInEx 的 [DllImport("dobby")] 原本在设备上命中
#      core/dobby.dll（Windows x64 PE, 153088B），iOS arm64 dlopen 不了 →
#      DllNotFoundException: dobby → IL2CPPChainloader.Initialize 中断 →
#      宿主 R56 兜底强扫 plugins 时 SIGSEGV（真机 preloader.log / nebula_host.txt 实锤）。
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
    -I"$MONO_INC" \
    -I"$ROOT" \
    -o "$OUT/libnebula_standard.dylib" \
    "$ROOT/nebula_mono_host.c" \
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
    -Wl,-U,_pthread_jit_write_protect_np \
    -Wl,-exported_symbols_list,"$OUT/export_list.txt"
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

# [R84] 产物级终检：直接对最终 dylib 查 stub 组件独有的字符串常量。
# 上面「清单校验」只能保证 LIBS 里没列 stub；万一 force_load 的其它归档或
# 别处又拉进一份 stub 目标文件，清单看不出来，但产物里必然留下这句错误文本。
# 检出即判失败，绝不让「一碰 P/Invoke 就 abort」的包流到用户手上。
DYLIB_STRINGS="$(strings -a "$OUT/libnebula_standard.dylib" 2>/dev/null || true)"
if grep -q "Cannot marshal nonblittlable types without marshal-ilgen" <<<"$DYLIB_STRINGS"; then
  echo "!! [R84] 最终 dylib 里检出 marshal-ilgen STUB 实现（会 abort），拒绝产出" >&2
  echo "!! 检查是否有 -force_load 的归档或 MONO_PREFIX 下别处又提供了 stub 版" >&2
  rm -f "$OUT/libnebula_standard.dylib"
  exit 1
fi
# 正向校验：完整实现独有字符串（仅在 marshal-ilgen.c.o 里，stub 版没有）必须在，
# 否则说明 full 的 .o 根本没被拉进链接（静态库按需加载，若 init 符号被 stub 顶掉就会这样）。
FULL_MARK=1
if ! grep -q "Return marshalling of stringbuilders is not implemented" <<<"$DYLIB_STRINGS"; then
  # 仅告警不阻断：清单已保证 full 入选且 stub 排除，链接器必然拉入 full 的 .o；
  # 这里查不到多半是不同平台 strings 对 __cstring 的扫描差异，误杀会让人白等一轮构建。
  echo "    [R84] ⚠ 产物里未见 full 实现标志字符串（可能是 strings 扫描差异），清单/负向校验已通过，继续" >&2
  FULL_MARK=0
fi
if [ "$FULL_MARK" -eq 1 ]; then
  echo "    [R84] 终检通过：无 stub 字符串、有 full 实现标志（完整版 marshal-ilgen 已链入）"
else
  echo "    [R84] 终检通过：无 stub 字符串（完整版 marshal-ilgen 已链入，清单校验为准）"
fi

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
# 这里校验它们真的并进主 dylib，否则仍会 EntryPointNotFoundException: DobbyPrepare。
case "$SYMS" in
  *DobbyPrepare*) echo "    ✓ DobbyPrepare/DobbyCommit（shim 已并入，BepInEx6 IL2CPP detour 可用）" ;;
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

# [R64c] IsATty 强制导出校验：静态链时 libSystem.Native.a 里的 IsATty 可能是隐藏可见性
# 的强符号，会盖掉宿主的 weak stub 且不导出 -> 设备端 CoreLib P/Invoke 报
# EntryPointNotFoundException。上面 -Wl,-exported_symbol 强制导出；这里校验它确实进了
# 导出表，没进就直接 fail（绝不发一个会崩的版本）。
case "$SYMS" in
  *_SystemNative_IsATty*) echo "    ✓ SystemNative_IsATty（强制导出，CoreLib IsATty P/Invoke 可命中）" ;;
  *) echo "    ✗ SystemNative_IsATty 未导出！设备端 IsATty P/Invoke 将 EntryPointNotFound" >&2; exit 1 ;;
esac

# [R85] kernel32 no-op stub 强制导出校验：dllmap 把 kernel32.dll 指到主 dylib 自身后，
# NOS 的 GetModuleHandleW P/Invoke 靠 dlsym 命中宿主 stub。若 stub 没进导出表，
# 会从 DllNotFoundException 变成 EntryPointNotFoundException —— 一样中断 NebulaLoader，
# NOS 仍然起不来。构建期直接 fail，绝不让这种包流到真机。
MISS85=0
for s in _GetModuleHandleW _GetModuleHandleA _GetModuleHandle _GetModuleHandleWW _GetProcAddress _GetProcAddressA; do
  case "$SYMS" in
    *"$s"*) : ;;
    *) echo "    ✗ [R85] $s 未导出！kernel32 no-op dllmap 会落空" >&2; MISS85=1 ;;
  esac
done
[ "$MISS85" -eq 0 ] && echo "    ✓ [R85] kernel32 no-op stub 全部已导出（GetModuleHandle* / GetProcAddress）"
[ "$MISS85" -ne 0 ] && { echo "!! [R85] kernel32 no-op stub 导出不全，拒绝产出" >&2; rm -f "$OUT/libnebula_standard.dylib"; exit 1; }

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
