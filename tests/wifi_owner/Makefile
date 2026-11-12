CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror
CPPFLAGS ?= -I. -I../../main

BIN  := wifi_owner_test
SRCS := test_wifi_owner.c ../../main/wifi_op_owner.c

.PHONY: all run clean
all: $(BIN)

$(BIN): $(SRCS) ../../main/wifi_op_owner.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS)

run: all
	./$(BIN)

clean:
	rm -f $(BIN)
