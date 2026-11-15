CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS ?= -Istub -I../../main
DEFS    := -DAPP_C1_TEST_HOOKS=1

BIN := operation_window_test
SRCS := test_operation_window.c rtos_shim.c ../../main/operation.c ../../main/operation_state.c
HDRS := rtos_shim.h stub/esp_err.h stub/esp_log.h stub/esp_random.h \
        stub/esp_timer.h stub/esp_system.h stub/esp_heap_caps.h stub/sdkconfig.h \
        stub/driver/gpio.h stub/driver/uart.h \
        stub/freertos/FreeRTOS.h stub/freertos/task.h \
        stub/freertos/event_groups.h stub/freertos/semphr.h \
        ../../main/operation.h ../../main/operation_state.h

.PHONY: all run falsify clean
all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -o $@ $(SRCS)

run: all
	./$(BIN)

falsify: all
	./check_falsifiable.sh

clean:
	rm -f $(BIN)
