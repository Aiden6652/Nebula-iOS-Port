#!/bin/bash
# 构建 Nebula Mono 嵌入宿主。
# 桌面（验证用）：用系统 Mono 编宿主 + dotnet 编 Engine.dll(含 Roslyn)。
# iOS：交叉编译见下方“iOS 交叉编译”段落（需 macOS + Xcode + ios Mono）。
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT"

echo "[build] 1/2 编译托管侧 Engine.dll (+Roslyn) ..."
dotnet publish "$ROOT/Engine.csproj" -c Release -o "$OUT" >/dev/null
cp "$ROOT/script.cs.txt" "$OUT/"

echo "[build] 2/2 编译原生宿主 ..."
if pkg-config --exists mono-2 2>/dev/null; then
    # 注意：-lstdc++ 必须加，libmono-2.0 含 C++ 代码（EXE 链接需要解析 __cxa_* 等符号）。
    cc "$ROOT/nebula_mono_host.c" -o "$OUT/nebula_mono_host" \
       $(pkg-config --cflags --libs mono-2) -lstdc++ -lpthread
    cc -shared -fPIC "$ROOT/nebula_mono_host.c" -o "$OUT/libnebula_mono_host.so" \
       $(pkg-config --cflags --libs mono-2) -lstdc++ -lpthread
    echo "[build] OK (desktop Mono)"
else
    echo "[build] 未检测到桌面 Mono（pkg-config mono-2 不存在）。"
    echo "        仅生成了托管侧；宿主需在装有 Mono 的环境或 iOS 交叉工具链下编译。"
    echo "        见下方 iOS 交叉编译说明。"
fi

echo
echo "================ iOS 交叉编译（在 macOS 上执行）================"
echo "1) 交叉编译 Mono for ios-arm64 + JIT："
echo "   # 用 Mono 官方 build 脚本，目标 ios，开启 JIT（默认 ios 为 AOT-only，需显式允许 JIT）"
echo "   ./configure --host=arm64-apple-ios --enable-minimal=jit?no \\"
echo "       --disable-mcs --with-mono-arch=arm64 --enable-llvm=no"
echo "   make; make install DESTDIR=\$MONO_IOS"
echo "   要点：构建出的 libmonosgen-2.0 必须带 JIT（不是纯 AOT）；"
echo "        真机靠 entitlements 拿到 MAP_JIT（你已通过 SideStore/LiveContainer 获得 JIT 权限）。"
echo
echo "2) 编译宿主为 iOS dylib："
echo "   cc -target arm64-apple-ios -shared -fPIC nebula_mono_host.c \\"
echo "      -I\$MONO_IOS/include/mono-2.0 -L\$MONO_IOS/lib -lmonosgen-2.0 -lpthread \\"
echo "      -o libnebula_mono_host.dylib"
echo
echo "3) entitlements（注入 AU 的 dylib / 或重打包的 AU 需带）："
echo "   <key>com.apple.security.cs.allow-jit</key><true/>"
echo "   <key>com.apple.security.cs.disable-library-validation</key><true/>"
echo "   （配合 MAP_JIT；SideStore/LiveContainer 已为你提供 JIT 启用途径）"
echo
echo "4) 把 out/ 下的 Engine.dll + Roslyn 依赖 + script.cs.txt + 本 dylib"
echo "   一并随 AU 部署；dylib 被加载即异步启动 Mono 并跑 Engine.Main。"
