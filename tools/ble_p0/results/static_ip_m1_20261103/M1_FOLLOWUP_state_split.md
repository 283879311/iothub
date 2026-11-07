# M1 后续修正项：配置模式与运行模式分裂（登记，未改代码）

> 来源：T3 PASS 的观察项。未批准修改；仅登记。

## 现象

T3 fallback 期间（持久化 `sta_use_static=true`，运行 netif 已切 DHCP）：

```text
STA_CONNECTED: static ip mode
static ip 192.168.1.28: no ARP answer, no conflict
```

## 根因

`main/wifi.c` 的 `WIFI_EVENT_STA_CONNECTED` 与 `IP_EVENT_STA_GOT_IP` 处理器读取
`s_config.sta_use_static`（持久化要求）判断运行模式，而不是 `cfg_state`（实际应用状态）。
救援成功进入 `fallback` 后两者不一致。

## 影响

1. DHCP 地址被日志误标为静态 IP；
2. 对 DHCP 地址执行静态 IP ARP 冲突探测（无害但语义错误；自身 MAC 比对避免误报）；
3. 日志不能准确反映运行模式（M5 状态字段之前，人工判读易混淆）。

不影响：连接、GOT_IP、MQTT、NVS 不变性、救援结果。

## 建议修正方向（待单独评审）

- 运行模式判断统一使用实际应用状态（`cfg_state` / 运行时模式），`fallback` 明确等同 DHCP；
- 静态 ARP 检测只在"实际静态 IP 应用成功"时启动；
- `STA_CONNECTED` 的 DHCP 重启分支按运行模式而非持久化配置；
- 不改 tryconnect 事件归属状态机；不改 NVS 配置语义。

## 状态

已登记；未改代码、未烧板；需独立审查后实施。
