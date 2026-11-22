CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS ?= -Istub -I../operation_host -I../operation_host/stub -I../../main
DEFS := -DAPP_CMD_NET_JOB_TEST=1

BIN := cmd_net_job_test
SRCS := test_cmd_net_job.c job_port_shim.c \
        ../operation_host/rtos_shim.c \
        ../../main/cmd_net_job.c ../../main/operation.c ../../main/operation_state.c \
        ../../main/cmd_cfg_txn.c ../../main/cmd_context.c
HDRS := job_port_shim.h ../operation_host/rtos_shim.h \
        ../../main/cmd_net_job.h ../../main/operation.h \
        ../../main/operation_state.h ../../main/cmd_cfg_txn.h ../../main/cmd_context.h

.PHONY: all run clean
all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -o $@ $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
