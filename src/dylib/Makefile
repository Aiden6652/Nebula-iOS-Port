# 交叉编译 nebula_iOS 加载器为 ARM64 iOS dylib
# 需 macOS + Xcode 命令行工具（GitHub Actions 的 macos-latest 自带）。

CC      ?= clang
TARGET  := arm64-apple-ios14.0
SYSROOT := $(shell xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)
SRC     := loader.c ../net/protocol.c ../net/dispatch.c ../core/core.c ../core/roles.c ../core/rolecfg.c ../core/rolemapping.c ../core/freeplay.c ../ui/overlay.m ../il2cpp/bridge.c ../il2cpp/hook.c ../addon/addon.c
OUT     := nebula_ios_loader.dylib
CFLAGS  := -target $(TARGET) -isysroot $(SYSROOT) -fPIC -O2 -Wall

ifeq ($(SYSROOT),)
$(error 找不到 iPhoneOS SDK，请在 macOS + Xcode 环境下编译)
endif

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -shared -o $(OUT) $(SRC) -lz

clean:
	rm -f $(OUT)

.PHONY: all clean