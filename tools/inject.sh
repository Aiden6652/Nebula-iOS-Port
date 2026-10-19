#!/usr/bin/env bash
#
# inject.sh — 一键：注入 dylib + 开文件共享 + 套用 entitlements + 给出重签命令
#
# 用法：
#   ./tools/inject.sh <脱壳IPA> [输出目录]
#   ./tools/inject.sh <脱壳IPA> --jit        # 同时启用 JIT entitlements
#
# 做的事：
#   1. 解包 IPA（自动识别 *.app，不写死名字 —— AmongUs.app / Among Us.app 都行）
#   2. 把 nebula dylib 放进 Frameworks 并注入 LC_LOAD_DYLIB
#   3. 给 Info.plist 加 UIFileSharingEnabled（便于电脑上取运行日志，无需越狱）
#   4. 选用 entitlements（基础版 / JIT 版），打印完整重签命令
#
set -e

IPA="${1:-}"
OUTDIR="${2:-build}"
USE_JIT=0
FLAGS=()
ARGS=("$@")
_i=0
while [ $_i -lt ${#ARGS[@]} ]; do
  case "${ARGS[$_i]}" in
    --jit)  USE_JIT=1 ;;
    --flag) _i=$((_i+1)); [ -n "${ARGS[$_i]:-}" ] && FLAGS+=("${ARGS[$_i]}") ;;
  esac
  _i=$((_i+1))
done

if [ -z "$IPA" ] || [ ! -f "$IPA" ]; then
  echo "用法: $0 <脱壳IPA> [输出目录] [--jit] [--flag <开关文件> ...]"
  echo "例:   $0 game.ipa --flag nebula_no_text.txt"
  echo "可用开关见 tools/flags/"
  exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
DYLIB="$HERE/../src/dylib/nebula_ios_loader.dylib"
WORK="$OUTDIR/payload"

echo "[*] IPA    : $IPA"
echo "[*] dylib  : $DYLIB"
[ ! -f "$DYLIB" ] && echo "[!] 找不到 dylib，请先编译（cd src/dylib && make）或下载 Actions 产物" && exit 1

# ---------- 1. 解包 ----------
rm -rf "$WORK" && mkdir -p "$WORK"
unzip -q "$IPA" -d "$WORK"

# ---------- 2. 自动识别 app（不写死名字） ----------
APP=$(find "$WORK/Payload" -maxdepth 1 -name '*.app' | head -1)
[ -z "$APP" ] && echo "[!] 未找到 Payload/*.app" && exit 1
echo "[*] app    : $(basename "$APP")"

# 主可执行文件名（从 Info.plist 的 CFBundleExecutable 取，取不到就退化为同名）
EXE=$(defaults read "$(pwd)/$APP/Info" CFBundleExecutable 2>/dev/null || true)
[ -z "$EXE" ] && EXE="$(basename "$APP" .app)"
echo "[*] 主执行文件: $EXE"
[ ! -f "$APP/$EXE" ] && echo "[!] 主执行文件不存在: $APP/$EXE" && exit 1

# ---------- 3. 放入 dylib 并注入 LC_LOAD_DYLIB ----------
mkdir -p "$APP/Frameworks"
cp "$DYLIB" "$APP/Frameworks/"
echo "[*] 已放入 Frameworks/$(basename "$DYLIB")"

if command -v insert_dylib >/dev/null 2>&1; then
  insert_dylib --no-strip-codesig \
    "@executable_path/Frameworks/$(basename "$DYLIB")" \
    "$APP/$EXE" "$APP/$EXE" && echo "[*] LC_LOAD_DYLIB 注入完成(insert_dylib)"
elif command -v optool >/dev/null 2>&1; then
  optool install -c "@executable_path/Frameworks/$(basename "$DYLIB")" -t "$APP/$EXE" \
    && echo "[*] LC_LOAD_DYLIB 注入完成(optool)"
else
  echo "[!] 未找到 insert_dylib / optool，请手动注入："
  echo "    insert_dylib --no-strip-codesig \\"
  echo "      \"@executable_path/Frameworks/$(basename "$DYLIB")\" \"$APP/$EXE\" \"$APP/$EXE\""
fi

# ---------- 4. 开文件共享（取日志用，无需越狱） ----------
PLIST="$APP/Info.plist"
if [ -f "$PLIST" ]; then
  /usr/libexec/PlistBuddy -c "Add :UIFileSharingEnabled bool true" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Set :UIFileSharingEnabled true" "$PLIST" 2>/dev/null \
    || echo "[!] 无 PlistBuddy(macOS)，请手动给 Info.plist 加 UIFileSharingEnabled=true"
  echo "[*] 已尝试开启文件共享（电脑上可取 Documents/nebula_log.txt）"
fi

# ---------- 4.5 开关文件（--flag 指定，可多次） ----------
# 可用开关见 tools/flags/：
#   nebula_hook.txt      启用每帧 hook（默认关闭）
#   nebula_speed.txt     启用按职业调速（默认关闭）
#   nebula_no_name.txt   不写玩家名字
#   nebula_no_text.txt   不做 UI 文本替换
#   nebula_no_bridge.txt 完全不启动游戏桥
if [ ${#FLAGS[@]} -gt 0 ]; then
  for f in "${FLAGS[@]}"; do
    SRC="$HERE/flags/$f"
    if [ -f "$SRC" ]; then
      cp "$SRC" "$APP/$f" && echo "[*] 已放入开关: $f"
    else
      echo "[!] 未知开关: $f（可选项见 tools/flags/）"
    fi
  done
else
  echo "[*] 未指定开关，使用默认（不装 hook、不改速度、只显示职业名）"
fi

# ---------- 5. entitlements ----------
if [ "$USE_JIT" = "1" ]; then
  ENT="$HERE/entitlements-jit.plist"; TAG="JIT 版"
else
  ENT="$HERE/entitlements.plist";     TAG="基础版"
fi
echo "[*] entitlements: $TAG ($ENT)"

echo
echo "=============== 接下来用它重签（复制整段） ==============="
echo "# zsign（命令行）:"
echo "zsign -k your.p12 -p '密码' -m embedded.mobileprovision \\"
echo "      -e \"$ENT\" \\"
echo "      -o signed.ipa \"$WORK\""
echo
echo "# Sideloadly（图形界面）: 在 Advanced → Entitlements 选择:"
echo "    $ENT"
echo "=========================================================="
echo
echo "[*] 装好后：进一局游戏 → 电脑取 Documents/nebula_log.txt"
echo "    （或 idevicesyslog | grep Nebula-iOS）把内容发我，我据此继续修。"
