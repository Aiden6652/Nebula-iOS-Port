# src/il2cpp/Makefile
# host 端：gcc 验证无 Frida 桥的优雅降级（CI 也会跑，与 iOS 构建解耦）
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
LDLIBS  ?= -ldl -lpthread
TARGET  := test_bridge
SRC     := test_bridge.c bridge.c hook.c ../core/roles.c ../core/rolecfg.c ../core/rolemapping.c ../core/freeplay.c ../core/core.c

# 模拟 IL2CPP 环境：本地验证"不会把游戏改坏"（尤其速度不累乘），无需真机
MOCK    := test_mock_il2cpp
MSRC    := test_mock_il2cpp.c bridge.c ../core/roles.c ../core/rolecfg.c ../core/rolemapping.c ../core/freeplay.c ../core/core.c

.PHONY: test clean

test: $(TARGET) $(MOCK)
	./$(TARGET)
	./$(MOCK)

$(TARGET): $(SRC) bridge.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

# -rdynamic：把本文件里的假 il2cpp_* 符号导出，
# 让 bridge.c 的 dlsym(RTLD_DEFAULT,...) 解析到它们（产品代码零改动）
$(MOCK): $(MSRC) bridge.h
	$(CC) $(CFLAGS) -rdynamic -o $@ $(MSRC) $(LDLIBS) -lz

clean:
	rm -f $(TARGET)