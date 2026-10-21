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

echo "=== [4/5] 构建 iOS arm64 运行时 ==="
export MONO_SOURCE_ROOT="$WORK/mono"
cd "$WORK/gmb"
python3 ./patch_mono.py
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
