CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror
CPPFLAGS ?= -I../../main
LDLIBS ?= -lm
BIN := proto_id_test
SRCS := test_cmd_v1_id.c ../../main/cmd_v1_id.c

.PHONY: all run clean
all: $(BIN)
$(BIN): $(SRCS) ../../main/cmd_v1_id.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)
run: $(BIN)
	./$(BIN)
clean:
	rm -f $(BIN)
