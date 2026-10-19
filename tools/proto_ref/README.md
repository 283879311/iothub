# proto_ref — 快递秤协议参考实现(主机侧,**不参与固件构建**)

**本目录不进固件**。设计决定见 `问题与版本优化.md` 第 36 项:设备端**只做字节透传**,协议解析
放在**手机 / 电脑 / 服务器**侧。因此本目录对固件 flash **零占用**
(实测:若接入固件,约 **4.9 KB flash / 0 B RAM**)。

保留价值:下面 20 个解析器已用《快递秤通信协议汇总》里的**真实报文逐条验证**过,
端上可直接移植或对照实现。

| 文件 | 说明 |
| --- | --- |
| `data_proto.h` / `data_proto.c` | 20 个快递秤协议解析器(纯函数,无 ESP-IDF / cJSON / 堆依赖) |
| `test_data_proto.c` | 用汇总文档里的真实报文逐条断言,28 例 |

## 运行单测

```bash
gcc -I tools/proto_ref -o /tmp/t \
    tools/proto_ref/test_data_proto.c tools/proto_ref/data_proto.c -lm && /tmp/t
# 期望:结果: 共 28 例, 失败 0
```

## 输出 schema

解析结果统一为 `scale_value_t`(见 `data_proto.h`):

```
value / unit / decimals / negative / stable / overload / zero / tare / gross / is_gross / checksum_ok
```

`data_proto_t` 还携带**分帧提示**(`fixed_len` / `delimiter` / `poll_cmd` / `default_decimals`),
端上做分帧时可直接使用。

## 移植前必读:汇总文档有 7 处自相矛盾

本实现按"**文档自身规则**"取舍,并逐条记录在 `问题与版本优化.md` 第 36 项:

1. 协议 1 负值例子 `FF -644 …` 是笔误(应为 `FF 64 …`);
2. 协议 15 **不携带符号**(`-20.00` 与 `20.00` 字节完全相同);
3. 协议 15 / 16 **不带小数位**,例子里 `2.00` 与 `20.070` 暗示不同小数位;
4. 协议 18 例子 `02 3B …` 按规则解出 **1 位小数 / -236.5**,与"净重 -2.365"不符;
5. 协议 20 校验位描述含糊(`1236+9=0x4DD` → 例子填 `'D'`);
6. 协议 4 / 12 的"符号位"在例子里用 `0` 填充;
7. 协议 7 / 9 是**应答式**(7 需先发 `'P'`;9 是 `ENQ/ACK/DC1` 握手 + BCC)。

**建议向秤厂确认后再固化移植。**
