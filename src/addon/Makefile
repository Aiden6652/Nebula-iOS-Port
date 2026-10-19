# src/addon/Makefile — host 端验证插件系统与汉化
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
TARGET  := test_addon
SRC     := test_addon.c addon.c ../core/roles.c ../core/rolecfg.c ../core/core.c ../il2cpp/bridge.c ../il2cpp/hook.c
LDLIBS  ?= -ldl -lpthread -lz

.PHONY: test clean

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SRC) addon.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET)
