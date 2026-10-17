# src/core/Makefile
# host 端：gcc 验证纯本地玩法逻辑（CI 也会跑）
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
TARGET  := test_core
SRC     := test_core.c core.c

.PHONY: test clean

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SRC) core.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f $(TARGET)
