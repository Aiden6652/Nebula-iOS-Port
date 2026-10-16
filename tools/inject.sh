#!/usr/bin/env bash
#
# inject.sh — 注入 + 重签 示意脚本（签名步骤由用户自行处理）
#
# 前置：
#   - 脱壳后的 Among Us IPA
#   - 已编译的 nebula_ios_loader.dylib（见 src/dylib/Makefile）
#   - insert_dylib / optool（注入 LC_LOAD_DYLIB）
#   - zsign / 全能签 / Sideloadly（重签）
#
set -e

IPA="Among Us.decrypted.ipa"
DYLIB="src/dylib/nebula_ios_loader.dylib"
WORK="build/payload"

# 1. 解包
rm -rf "$WORK" && mkdir -p "$WORK"
unzip -q "$IPA" -d "$WORK"

# 2. 放入 dylib（以 @executable_path/Frameworks/ 为例）
APP=$(find "$WORK/Payload" -name '*.app' | head -1)
mkdir -p "$APP/Frameworks"
cp "$DYLIB" "$APP/Frameworks/"

# 3. 注入 LC_LOAD_DYLIB
#    insert_dylib 方式：
# insert_dylib --no-strip-codesig \
#   "@executable_path/Frameworks/nebula_ios_loader.dylib" \
#   "$APP/Among Us" "$APP/Among Us"
#
#    或 optool 方式：
# optool install -c "@executable_path/Frameworks/nebula_ios_loader.dylib" \
#   -t "$APP/Among Us"

# 4. 重签（用户自行处理；示例用 zsign）
# zsign -k your.p12 -p password -m embedded.mobileprovision -o signed.ipa "$WORK"

echo "[*] 注入步骤示意完成；请用你的签名工具重签后安装到 iPad。"
