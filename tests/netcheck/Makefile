CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror
CPPFLAGS ?= -I../../main
BIN := netcheck_policy_test
SRCS := test_netcheck_policy.c ../../main/netcheck_policy.c

.PHONY: all run clean
all: $(BIN)
$(BIN): $(SRCS) ../../main/netcheck_policy.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS)
run: $(BIN)
	./$(BIN)
clean:
	rm -f $(BIN)
