#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 快递秤协议解析层 —— **主机侧参考实现,不参与固件构建**
 *
 * 设计决定(问题与版本优化.md 第 36 项):设备端只做字节透传(透传模式),协议解析放在
 * 手机 / 电脑 / 服务器侧;本目录不进入 main/CMakeLists.txt 的 SRCS,
 * 因此对固件 flash 零占用(实测接入时约 4.9 KB,见 问题与版本优化.md)。
 *
 * 保留价值:下面的 20 个解析器已用《快递秤通信协议汇总》里的**真实报文逐条验证**
 * (tools/proto_ref/test_data_proto.c,28 例全通过),端上可直接移植或对照。
 *
 * 解析器是**纯函数**:不依赖 ESP-IDF / cJSON / 堆,只吃字节出结构体,
 * 因此可在主机端用 gcc 直接编译与单测。
 *
 * 20 个协议归为 5 族:ASCII 定长/分隔帧、STX-ETX 带校验帧、定长二进制 BCD、
 * '=' 前缀反转顺序帧、应答式(需先发 poll 命令)。
 */

/* 解析结果(统一结构化字段) */
typedef struct {
    bool    valid;          /* 解析成功 */
    bool    stable;         /* 重量稳定 */
    bool    overload;       /* 超载 / 溢出 / 未归零 */
    bool    negative;       /* 数值为负 */
    bool    zero;           /* 重量为 0 */
    bool    tare_mode;      /* 去皮模式(仅协议 9 提供) */
    bool    is_gross;       /* true=毛重, false=净重 */
    uint8_t decimals;       /* 小数位数 0..4 */
    double  value;          /* 重量(已含符号与小数) */
    double  tare;           /* 皮重(协议 13/18 提供) */
    double  gross;          /* 毛重(协议 13/18 提供) */
    bool    checksum_ok;    /* 带校验的协议:校验是否通过 */
    char    unit[5];        /* "kg" / "g" / "lb" / "" */
} scale_value_t;

typedef struct data_proto {
    uint8_t     id;             /* 协议号(与协议汇总文档一致) */
    const char *name;           /* 短名,供日志/前端展示 */
    /* ---- 分帧提示(data_pipe 用它把字节流切成帧) ---- */
    uint16_t    fixed_len;      /* >0:定长帧 */
    const char *delimiter;      /* 非 NULL:帧以该串结尾,如 "\r\n"、"\r" */
    /* ---- 应答式协议 ---- */
    const uint8_t *poll_cmd;    /* 非 NULL:需要网关先发这些字节 */
    uint8_t        poll_len;
    /* ---- 需要用户预设小数位(协议 6/15/16 报文里不带小数信息) ---- */
    uint8_t     default_decimals;
    /* ---- 解析 ---- */
    bool (*parse)(const struct data_proto *self, const uint8_t *frame,
                  size_t len, scale_value_t *out);
} data_proto_t;

size_t              data_proto_count(void);
const data_proto_t *data_proto_by_index(size_t index);
const data_proto_t *data_proto_by_id(uint8_t id);
void                scale_value_reset(scale_value_t *v);
