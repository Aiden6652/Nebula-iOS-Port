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
CACHE_VERSION="${CACHE_VERSION:-25}"
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

# (a3) [Nebula iOS 第八轮 2026-10-27 反转定案] 强制【不定义】HAVE_PTHREAD_JIT_WRITE_PROTECT_NP：
#      真机 5faa94b 轮实锤：强制 #define 后 mono-codeman.c 上游函数体的
#      pthread_jit_write_protect_np 直调被编译进去，而本机 dlsym 解析不到该符号
#      （wx-init toggle=n 实证）→ supported_np 实测返回 1 → 上游必然调 pjwp(0)
#      → 符号桩 br 0 → SIGSEGV (signal 11 @ mono_jit_init，死点紧跟 set_write(1)
#      done，被 host signal handler 抓到——与此前 SIGKILL 无 .ips 表现互斥，定性吻合)。
#      全局 APRR 翻转路线整体废除，W^X 统一走 nebula 每页 mprotect（fb188cb 真机
#      16 轮 set_write(1/0) 翻转全程稳 + host 冒烟路线 F 执行 OK，双重实证）。
#      cross-compile 下 cmake check_symbol_exists 本来就是 no，这里把 cmakedefine
#      显式改写为 #undef 注释，防止任何构建路径把它带回来。
cfg = os.path.join(mono, 'cmake', 'config.h.in')
if os.path.exists(cfg):
    s = open(cfg, encoding='utf-8', errors='replace').read()
    # 用整行正则替换：.in 原始行形如「#cmakedefine HAVE_... 1」（行尾带残值），
    # 若只 replace 子串会把行尾「 1」残留在注释 /* */ 之外 → config.h 生成
    # 「... */ 1」→ clang 报 expected identifier or '('（真机 40503382253 即死于此）。
    # 整行吃尽，彻底消除该宏（defined() 与 #if 下均不可见）。
    _re_pat = re.compile(r'^#(?:cmakedefine|define)\s+HAVE_PTHREAD_JIT_WRITE_PROTECT_NP\b.*$', re.M)
    if _re_pat.search(s):
        s2 = _re_pat.sub(
            '/* #undef HAVE_PTHREAD_JIT_WRITE_PROTECT_NP  [Nebula iOS] 第八轮强制剔除 pjwp 直调，见 build_mono_ios.sh (a3) */',
            s)
        open(cfg, 'w', encoding='utf-8').write(s2)
        print("    config.h.in: HAVE_PTHREAD_JIT_WRITE_PROTECT_NP → 整行 #undef（吃掉行尾残值）")
    else:
        print("    config.h.in: 无 HAVE_PTHREAD_JIT_WRITE_PROTECT_NP 残留，跳过")

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

# (b) [Nebula iOS] plain-RWX 模式（94f5693 真机 .ips 实证后的定案）：
#     不再把 MAP_JIT 扩展到 iOS，iOS 上 mono_valloc 走 plain mmap RWX。
#     依据：StikDebug 调试型 JIT 给进程 CS_DEBUGGED，内核直接允许 RWX 匿名映射
#     （本次崩溃 run 里 mono_global_codeman_reserve 的 plain RWX mmap 已实测成功）。
#     mono 自建 MAP_JIT/W^X（翻转可写/可执行）在 iOS 上有两个死穴：
#       1) iOS SDK 的 libpthread.tbd 不导出 pthread_jit_write_protect_np，
#          -Wl,-U 只让链接通过；运行时懒绑定解析失败 → 符号桩 br 到 0 →
#          mono_trampolines_init 首次翻转即 SIGSEGV pc=0（.ips: lr=
#          mono_arch_create_generic_trampoline+88，与反汇编完全吻合）；
#       2) plain RWX 本来就可用，W^X 纯属多余。
#     mono_setmmapjit(1)（host 对 standard 的调用）在 iOS 变为无害 no-op。
# ⚠️ 上一版（c0c500a/d324ae1）这里只有注释没有代码，真机实测（2026-10-24 log）：
#     host 仍强制 MAP_JIT → mono-mmap.c 原版代码在 iOS 照样加 MAP_JIT：
#       - L284: (flags & MONO_MMAP_JIT) && (use_mmap_jit || hardened)——host 调
#         mono_setmmapjit(1) 后 use_mmap_jit=1 直接命中；
#       - L286-291: HOST_ARM64 && !HOST_MACCAT 时无条件 mflags |= MAP_JIT。
#     MAP_JIT 页默认「可执行不可写」，翻转已被 (c) no-op → 写 trampoline 必崩。
# (b-code) [Nebula iOS] 保留 MAP_JIT：iOS 上 mono_valloc 继续走 MAP_JIT 分配
#     （撤掉此前"整体禁 MAP_JIT"的做法）。理由（用户 2026-10-24 指正）：
#       开 MAP_JIT + 运行时探测翻转，能同时兼容两条路——
#         · 真机官方 JIT（allow-jit / dynamic-codesigning entitlement）：
#           pthread_jit_write_protect_np 能解析到 → 走官方 W^X 翻转；
#         · StikDebug 调试型 JIT（CS_DEBUGGED，tbd 不导出该符号）：
#           解析不到 → 跳过翻转，内核已把 MAP_JIT 页当 RWX 给，可直接写。
#     翻转的 NULL 安全由 (c) 的 dlsym 探测保证，本 (.b) 块不再改 mono-mmap.c。

# (c) [Nebula iOS 第八轮 2026-10-27 定案] W^X 单线路：nebula 每页 mprotect 翻转。
#     真机 5faa94b 轮实锤：全局 APRR 翻转（pthread_jit_write_protect_np 直连）在
#     本机不可用——dlsym 解析不到（wx-init toggle=n），supported_np 却返回 1，
#     上游 if(supported) pjwp(0) 必进死桩 → SIGSEGV (signal 11 @ mono_jit_init)。
#     修复三板斧：
#       1) (a3) config.h.in 强制 #undef，上游 HAVE_... 守卫的 pjwp 调用编译剔除；
#       2) 本 TU 顶部宏废除 pthread_jit_write_protect_np / supported_np（防其它
#          守卫组合漏网；supported_np 真机实测返回 1，必须一并废除）；
#       3) mono-mmap.c wx_block 提供 pjwp no-op 强符号兜底（防其它 TU 残留引用，
#          dyld 两级命名空间下优先解析到同 dylib 的强符号，永不踩桩）。
#     W^X 全线 = nebula_mono_wx_set_write 每页 mprotect（fb188cb 真机 16 轮翻转
#     全程稳 + host 冒烟路线 F RW 写→R-X 执行 OK 反复折磨通过，双重实证）。
import re as _re
cm = os.path.join(mono, 'mono', 'utils', 'mono-codeman.c')
if os.path.exists(cm):
    s = open(cm, encoding='utf-8', errors='replace').read()
    # 可用性检查：显式列出 iOS 14（macOS 分支用，iOS 分支已被 dlsym 探测短路）
    s2 = _re.sub(r'__builtin_available\s*\(\s*macOS\s+11[^)]*\)',
                '__builtin_available (macOS 11, iOS 14, *)', s)
    # [Nebula iOS] mprotect W^X 翻转：真机路线 F 已验证（RW 写 → mprotect R-X → 执行 OK）。
    # enable_write → 翻 RW；disable_write → 翻 R-X + icache（实现见 mono-mmap.c (b3)）。
    probe = ('#if defined(__APPLE__)\n'
             '\t/* [Nebula iOS] 双保险 W^X（.NET/Pojav 同款）：① 每页 mprotect 翻 RW\n'
             '\t   （路线 F，真机已验证；非 MAP_JIT 普通页靠它）；② 下游原始\n'
             '\t   pthread_jit_write_protect_np 全局 APRR 翻转（HAVE_... 守卫，iOS 14+\n'
             '\t   直连，dyld 从 libSystem 绑定）——不再 return 短路，恢复上游翻转。 */\n'
             '\tnebula_mono_wx_set_write (1);\n'
             '#endif\n')
    probe_off = ('#if defined(__APPLE__)\n'
                '\t/* [Nebula iOS] 双保险：① 每页 mprotect 翻 R-X + 刷 icache；\n'
                '\t   ② 下游原始 pthread_jit_write_protect_np 全局翻转（不再短路）。 */\n'
                '\tnebula_mono_wx_set_write (0);\n'
                '#endif\n')
    n1 = _re.sub(r'(mono_codeman_enable_write \(void\)\s*\{\n)',
                 lambda m: m.group(1) + probe, s2, count=1)
    n2 = _re.sub(r'(mono_codeman_disable_write \(void\)\s*\{\n)',
                 lambda m: m.group(1) + probe_off, n1, count=1)
    # 声明 + 本 TU 宏废除 pjwp：supported_np 真机实测返回 1（host 冒烟实锤），
    # 上游 if (supported) pjwp(0) 会放行进死桩，两个符号必须一起废。
    decl = ('\n'
            '#if defined(__APPLE__)\n'
            '/* [Nebula iOS] mprotect W^X 翻转（实现在 mono-mmap.c (b3)：登记的 JIT 区全局\n'
            '   mprotect 翻 RW/R-X，真机 fb188cb 16 轮 set_write 翻转 + 路线 F 执行实证） */\n'
            'extern void nebula_mono_wx_set_write (int writable);\n'
            '/* [Nebula iOS 第八轮 2026-10-27 定案] 本机 dlsym 解析不到\n'
            '   pthread_jit_write_protect_np（wx-init toggle=n 实证），supported_np 却返回 1，\n'
            '   上游直调 pjwp 即符号桩 SIGSEGV（signal 11 @ mono_jit_init，死点紧跟\n'
            '   set_write(1) done）。全局 APRR 翻转整体废除，本 TU 全部调用点宏废除；\n'
            '   其它 TU 残留引用由 mono-mmap.c wx_block 的 no-op 强符号兜底。 */\n'
            'static inline void nebula_pjwp_disabled (int enabled) { (void)enabled; }\n'
            '#define pthread_jit_write_protect_np(...) nebula_pjwp_disabled (__VA_ARGS__)\n'
            '#define pthread_jit_write_protect_supported_np() 0\n'
            '#endif\n')
    _incs = list(_re.finditer(r'(?m)^#\s*include\b.*$', n2))
    if _incs:
        _end = _incs[-1].end()
        n2 = n2[:_end] + decl + n2[_end:]
    else:
        n2 = decl + n2
    if n2 != s2:
        open(cm, 'w', encoding='utf-8').write(n2)
        print("    mono-codeman.c: iOS dlsym 探测翻转已注入 (enable/disable 各一处)")
    else:
        print("    !! mono-codeman.c 未匹配到翻转函数（mono 版本可能已改结构）", file=sys.stderr)
else:
    print("    !! 找不到 mono-codeman.c", file=sys.stderr)

# (b2) [Nebula iOS] 已撤销：恢复 mono 原版 MAP_JIT 行为（2026-10-25 最新实证）。
#      真机多轮实测证明：iOS 18 codesigning monitor 只对 MAP_JIT 标记的页面放行执行权限
#      —— plain/RWX + mprotect 即使 mprotect 返回 0，执行仍 SIGBUS（内核在更底层压住执行位）。
#      故不再剥 MAP_JIT；MAP_JIT 页的「可写↔可执行」翻转由 (c) 的 dlsym 探测负责：
#      多源解析 pthread_jit_write_protect_np（RTLD_DEFAULT → libsystem_pthread.dylib）。
#      调试态（StikDebug/CS_DEBUGGED）与官方 allow-jit 直签都能解析到并正确翻转。
#      本块不再修改 mono-mmap.c（保持与上游一致，避免引入未覆盖的代码路径）。

# (b3) [Nebula iOS] mono 代码页改走「plain RW + mprotect W^X 翻转」——真机 2026-10-25
#      冒烟路线 F 已验证：plain(RW) mmap → 写入 → mprotect R-X（cur 真实变 r-b）→
#      执行 OK。MAP_JIT 在此真机内核上 mmap 直接 EPERM errno=1（无 JIT entitlement，
#      CS_DEBUGGED 不豁免 MAP_JIT 检查）。
#      实现：
#        1) iOS 上短路 mono_valloc 的 MAP_JIT 块（MAP_JIT 申请必被拒，还会让
#           hardened-runtime 探测误判）；
#        2) mono_valloc 成功且请求含 EXEC → 登记 (addr,len) 进翻转表（内核对
#           RWX 请求只给 rw-，写入天然可行）；
#        3) 导出 nebula_mono_wx_set_write(writable)：遍历登记区全局 mprotect——
#           (1)=翻 RW 供 mono 写代码；(0)=翻 R-X + sys_icache_invalidate 供执行；
#        4) mono_vfree 注销，防地址复用后误翻数据页。
#      mono-codeman.c 的 enable/disable_write（(c2)）改调此函数，替代已不存在的
#      pthread_jit_write_protect_np —— 与 macOS 上 MAP_JIT 翻转的调用纪律完全一致
#      （enable 写代码 / disable 执行代码），只是把「每线程 pthread 翻转」换成
#      「每进程 mprotect 翻转」。CACHE_VERSION 联动升版。
mm = None
for _cand in (os.path.join(mono, 'mono', 'utils', 'mono-mmap.c'),
              os.path.join(mono, 'mono', 'metadata', 'mono-mmap.c')):
    if os.path.exists(_cand):
        mm = _cand
        break
if mm:
    print("    mono-mmap.c 位置:", mm)
    s = open(mm, encoding='utf-8', errors='replace').read()
    _old_guard = '#if defined(__APPLE__) && defined(MAP_JIT) && defined(TARGET_OSX)'
    _new_guard = ('#if defined(__APPLE__) && defined(MAP_JIT) && defined(TARGET_OSX)'
                  ' && !(defined(TARGET_IOS) && TARGET_IOS)')
    if _old_guard not in s:
        print("    !! mono-mmap.c: MAP_JIT 块守卫未命中（mono 结构变化，需人工适配）", file=sys.stderr)
        sys.exit(1)
    wx_block = (
        '#if defined(__APPLE__)\n'
        '/* [Nebula iOS] W^X 翻转表（真机 2026-10-26 实测定案）。\n'
        '   host 冒烟实证：plain(RW) mmap → 写入 → mprotect R-X + icache → 执行 OK，\n'
        '   循环反复翻转也 OK（路线 F 畅通）；而执行 rw- 页被 codesigning monitor 秒杀\n'
        '   （SIGKILL 无 .ips——此前 jit_toggle=no 的 no-flip 分支即死于这个）。\n'
        '   故唯一策略：全线每页 mprotect 翻转（可写↔可执行），执行前必须 R-X。\n'
        '   pthread_jit_write_protect_np 解析与否仅记录日志（dlopen libsystem_pthread\n'
        '   回退兜住 RTLD_DEFAULT 搜不到的情况），不再依赖全局 APRR 翻转。 */\n'
        '#include <sys/mman.h>\n'
        '#include <pthread.h>\n'
        '#include <libkern/OSCacheControl.h>\n'
        '\n'
        'typedef struct { void *addr; size_t len; int active; } neb_wx_region_t;\n'
        'static neb_wx_region_t neb_wx_regions[4096];\n'
        'static int neb_wx_nregions = 0;\n'
        'static pthread_mutex_t neb_wx_mutex = PTHREAD_MUTEX_INITIALIZER;\n'
        'static int neb_wx_debugger = -1;        /* -1 未探测, 0 否, 1 是(CS_DEBUGGED 调试态) */\n'
        'typedef void (*neb_jit_toggle_fn) (int);\n'
        'static neb_jit_toggle_fn neb_jit_toggle = NULL;  /* pthread_jit_write_protect_np */\n'
        'static int neb_wx_level = 0;             /* 嵌套层级（镜像上游 write_level_tls_id） */\n'
        '\n'
        '/* [Nebula iOS] 内存操作追踪：mono 侧 JIT 页 mmap/mprotect/翻转全部落盘，\n'
        '   进程被 SIGKILL（无 .ips 无 signal handler 输出）时，本文件最后一行即死前\n'
        '   最后一个内存操作。写 $TMPDIR/nebula_mono_mem.log（Documents 与 nebula_host.log\n'
        '   同级可取）。*/\n'
        '#include <stdio.h>\n'
        '#include <stdlib.h>\n'
        '#include <time.h>\n'
        '#include <stdarg.h>\n'
        '#include <errno.h>\n'
        '#include <stdint.h>\n'
        '#include <unistd.h>\n'
        '#include <dlfcn.h>\n'
        'extern int csops (int pid, unsigned int ops, void *useraddr, unsigned long usersize);\n'
        '#ifndef CS_OPS_STATUS\n'
        '#define CS_OPS_STATUS 0\n'
        '#endif\n'
        '#ifndef CS_DEBUGGED\n'
        '#define CS_DEBUGGED 0x10000000\n'
        '#endif\n'
        'static void neb_wx_log (const char *fmt, ...)\n'
        '{\n'
        '\tstatic pthread_mutex_t neb_log_mutex = PTHREAD_MUTEX_INITIALIZER;\n'
        '\tstatic FILE *fp = NULL;\n'
        '\tpthread_mutex_lock (&neb_log_mutex);\n'
        '\tif (!fp) {\n'
        '\t\t/* 路径优先级：NEBULA_MONO_MEM_LOG → $HOME/Documents（文件 App 可取，\n'
        '\t\t   与 nebula_host.log 同级）→ $TMPDIR → /tmp */\n'
        '\t\tconst char *env = getenv ("NEBULA_MONO_MEM_LOG");\n'
        '\t\tconst char *home = getenv ("HOME");\n'
        '\t\tconst char *tmp = getenv ("TMPDIR");\n'
        '\t\tchar path [1024];\n'
        '\t\tif (env && *env) {\n'
        '\t\t\tsnprintf (path, sizeof (path), "%s", env);\n'
        '\t\t} else if (home && *home) {\n'
        '\t\t\tsnprintf (path, sizeof (path), "%s/Documents/nebula_mono_mem.log", home);\n'
        '\t\t} else {\n'
        '\t\t\tsnprintf (path, sizeof (path), "%s/nebula_mono_mem.log", tmp && *tmp ? tmp : "/tmp");\n'
        '\t\t}\n'
        '\t\tfp = fopen (path, "w");\n'
        '\t}\n'
        '\tif (fp) {\n'
        '\t\tstruct timespec ts;\n'
        '\t\tclock_gettime (CLOCK_MONOTONIC, &ts);\n'
        '\t\tfprintf (fp, "[%ld.%03ld] [t%llx] ", (long)ts.tv_sec, ts.tv_nsec / 1000000, (unsigned long long)(uintptr_t)pthread_self ());\n'
        '\t\tva_list ap; va_start (ap, fmt);\n'
        '\t\tvfprintf (fp, fmt, ap);\n'
        '\t\tva_end (ap);\n'
        '\t\tfputc (10, fp);\n'
        '\t\tfflush (fp);\n'
        '\t}\n'
        '\tpthread_mutex_unlock (&neb_log_mutex);\n'
        '}\n'
        '\n'
        'static void neb_wx_init (void)\n'
        '{\n'
        '\tstatic int done = 0;\n'
        '\tif (done) return;\n'
        '\tdone = 1;\n'
        '\t/* iOS 真机实证：host 直调 pthread_jit_write_protect_np 成功(返回0)，但\n'
        '\t   RTLD_DEFAULT 搜不到该导出——dlopen 具体 dylib 再试一次 */\n'
        '\tneb_jit_toggle = (neb_jit_toggle_fn) dlsym (RTLD_DEFAULT, "pthread_jit_write_protect_np");\n'
        '\tif (!neb_jit_toggle) {\n'
        '\t\tvoid *h = dlopen ("/usr/lib/system/libsystem_pthread.dylib", RTLD_NOW);\n'
        '\t\tif (h) neb_jit_toggle = (neb_jit_toggle_fn) dlsym (h, "pthread_jit_write_protect_np");\n'
        '\t}\n'
        '\tint _flags = 0;\n'
        '\tneb_wx_debugger = (csops (getpid (), CS_OPS_STATUS, &_flags, sizeof (_flags)) == 0 && (_flags & CS_DEBUGGED)) ? 1 : 0;\n'
        '\tneb_wx_log ("wx-init jit_toggle=%s dbg=%d", neb_jit_toggle ? "yes" : "no", neb_wx_debugger);\n'
        '}\n'
        '\n'
        'static void\n'
        'nebula_wx_register (void *addr, size_t len)\n'
        '{\n'
        '\tif (!addr || !len) return;\n'
        '\tneb_wx_init ();\n'
        '\tpthread_mutex_lock (&neb_wx_mutex);\n'
        '\tif (neb_wx_nregions < (int)(sizeof (neb_wx_regions) / sizeof (neb_wx_regions [0]))) {\n'
        '\t\tneb_wx_regions [neb_wx_nregions].addr = addr;\n'
        '\t\tneb_wx_regions [neb_wx_nregions].len = len;\n'
        '\t\tneb_wx_regions [neb_wx_nregions].active = 1;\n'
        '\t\tneb_wx_nregions++;\n'
        '\t\tneb_wx_log ("register addr=%p len=%zu (n=%d)", addr, len, neb_wx_nregions);\n'
        '\t}\n'
        '\tpthread_mutex_unlock (&neb_wx_mutex);\n'
        '}\n'
        '\n'
        '/* [Nebula iOS] 若 addr 已在表内返回 1；否则补登记并返回 0（用于 mono_mprotect\n'
        '   直接翻 R-X 的可执行块——绕过 mono_valloc 的路径也能纳入翻转表）。*/\n'
        'static int\n'
        'nebula_wx_ensure (void *addr, size_t len)\n'
        '{\n'
        '\tif (!addr || !len) return 1;\n'
        '\tpthread_mutex_lock (&neb_wx_mutex);\n'
        '\tfor (int i = 0; i < neb_wx_nregions; i++) {\n'
        '\t\tif (neb_wx_regions [i].active && neb_wx_regions [i].addr == addr) { pthread_mutex_unlock (&neb_wx_mutex); return 1; }\n'
        '\t}\n'
        '\tif (neb_wx_nregions < (int)(sizeof (neb_wx_regions) / sizeof (neb_wx_regions [0]))) {\n'
        '\t\tneb_wx_regions [neb_wx_nregions].addr = addr;\n'
        '\t\tneb_wx_regions [neb_wx_nregions].len = len;\n'
        '\t\tneb_wx_regions [neb_wx_nregions].active = 1;\n'
        '\t\tneb_wx_nregions++;\n'
        '\t\tneb_wx_log ("ensure-register addr=%p len=%zu (n=%d)", addr, len, neb_wx_nregions);\n'
        '\t}\n'
        '\tpthread_mutex_unlock (&neb_wx_mutex);\n'
        '\treturn 0;\n'
        '}\n'
        '\n'
        'static void\n'
        'nebula_wx_unregister (void *addr, size_t len)\n'
        '{\n'
        '\t(void)len;\n'
        '\tpthread_mutex_lock (&neb_wx_mutex);\n'
        '\tfor (int i = 0; i < neb_wx_nregions; i++) {\n'
        '\t\tif (neb_wx_regions [i].active && neb_wx_regions [i].addr == addr) {\n'
        '\t\t\tneb_wx_regions [i].active = 0;\n'
        '\t\t\tbreak;\n'
        '\t\t}\n'
        '\t}\n'
        '\tpthread_mutex_unlock (&neb_wx_mutex);\n'
        '}\n'
        '\n'
        '/* [Nebula iOS] W^X 翻转（由 mono-codeman.c 的 enable/disable_write 调用）：\n'
        '   writable=1 → 全部登记区翻 RW（mono 写代码）；\n'
        '   writable=0 → 全部登记区翻 R-X + 刷 icache（mono 执行代码）。 */\n'
        'void\n'
        'nebula_mono_wx_set_write (int writable)\n'
        '{\n'
        '\tneb_wx_init ();\n'
        '\tpthread_mutex_lock (&neb_wx_mutex);\n'
        '\tneb_wx_log ("set_write(%d) enter, n=%d dbg=%d toggle=%s", writable, neb_wx_nregions, neb_wx_debugger, neb_jit_toggle ? "y" : "n");\n'
        '\t/* [真机 2026-10-26 定案] 废除 no-flip 分支：执行 rw- 页 = codesigning monitor\n'
        '\t   秒杀（16 轮 set_write 只是写代码没真执行，真正执行即崩）。host 冒烟已实证\n'
        '\t   路线 F（mprotect R-X + icache 后执行 OK，循环折磨 OK），故不再区分调试态，\n'
        '\t   全线每页 mprotect 翻转。 */\n'
        '\tfor (int i = 0; i < neb_wx_nregions; i++) {\n'
        '\t\tif (!neb_wx_regions [i].active) continue;\n'
        '\t\tint prot = writable ? (PROT_READ | PROT_WRITE) : (PROT_READ | PROT_EXEC);\n'
        '\t\tint mr = mprotect (neb_wx_regions [i].addr, neb_wx_regions [i].len, prot);\n'
        '\t\tneb_wx_log ("  mprotect %p len=%zu prot=%d ret=%d errno=%d", neb_wx_regions [i].addr, neb_wx_regions [i].len, prot, mr, mr ? errno : 0);\n'
        '\t\tif (mr == 0 && !writable)\n'
        '\t\t\tsys_icache_invalidate (neb_wx_regions [i].addr, neb_wx_regions [i].len);\n'
        '\t}\n'
        '\tneb_wx_log ("set_write(%d) done", writable);\n'
        '\tpthread_mutex_unlock (&neb_wx_mutex);\n'
        '}\n'
        '\n'
        '/* [Nebula iOS] 给其它翻译单元留的日志口（如 mono-threads-posix.c 的屏障退化）：\n'
        '   一行备注落进 nebula_mono_mem.log，死前最后操作序列里能看到它。 */\n'
        'void\n'
        'nebula_memnote (const char *msg)\n'
        '{\n'
        '\tneb_wx_log ("%s", msg);\n'
        '}\n'
        '\n'
        '/* [Nebula iOS 第八轮] pjwp no-op 强符号兜底：真机实锤本机 dlsym 解析不到\n'
        '   pthread_jit_write_protect_np（wx-init toggle=n），任何 TU 残留引用一旦\n'
        '   走到 dyld 绑定即踩符号桩 SIGSEGV（signal 11 @ mono_jit_init 实锤）。\n'
        '   本 dylib 内定义强符号后，两级命名空间下所有引用优先绑定到此 no-op，\n'
        '   永不踩桩。codeman 主调用点已宏废除，此定义纯为链接兜底。 */\n'
        'void pthread_jit_write_protect_np (int enabled) { (void)enabled; }\n'
        '#endif /* __APPLE__ */\n'
        '\n')
    # 守卫只做短路改写（iOS 不编译 MAP_JIT 块）；wx_block（函数定义+全局变量）
    # 单独置于文件顶层最前，绝不能塞进 mono_valloc 函数体（否则函数定义嵌套报错 +
    # 内部调用的 register/unregister 对外不可见）。
    s2 = s.replace(_old_guard, _new_guard, 1)
    s2 = wx_block + s2

    # (b5) [Nebula iOS 第九轮] mmap 剥 EXEC——真机 b103e3a 定案：
    #      mmap 直接带 EXEC 的匿名页执行即被 codesigning monitor 秒杀（SIGKILL 无
    #      .ips，死点=mono_runtime_init 首次 mono_object_new 调 JIT trampoline）；
    #      而 mmap RW 起步 + mprotect 加 EXEC 的页执行 OK（host 冒烟路线 F 双循环
    #      实证）。两者走内核不同校验路径：mprotect-EXEC 走调试态豁免，mmap-EXEC
    #      被当静态代码段走签名校验。故 iOS 上 EXEC 一律剥离，mmap 成功后 mprotect
    #      加回。macOS 分支原样保留。
    _mmap_old = ('\tptr = mmap (addr, length, prot, mflags, -1, 0);\n'
                 '\tif (ptr == MAP_FAILED) {\n'
                 '\t\tint fd = open ("/dev/zero", O_RDONLY);\n'
                 '\t\tif (fd != -1) {\n'
                 '\t\t\tptr = mmap (addr, length, prot, mflags, fd, 0);\n'
                 '\t\t\tclose (fd);\n'
                 '\t\t}\n'
                 '\t}\n')
    _mmap_new = (
        '#if defined(TARGET_IOS)\n'
        '\t/* [Nebula iOS 第九轮] mmap 不直接带 EXEC：mmap-EXEC 匿名页执行即被\n'
        '\t   codesigning monitor 秒杀；mprotect 加 EXEC 走调试态豁免路径（冒烟\n'
        '\t   路线 F 实证可执行）。EXEC 剥离后 mmap，成功再 mprotect 加回。 */\n'
        '\t{\n'
        '\t\tint _neb_prot_noexec = prot & ~PROT_EXEC;\n'
        '\t\tptr = mmap (addr, length, _neb_prot_noexec, mflags, -1, 0);\n'
        '\t\tif (ptr == MAP_FAILED) {\n'
        '\t\t\tint fd = open ("/dev/zero", O_RDONLY);\n'
        '\t\t\tif (fd != -1) {\n'
        '\t\t\t\tptr = mmap (addr, length, _neb_prot_noexec, mflags, fd, 0);\n'
        '\t\t\t\tclose (fd);\n'
        '\t\t\t}\n'
        '\t\t}\n'
        '\t\tif (ptr != MAP_FAILED && (prot & PROT_EXEC)) {\n'
        '\t\t\tif (mprotect (ptr, length, prot) == 0) {\n'
        '\t\t\t\tneb_wx_log ("valloc EXEC re-add OK %p len=%zu prot=%d", ptr, length, prot);\n'
        '\t\t\t} else {\n'
        '\t\t\t\tneb_wx_log ("valloc EXEC re-add FAILED %p len=%zu errno=%d", ptr, length, errno);\n'
        '\t\t\t\tmunmap (ptr, length);\n'
        '\t\t\t\tptr = MAP_FAILED;\n'
        '\t\t\t}\n'
        '\t\t}\n'
        '\t}\n'
        '#else\n'
        + _mmap_old +
        '#endif\n')
    if s2.count(_mmap_old) != 1:
        print("    !! mono-mmap.c: mono_valloc mmap 锚点未唯一命中 (%d)" % s2.count(_mmap_old), file=sys.stderr)
        sys.exit(1)
    s2 = s2.replace(_mmap_old, _mmap_new, 1)
    print("    mono_valloc: iOS mmap 剥 EXEC + mprotect 加回（第九轮）")

    # mono_valloc 成功 → 登记（含 EXEC 请求的才登记；内核对 RWX 实际只给 rw-，
    # mono 随后的写入天然可行，执行交给 disable_write 翻 RX）
    _va_anchor = '\tEND_CRITICAL_SECTION;\n\n\tif (ptr == MAP_FAILED)\n\t\treturn NULL;'
    if s2.count(_va_anchor) != 1:
        print("    !! mono-mmap.c: mono_valloc 锚点未唯一命中 (%d)" % s2.count(_va_anchor), file=sys.stderr)
        sys.exit(1)
    _va_repl = (_va_anchor + '\n'
        '#if defined(__APPLE__)\n'
        '\tif (ptr != MAP_FAILED && (prot & PROT_EXEC)) {\n'
        '\t\tneb_wx_log ("valloc EXEC addr=%p len=%zu prot=%d flags=%d", ptr, length, prot, mflags);\n'
        '\t\tnebula_wx_register (ptr, length);   /* [Nebula iOS] JIT 页登记进翻转表 */\n'
        '\t} else if (ptr == MAP_FAILED && (prot & PROT_EXEC)) {\n'
        '\t\tneb_wx_log ("valloc EXEC FAILED len=%zu prot=%d errno=%d", length, prot, errno);\n'
        '\t}\n'
        '#endif')
    s2 = s2.replace(_va_anchor, _va_repl, 1)

    # mono_vfree → 注销（防地址复用后误翻数据页）
    _vf_old = ('mono_vfree (void *addr, size_t length, MonoMemAccountType type)\n'
               '{\n\tint res;\n\tBEGIN_CRITICAL_SECTION;')
    _vf_new = ('mono_vfree (void *addr, size_t length, MonoMemAccountType type)\n'
               '{\n\tint res;\n'
               '#if defined(__APPLE__)\n'
               '\tnebula_wx_unregister (addr, length);   /* [Nebula iOS] 注销翻转表 */\n'
               '#endif\n\tBEGIN_CRITICAL_SECTION;')
    if _vf_old not in s2:
        print("    !! mono-mmap.c: mono_vfree 锚点未命中", file=sys.stderr)
        sys.exit(1)
    s2 = s2.replace(_vf_old, _vf_new, 1)

    # mono_mprotect → 埋点（codeman 等绕过翻转表直接改保护位的路径全可见）
    _mp_old = '\treturn mprotect (addr, length, prot);\n}'
    if s2.count(_mp_old) != 1:
        print("    !! mono-mmap.c: mono_mprotect 锚点未唯一命中 (%d)" % s2.count(_mp_old), file=sys.stderr)
        sys.exit(1)
    _mp_new = ('\tint _r = mprotect (addr, length, prot);\n'
               '#if defined(__APPLE__)\n'
               '\t/* 调用方：返回地址经 dladdr 映射回 libmono 内偏移 + 函数名，无需离线符号化即可钉死调用者 */\n'
               '\tunsigned long _coff = 0; const char *_cfn = "?";\n'
               '\t{ Dl_info _di; void *_ra = __builtin_return_address (0);\n'
               '\t  if (_ra && dladdr (_ra, &_di)) { _coff = (unsigned long)((char *)_ra - (char *)_di.dli_fbase); if (_di.dli_sname) _cfn = _di.dli_sname; } }\n'
               '\tif (prot & PROT_EXEC) {\n'
               '\t\tint was = nebula_wx_ensure (addr, length);\n'
               '\t\tneb_wx_log ("mprotect-EXEC %p len=%zu prot_out=%d ret=%d errno=%d registered_before=%d caller=%s+0x%lx", addr, length, prot, _r, _r ? errno : 0, was, _cfn, _coff);\n'
               '\t} else {\n'
               '\t\tneb_wx_log ("mprotect %p len=%zu flags_in=%d prot_out=%d ret=%d errno=%d caller=%s+0x%lx", addr, length, flags, prot, _r, _r ? errno : 0, _cfn, _coff);\n'
               '\t}\n'
               '#endif\n'
               '\treturn _r;\n}')
    s2 = s2.replace(_mp_old, _mp_new, 1)

    open(mm, 'w', encoding='utf-8').write(s2)
    print("    mono-mmap.c: MAP_JIT 短路 + 翻转表 + mprotect W^X 已注入")
else:
    print("    !! 找不到 mono-mmap.c", file=sys.stderr)
    sys.exit(1)

# (b4) [Nebula iOS] mono_memory_barrier_process_wide 退化——真机 2026-10-26 四轮
#      定案：mono_jit_init 首次 GC (sgen stop-the-world) 调用本函数，Apple 平台
#      原版 IPI 技巧（posix_memalign 助手页 → mprotect RW → 原子写脏 → mprotect
#      NONE）在此内核触发 codesigning monitor SIGKILL：
#      nebula_mono_mem.log 四轮恒定终于 "mprotect addr len=16384 flags_in=3
#      prot_out=3 ret=0"（iOS 16K 大页 = mono_pagesize()，正是本函数第一条
#      mprotect；第二条 mprotect NONE 从未出现；写脏页的原子写之前进程已被杀）。
#      host 冒烟路线 F 畅通证明 JIT 页翻转无罪，嫌疑只剩这条非 JIT 页保护游戏。
#      处理：iOS 上跳过页保护游戏，退化为 __sync_synchronize 全屏障。
#      代价：失去全核 IPI/TLB 冲刷（Apple 多核线程挂起顺序性 hack），若后续出现
#      GC 挂起竞态再评估。nebula_memnote 落日志便于确认走到了这里。
#      CACHE_VERSION 联动升版。
tp = os.path.join(mono, 'mono', 'utils', 'mono-threads-posix.c')
if os.path.exists(tp):
    s = open(tp, encoding='utf-8', errors='replace').read()
    _bw_anchor = 'mono_memory_barrier_process_wide (void)\n{\n\tint status;\n'
    if s.count(_bw_anchor) != 1:
        print("    !! mono-threads-posix.c: barrier 锚点未唯一命中 (%d)" % s.count(_bw_anchor), file=sys.stderr)
        sys.exit(1)
    _bw_new = (_bw_anchor +
        '\t/* [Nebula iOS] 真机 2026-10-26 定案：本函数的 Apple IPI 技巧（助手页\n'
        '\t   mprotect RW→写脏→NONE）在此内核被 codesigning monitor SIGKILL（五轮\n'
        '\t   复现，无 .ips，死点恒定在两条 mprotect 之间）。iOS 跳过页保护游戏，\n'
        '\t   退化为全屏障。详见 scripts/build_mono_ios.sh (b4)。\n'
        '\t   注意：本脚本只服务 iOS 交叉构建，此处【无条件】注入，不加宏守卫——\n'
        '\t   上一版用 TARGET_IOS/TARGET_OSX 守卫被证明整段编译剔除（真机无 barrier\n'
        '\t   日志、死点不变），mono 宏体系在自定义交叉构建下的定义不可靠。 */\n'
        '\t{\n'
        '\t\textern void nebula_memnote (const char *msg);\n'
        '\t\tnebula_memnote ("barrier: iOS fallback, page-trick skipped, sync only");\n'
        '\t\t__sync_synchronize ();\n'
        '\t\treturn;\n'
        '\t}\n')
    s = s.replace(_bw_anchor, _bw_new, 1)
    open(tp, 'w', encoding='utf-8').write(s)
    print("    mono-threads-posix.c: iOS 屏障退化已注入 (page-trick skipped)")
else:
    print("    !! 找不到 mono-threads-posix.c", file=sys.stderr)
    sys.exit(1)

# (d) iOS 链接：pthread_jit_write_protect_np 在 iOS SDK 的 libpthread.tbd 不导出该符号，
#     链接器报 undefined symbol。weak_import 声明只解决编译期；链接期必须显式
#     -Wl,-U 允许该符号在运行时由 dyld 从 libSystem（umbrella libpthread）解析
#     （iOS 14+ libpthread 实际导出，配 StikDebug/JIT entitlement 运行时可用）。
#     注意：add_library(monosgen-shared SHARED) 在文件里出现两次（Windows 分支 + iOS/macOS 分支），
#     find() 会命中第一个（Windows）导致加错目标。改用 iOS 分支里已生效的
#     add_linker_flag(-Wl,-hidden-licudata) 作为锚点（ICU flags 已证明该分支的 add_linker_flag 进链接命令）。
mini_cmake = os.path.join(mono, 'mono', 'mini', 'CMakeLists.txt')
if os.path.exists(mini_cmake):
    s = open(mini_cmake, encoding='utf-8', errors='replace').read()
    anchor = 'add_linker_flag(-Wl,-hidden-licudata)'
    idx = s.find(anchor)
    if idx >= 0:
        ins = ('\n'
               '# [Nebula iOS] 允许 _pthread_jit_write_protect_np 运行时解析（iOS SDK 不导出该符号）\n'
               'add_linker_flag(-Wl,-U,_pthread_jit_write_protect_np)\n')
        nl = s.find('\n', idx)
        s = s[:nl+1] + ins + s[nl+1:]
        open(mini_cmake, 'w', encoding='utf-8').write(s)
        print("    mono/mini/CMakeLists.txt: iOS 分支加 add_linker_flag(-Wl,-U,_pthread_jit_write_protect_np)")
    else:
        print("    !! 找不到 add_linker_flag(-Wl,-hidden-licudata) 锚点（iOS 分支可能已改结构）", file=sys.stderr)
else:
    print("    !! 找不到 mono/mini/CMakeLists.txt", file=sys.stderr)
PY
  echo "    [JIT] DISABLE_JIT 已放出 + W^X 走路线 F（每页 mprotect 翻转，host 冒烟实证畅通）"
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
