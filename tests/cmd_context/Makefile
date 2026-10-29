CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS ?= -I. -I../../main

BIN  := cmd_context_test
SRCS := test_cmd_context.c ../../main/cmd_context.c

.PHONY: all run clean
all: $(BIN)

$(BIN): $(SRCS) ../../main/cmd_context.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
