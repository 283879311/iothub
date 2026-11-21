CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror
CPPFLAGS ?= -I../../main
LDLIBS ?= -lm

CJSON_DIR := ../../managed_components/espressif__cjson/cJSON

BIN      := proto_id_test
SRCS     := test_cmd_v1_id.c ../../main/cmd_v1_id.c

# M8 端到端序列化用例：把真的 cmd_v1_id_json.c + cJSON 拉进来，断言最终 JSON 文本
JSON_BIN  := proto_id_json_test
JSON_SRCS := test_cmd_v1_id_json.c ../../main/cmd_v1_id.c ../../main/cmd_v1_id_json.c cjson.o

.PHONY: all run clean
all: $(BIN) $(JSON_BIN)

$(BIN): $(SRCS) ../../main/cmd_v1_id.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

# 第三方 cJSON 用自己的告警口径：不套项目的 -Werror（否则第三方风格差异会挡测试）
cjson.o: $(CJSON_DIR)/cJSON.c
	$(CC) -std=c11 -O2 -w -I$(CJSON_DIR) -c -o $@ $(CJSON_DIR)/cJSON.c

$(JSON_BIN): $(JSON_SRCS) ../../main/cmd_v1_id.h ../../main/cmd_v1_id_json.h $(CJSON_DIR)/cJSON.h
	$(CC) $(CPPFLAGS) -I$(CJSON_DIR) $(CFLAGS) -o $@ $(JSON_SRCS) $(LDLIBS)

run: all
	./$(BIN)
	./$(JSON_BIN)

clean:
	rm -f $(BIN) $(JSON_BIN) cjson.o
