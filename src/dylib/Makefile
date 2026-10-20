# 交叉编译 nebula_iOS 加载器为 ARM64 iOS dylib
# 需 macOS + Xcode 命令行工具（GitHub Actions 的 macos-latest 自带）。

CC      ?= clang
TARGET  := arm64-apple-ios14.0
SYSROOT := $(shell xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)
SRC     := loader.c ../net/protocol.c ../net/dispatch.c ../core/core.c ../core/roles.c ../core/rolecfg.c ../core/rolemapping.c ../core/roleweight.c ../core/freeplay.c ../ui/roles_ui.c ../il2cpp/bridge.c ../il2cpp/hook.c ../addon/addon.c
OUT     := nebula_ios_loader.dylib
CFLAGS  := -target $(TARGET) -isysroot $(SYSROOT) -fPIC -O2 -Wall

ifeq ($(SYSROOT),)
$(error 找不到 iPhoneOS SDK，请在 macOS + Xcode 环境下编译)
endif

all: $(OUT)

# ObjC 单独编译并开 ARC：MRC 下很容易踩"autorelease 对象赋给全局变量
# → 池排空变野指针 → 一点就崩"的坑。ARC 能自动把强引用持有住。
OBJC   := ../ui/overlay.m
OBJO   := overlay_ui.o

$(OUT): $(SRC) $(OBJO)
	$(CC) $(CFLAGS) -shared -o $(OUT) $(SRC) $(OBJO) -lz -framework UIKit -framework Foundation

$(OBJO): $(OBJC)
	$(CC) $(CFLAGS) -fobjc-arc -c -o $(OBJO) $(OBJC)

clean:
	rm -f $(OUT) $(OBJO)

.PHONY: all clean