# src/il2cpp/Makefile
# host 端：gcc 验证无 Frida 桥的优雅降级（CI 也会跑，与 iOS 构建解耦）
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
LDLIBS  ?= -ldl -lpthread
TARGET  := test_bridge
SRC     := test_bridge.c bridge.c hook.c ../core/roles.c

.PHONY: test clean

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SRC) bridge.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET)
