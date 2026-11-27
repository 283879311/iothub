CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS ?= -Istub -I../operation_host -I../operation_host/stub -I../../main
DEFS    := -DAPP_C1_TEST_HOOKS=1 -DAPP_C15_HOOK_TARGET=1 -DAPP_C15_BUSY_DIAG=1

BIN := c15_hook_test
SRCS := test_c15_hook.c ../operation_host/rtos_shim.c \
        ../../main/operation.c ../../main/operation_state.c
HDRS := ../operation_host/rtos_shim.h ../../main/operation.h \
        ../../main/operation_state.h

.PHONY: all run clean
all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -o $@ $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
