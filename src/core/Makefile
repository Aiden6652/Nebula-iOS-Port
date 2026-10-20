# src/core/Makefile
# host 端：gcc 验证纯本地玩法逻辑（CI 也会跑）
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
TARGET  := test_core test_rolecfg test_freeplay
SRC     := test_core.c core.c roles.c rolecfg.c rolemapping.c freeplay.c

.PHONY: test clean

testrolecfg: test_rolecfg.c roles.c rolecfg.c rolemapping.c freeplay.c core.c ../il2cpp/bridge.c ../il2cpp/hook.c
	$(CC) $(CFLAGS) -o $@ test_rolecfg.c roles.c rolecfg.c rolemapping.c freeplay.c core.c ../il2cpp/bridge.c ../il2cpp/hook.c -ldl -lpthread

testcore: test_core.c core.c
	$(CC) $(CFLAGS) -o $@ test_core.c core.c

testfreeplay: test_freeplay.c freeplay.c roles.c rolecfg.c rolemapping.c core.c ../il2cpp/bridge.c ../il2cpp/hook.c
	$(CC) $(CFLAGS) -o $@ test_freeplay.c freeplay.c roles.c rolecfg.c rolemapping.c core.c ../il2cpp/bridge.c ../il2cpp/hook.c -ldl -lpthread

test: testcore testrolecfg testfreeplay
	./testcore
	./testrolecfg
	./testfreeplay

clean:
	rm -f $(TARGET)