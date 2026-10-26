CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS ?= -I. -Istubs -I../../main

BIN  := token_store_test
SRCS := test_token_store.c host_stubs.c ../../main/auth_token.c

.PHONY: all run audit clean
all: $(BIN)

$(BIN): $(SRCS) ../../main/auth.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS)

run: $(BIN)
	./$(BIN)

# 临界区静态审计:不依赖编译器,任何机器都能跑
audit:
	python3 audit_critical_sections.py

clean:
	rm -f $(BIN)
