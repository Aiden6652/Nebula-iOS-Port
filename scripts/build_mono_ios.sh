#!/usr/bin/env bash
# 交叉编译 Mono 运行时 → iOS arm64 (device)
#
# 复用 godot-mono-builds：Mono 官方 sdks 脚本的成熟封装，专门给 iOS 编运行时，
# 比自己从零 configure 少踩无数坑。
#
# 与 Godot 默认 iOS 模板的唯一关键差异（第 3 步）：
#   Godot 为了上架走"纯全 AOT"，在 --enable-minimal 里把 jit 和 interpreter 都砍了。
#   而我们要跑 BepInEx + Harmony —— Harmony 是在运行时生成并执行动态 IL 补丁的，
#   必须由 JIT 或解释器来跑，所以这两个绝不能砍。
#
#   （Mono 语义提示：--enable-minimal=X 意思是"砍掉 X"，不是"启用 X"。）
set -euo pipefail

MONO_VERSION="${MONO_VERSION:-6.12.0.206}"
WORK="${WORK:-$HOME/mono-ios-build}"
INSTALL="${INSTALL:-$HOME/mono-installs}"
MANIFEST="${MANIFEST:-$HOME/mono_ios_manifest.txt}"

echo "=== [0/5] 环境 ==="
sw_vers 2>/dev/null || true
xcodebuild -version 2>/dev/null || true
echo "iPhoneOS SDK: $(xcrun --sdk iphoneos --show-sdk-path)"

echo "=== [1/5] 拉 godot-mono-builds ==="
rm -rf "$WORK/gmb"
git clone --depth 1 https://github.com/godotengine/godot-mono-builds.git "$WORK/gmb"

echo "=== [2/5] 拉 Mono 源码 (tag mono-$MONO_VERSION) ==="
rm -rf "$WORK/mono"
git clone --depth 1 --branch "mono-$MONO_VERSION" https://github.com/mono/mono.git "$WORK/mono"

echo "=== [3/5] 改 iOS 模板：保留 JIT + 解释器 ==="
python3 - "$WORK/gmb/ios.py" <<'PY'
import sys

p = sys.argv[1]
s = open(p).read()

key = "'--enable-minimal="
i = s.index(key)
j = s.index("',", i) + 2          # minimal 列表的收尾 "',"
seg = s[i:j]

new = seg.replace("interpreter,", "").replace("jit,", "")
if new == seg:
    sys.exit("!! minimal 列表里没找到 interpreter/jit —— godot-mono-builds 版本可能变了，请人工核对")

s = s[:i] + new + s[j:]
open(p, "w").write(s)
print("已保留 JIT + interpreter（原本被 Godot 的 iOS 设备模板砍掉）")
PY

echo "=== [3b/5] iOS 特有问题：aot-compiler.c 用了 system() ==="
# iOS SDK 把 system() 标成 unavailable，而 mono/mini/aot-compiler.c 会调它
# （全 AOT 时用来起外部汇编器）。我们走 JIT/解释器，AOT 编译器完全用不到，
# 所以在这个文件里把 system() 用宏桩掉 —— 只在 HOST_IOS 下生效，不影响别的平台。
python3 - "$WORK/mono" <<'PY'
import sys, os
src = sys.argv[1]
p = os.path.join(src, "mono/mini/aot-compiler.c")
lines = open(p).read().split("\n")
idx = None
for i, l in enumerate(lines[:300]):
    if l.lstrip().startswith("#include"):
        idx = i
if idx is None:
    sys.exit("!! aot-compiler.c 里没找到 #include，无法定位插入点")
guard = [
    "",
    "#if defined(HOST_IOS)",
    "/* iOS 没有 system()（SDK 标为 unavailable）。我们走 JIT/解释器，",
    "   AOT 编译器用不到，用桩顶掉，避免编译失败。 */",
    "#define system(cmd) (-1)",
    "#endif",
]
lines[idx + 1:idx + 1] = guard
open(p, "w").write("\n".join(lines))
print("已在 aot-compiler.c 注入 HOST_IOS 下的 system() 桩")
PY

echo "=== [4/5] 构建 iOS arm64 运行时 ==="
export MONO_SOURCE_ROOT="$WORK/mono"
cd "$WORK/gmb"

# 4a. 打补丁。注意：godot-mono-builds 的 patch_mono.py 只要有一个补丁对不上就整体
#     exit(1)，会把排在它后面的 iOS 关键补丁（mono_ios_asl_log_deprecated 等）
#     一起吞掉。所以先容忍它的失败，再用兜底循环把没打上的补齐。
python3 ./patch_mono.py || echo "!! patch_mono.py 有补丁未打上，交给兜底循环补齐"

# 4b. 兜底：逐个补丁容忍失败再打一遍（-N 自动跳过已应用的，--batch 不再交互卡住）
echo "--- 兜底补齐补丁 ---"
for d in "$WORK/gmb/files/patches/"*.diff; do
  echo "  [patch] $(basename "$d")"
  ( cd "$WORK/mono" && patch -N -p1 --forward --batch < "$d" ) \
    || echo "          跳过（不适用或已应用）"
done

# 4c. Mono 从 git 拉下来没有 configure，需要 autogen 生成
if [ ! -x "$WORK/mono/configure" ]; then
  echo "--- 生成 configure (autogen) ---"
  ( cd "$WORK/mono" && NOCONFIGURE=1 ./autogen.sh )
fi

./ios.py configure --target=arm64 --install-dir="$INSTALL"
./ios.py make --target=arm64 --install-dir="$INSTALL"

echo "=== [5/5] 构建 iOS BCL（跑 C# 必需的基础类库：mscorlib/System.*）==="
./bcl.py make --product=ios --install-dir="$INSTALL"

echo "=== 产物清单 ==="
{
  echo "MONO_VERSION=$MONO_VERSION"
  echo "INSTALL=$INSTALL"
  echo "--- include 目录 ---"
  find "$INSTALL" -type d -name mono -path "*include*" 2>/dev/null | head -5
  echo "--- 静态库 ---"
  find "$INSTALL" -name "*.a" 2>/dev/null | head -40
  echo "--- BCL ---"
  find "$INSTALL" -type d -name "*bcl*" 2>/dev/null | head -5
} | tee "$MANIFEST"

echo "OK: Mono iOS arm64 构建完成，manifest -> $MANIFEST"
