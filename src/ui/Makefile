# src/ui/Makefile —— 面板数据层的本地自测（ObjC 界面只能在真机跑，
# 但读写的数据全是 C，可以在这里验证）
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
LDLIBS  ?= -ldl -lpthread

.PHONY: test clean

test: test_roles_ui
	./test_roles_ui

test_roles_ui: test_roles_ui.c roles_ui.c \
               ../core/roleweight.c ../core/freeplay.c ../core/roles.c ../core/rolecfg.c \
               ../core/rolemapping.c ../core/core.c ../il2cpp/bridge.c ../il2cpp/hook.c
	$(CC) $(CFLAGS) -o $@ test_roles_ui.c roles_ui.c \
	      ../core/roleweight.c ../core/freeplay.c ../core/roles.c ../core/rolecfg.c \
	      ../core/rolemapping.c ../core/core.c ../il2cpp/bridge.c ../il2cpp/hook.c $(LDLIBS)
	mkdir -p /tmp/nebula_ui_home

clean:
	rm -f test_roles_ui
