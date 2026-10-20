# src/core/Makefile
# host 端：gcc 验证纯本地玩法逻辑（CI 也会跑）
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
TARGET  := test_core test_rolecfg
SRC     := test_core.c core.c roles.c rolecfg.c rolemapping.c

.PHONY: test clean

testrolecfg: test_rolecfg.c roles.c rolecfg.c rolemapping.c core.c ../il2cpp/bridge.c ../il2cpp/hook.c
	$(CC) $(CFLAGS) -o $@ test_rolecfg.c roles.c rolecfg.c rolemapping.c core.c ../il2cpp/bridge.c ../il2cpp/hook.c -ldl -lpthread

testcore: test_core.c core.c
	$(CC) $(CFLAGS) -o $@ test_core.c core.c

test: testcore testrolecfg
	./testcore
	./testrolecfg

clean:
	rm -f $(TARGET)