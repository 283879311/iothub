CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS ?= -Istub -I../operation_host -I../operation_host/stub -I../../main
DEFS := -DAPP_CMD_NET_JOB_TEST=1 -DAPP_C15_BUSY_DIAG=1

BIN := cmd_net_job_test
BUILD := build
STUB_HDRS := $(wildcard stub/*.h)
GEN_HDRS := $(patsubst stub/%,$(BUILD)/%,$(STUB_HDRS))
GEN_SRC := $(BUILD)/cmd_net_job.c
SRCS := test_cmd_net_job.c job_port_shim.c \
        ../operation_host/rtos_shim.c \
        $(GEN_SRC) ../../main/operation.c ../../main/operation_state.c \
        ../../main/cmd_cfg_txn.c ../../main/cmd_context.c
HDRS := job_port_shim.h ../operation_host/rtos_shim.h \
        ../../main/cmd_net_job.h ../../main/operation.h \
        ../../main/operation_state.h ../../main/cmd_cfg_txn.h ../../main/cmd_context.h

.PHONY: all run clean
all: $(BIN)

$(BUILD):
	mkdir -p $@

# 生产源码用 "common.h"/"wifi.h" 等引号相对包含；直接编译 ../../main
# 会先命中生产头文件。复制到 build/ 后，build/ 下的空桩优先命中。
$(GEN_SRC): ../../main/cmd_net_job.c | $(BUILD)
	cp $< $@

$(BUILD)/%.h: stub/%.h | $(BUILD)
	cp $< $@

$(BIN): $(SRCS) $(GEN_HDRS) $(HDRS)
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -o $@ $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD) $(BIN)
