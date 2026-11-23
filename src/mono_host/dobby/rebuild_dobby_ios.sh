#!/bin/bash
# [R110/R111/R112] 从源码重编 Dobby 的 InstructionRelocationARM64.o + InterceptRouting.o + code-patch-tool-darwin.o，替换进 libdobby.a。
# 根治 iOS execute-only 代码页 hook 的 relo SIGSEGV（R99~R109 十一轮 shim 补丁全废）。
#
# 唯一改动（patches/InstructionRelocationARM64.cc.patched）：
#   relo_relocate 读源指令 CPU ldr  ->  vm_read(Mach IPC)
# iOS code-signed __TEXT_EXEC 页是 APRR execute-only，CPU 数据读必 SIGSEGV
# （崩溃点 ctx->buffer_cursor 的 ldr，buffer=源函数真实地址），vm_read 能读。
# R111 补漏：GenerateRelocatedCode 里 memcpy/log_hex_format 两处 CPU 读源页也改 vm_read
# （真机 R110 崩点：GenerateRelocatedCode -> memcpy 循环 ldr/str，execute-only 页）。
# R112 补漏：DobbyCodePatch remap 分支 line110 memcpy(remap_dummy, patch_page, page_size)
# 整页 CPU 读 execute-only 源页也改 vm_read（真机 R111 崩点：DobbyCodePatch -> memcpy ldp/stp）。
# 写跳板逻辑本身保持 Dobby 上游原样，只改读源。
#
# 依赖：CI(macos-15) 的 xcrun iPhoneOS SDK。沙箱无 SDK，不在这里跑。
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="$SCRIPT_DIR/patches/InstructionRelocationARM64.cc.patched"
PATCH2="$SCRIPT_DIR/patches/InterceptRouting.cpp.patched"
PATCH3="$SCRIPT_DIR/patches/code-patch-tool-darwin.cc.patched"
DOBBY_A="$SCRIPT_DIR/libdobby.a"
OUT_A="$SCRIPT_DIR/libdobby_nebula.a"

# Dobby 源码 commit —— 必须与预编译 libdobby.a 同源（归档时间戳 2023-03-29），
# 保证 ABI 一致，否则只换一个 .o 会内存布局错位。
DOBBY_REPO="https://github.com/jmpews/Dobby.git"
DOBBY_COMMIT="b0176de574104726bb68dff3b77ee666300fc338"
OBJ_NAME="InstructionRelocationARM64.cc.o"
OBJ2_NAME="InterceptRouting.cpp.o"
OBJ3_NAME="code-patch-tool-darwin.cc.o"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CXXBIN="$(xcrun --sdk iphoneos -f clang++)"
AR_BIN="ar"
command -v llvm-ar >/dev/null 2>&1 && AR_BIN="llvm-ar"

echo "[dobby-rebuild] SDK=$SDK CXX=$CXXBIN AR=$AR_BIN commit=${DOBBY_COMMIT:0:8}"
[ -f "$PATCH" ]   || { echo "!! 缺 $PATCH" >&2; exit 1; }
[ -f "$PATCH2" ]  || { echo "!! 缺 $PATCH2" >&2; exit 1; }
[ -f "$PATCH3" ]  || { echo "!! 缺 $PATCH3" >&2; exit 1; }
[ -f "$DOBBY_A" ] || { echo "!! 缺 $DOBBY_A" >&2; exit 1; }

# 1. 拉 Dobby 源码到临时目录（只取需要的 commit）
SRC="$(mktemp -d /tmp/dobby_src.XXXXXX)"
trap 'rm -rf "$SRC"' EXIT
git clone -q "$DOBBY_REPO" "$SRC"
git -C "$SRC" checkout -q "$DOBBY_COMMIT"
echo "[dobby-rebuild] 源码就绪 $(git -C "$SRC" rev-parse --short HEAD)"

# 2. 覆盖改动后的 relo 源文件
cp -f "$PATCH" "$SRC/source/InstructionRelocation/arm64/InstructionRelocationARM64.cc"
cp -f "$PATCH2" "$SRC/source/InterceptRouting/InterceptRouting.cpp"
cp -f "$PATCH3" "$SRC/source/Backend/UserMode/ExecMemory/code-patch-tool-darwin.cc"

# 3. include 路径（对齐 b0176de 的 CMakeLists include_directories）
# 注意：不要加 -I$SRC/external/TINYSTL —— 否则 platform_features.h 的
# #include <string.h> 会撞到 TINYSTL/string.h（无 size_t）。TINYSTL/vector.h
# 由 -I$SRC/external 提供即可（源码写的是 <TINYSTL/vector.h>）。
INC=("$SRC" "$SRC/include" "$SRC/source" "$SRC/source/dobby"
     "$SRC/external" "$SRC/external/logging" "$SRC/builtin-plugin"
     "$SRC/source/Backend/UserMode"
     "$SRC/source/InstructionRelocation/arm64" "$SRC/source/InterceptRouting"
     "$SRC/source/Backend/UserMode/ExecMemory")
INCS=""
for d in "${INC[@]}"; do INCS="$INCS -I$d"; done

# 4. 编译参数（对齐 Dobby cmake/compiler_and_linker.cmake Release）
CFLAGS="-arch arm64 -isysroot $SDK -std=c++17 -O3 \
 -fvisibility=hidden -fPIC -fno-stack-check -fno-stack-protector \
 -fno-exceptions -fno-rtti -fno-common -fno-zero-initialized-in-bss \
 -DBUILD_WITH_TRAMPOLINE_ASM -DDOBBY_LOGGING_DISABLE"

cd "$SRC"
echo "[dobby-rebuild] 编译 InstructionRelocationARM64.cc ..."
$CXXBIN $CFLAGS $INCS -c source/InstructionRelocation/arm64/InstructionRelocationARM64.cc \
  -o "$OBJ_NAME"

# 4b. 编译 patched InterceptRouting.cpp（GenerateRelocatedCode 的 vm_read 读源）
echo "[dobby-rebuild] 编译 InterceptRouting.cpp ..."
$CXXBIN $CFLAGS $INCS -c source/InterceptRouting/InterceptRouting.cpp \
  -o "$OBJ2_NAME"

# 4c. 编译 patched code-patch-tool-darwin.cc（DobbyCodePatch 的 vm_read 读源）
echo "[dobby-rebuild] 编译 code-patch-tool-darwin.cc ..."
$CXXBIN $CFLAGS $INCS -c source/Backend/UserMode/ExecMemory/code-patch-tool-darwin.cc \
  -o "$OBJ3_NAME"

# 5. 校验新 .o：必须导出 relo_relocate 符号
if ! nm -g "$OBJ_NAME" 2>/dev/null | grep -q 'relo_relocate'; then
  echo "!! [dobby-rebuild] 新 .o 缺 relo_relocate 符号" >&2; exit 1
fi
echo "[dobby-rebuild] 新 .o 符号校验通过"
[ -s "$OBJ2_NAME" ] || { echo "!! [dobby-rebuild] $OBJ2_NAME 未生成" >&2; exit 1; }
echo "[dobby-rebuild] $OBJ2_NAME 校验通过"
[ -s "$OBJ3_NAME" ] || { echo "!! [dobby-rebuild] $OBJ3_NAME 未生成" >&2; exit 1; }
echo "[dobby-rebuild] $OBJ3_NAME 校验通过"

# 6. 替换进归档副本（相对文件名确保同名替换而非追加）
BEFORE=$("$AR_BIN" t "$DOBBY_A" 2>/dev/null | wc -l | tr -d ' ')
cp -f "$DOBBY_A" "$OUT_A"
( cd "$SRC" && "$AR_BIN" r "$OUT_A" "$OBJ_NAME" "$OBJ2_NAME" "$OBJ3_NAME" )
AFTER=$("$AR_BIN" t "$OUT_A" 2>/dev/null | wc -l | tr -d ' ')
echo "[dobby-rebuild] 成员数 $BEFORE -> $AFTER"
if [ "$AFTER" != "$BEFORE" ]; then
  echo "!! [dobby-rebuild] 成员数变化，ar 未按同名替换，中止" >&2; exit 1
fi
N=$("$AR_BIN" t "$OUT_A" 2>/dev/null | grep -cx "$OBJ_NAME")
[ "$N" = "1" ] || { echo "!! [dobby-rebuild] $OBJ_NAME 出现 $N 次" >&2; exit 1; }
N2=$("$AR_BIN" t "$OUT_A" 2>/dev/null | grep -cx "$OBJ2_NAME")
[ "$N2" = "1" ] || { echo "!! [dobby-rebuild] $OBJ2_NAME 出现 $N2 次" >&2; exit 1; }
N3=$("$AR_BIN" t "$OUT_A" 2>/dev/null | grep -cx "$OBJ3_NAME")
[ "$N3" = "1" ] || { echo "!! [dobby-rebuild] $OBJ3_NAME 出现 $N3 次" >&2; exit 1; }

# 7. 原子替换
mv -f "$OUT_A" "$DOBBY_A"
echo "[dobby-rebuild] ✓ libdobby.a 已换成 relo+origin+codepatch 全 vm_read 读源版（$(wc -c < "$DOBBY_A") bytes）"
