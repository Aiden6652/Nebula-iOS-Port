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

# 3.5 ★重要★ 开启 iTunes/Finder 文件共享（无需越狱即可取出运行日志）
#      dylib 会把诊断日志写到 <沙盒>/Documents/nebula_log.txt，
#      打开此开关后，电脑用 Finder(macOS) 或 iTunes(Windows) 就能直接取到文件。
PLIST="$APP/Info.plist"
if [ -f "$PLIST" ]; then
  /usr/libexec/PlistBuddy -c "Add :UIFileSharingEnabled bool true" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Set :UIFileSharingEnabled true" "$PLIST" 2>/dev/null \
    || echo "[!] 未找到 PlistBuddy(macOS)，请手动给 Info.plist 加 UIFileSharingEnabled=true"
  echo "[*] 已尝试开启文件共享（便于取出 nebula_log.txt）"
else
  echo "[!] 未找到 Info.plist，请手动确认 App 路径：$(dirname "$PLIST")"
fi

#      （可选）熔断开关：不想跑 hook 时，把下面的文件放进 .app 里即可停用 hook
# touch "$APP/nebula_no_hook.txt"

# 4. 重签（用户自行处理；示例用 zsign）
# zsign -k your.p12 -p password -m embedded.mobileprovision -o signed.ipa "$WORK"

echo "[*] 注入步骤示意完成；请用你的签名工具重签后安装到 iPad。"
echo "[*] 装好后：进游戏 → 电脑上取 Documents/nebula_log.txt（或 idevicesyslog 过滤 Nebula-iOS）"
