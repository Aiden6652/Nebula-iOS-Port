# src/net/Makefile
# host 端：用 gcc 验证协议 + 分发层（CI 也会跑，与 iOS 构建解耦）
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra

.PHONY: test clean

test: test_protocol test_dispatch
	./test_protocol
	./test_dispatch

test_protocol: test_protocol.c protocol.c protocol.h
	$(CC) $(CFLAGS) -o $@ test_protocol.c protocol.c

test_dispatch: test_dispatch.c protocol.c dispatch.c net_layer.h protocol.h
	$(CC) $(CFLAGS) -o $@ test_dispatch.c protocol.c dispatch.c

clean:
	rm -f test_protocol test_dispatch
