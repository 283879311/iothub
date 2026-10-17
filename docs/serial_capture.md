# 串口抓包与 ESP32 自动复位(2026-10-17,1.3-60c)

## 现象

用 `tools/capture_serial.py` 做**短时**抓包(几秒)时,板子反复出现:

```
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
```

表现为:每次抓包板子重启一次 → Web 会话失效(`auth_required`)、BLE 链路断开、`data_pipe`
计数清零。双板 E2E 中一度误判为固件崩溃,实际是抓包工具把板子复位了。

## 根因

ESP32 开发板的 USB 串口 DTR/RTS 经三极管接到 EN/GPIO0:

- 打开串口时,驱动/`pyserial` 会按初始状态设置 DTR/RTS,这两根线的一次跳变就会在 EN 上
  产生复位脉冲;
- 关闭串口时,termios 的 `HUPCL` 会让驱动拉低 DTR,同样可能产生复位脉冲;
- 因此**短抓包 = 反复 open/close = 反复复位**。抓包期间越长(一路保持打开)越安全。

## 已做的修复

`tools/capture_serial.py`:

1. open 前显式 `dtr=False/rts=False`(原有行为);
2. open 后清 `termios.HUPCL`,避免 close 时拉低 DTR;
3. 退出前再次保持 `dtr=False/rts=False`。

## 使用规范(重要)

- **测试期间保持一路长抓包**(整段测量只 open 一次),不要每隔几秒启动/停止抓包;
- 需要换固件/烧录前,先停掉抓包进程(烧录本身会复位,属预期);
- 抓包 open 仍可能触发**一次**复位(取决于板子的复位电路),把它当作"本次上电"的起点:
  等待启动完成、WiFi/MQTT/BLE 连接建立后再开始采样;
- 只取状态/计数时优先走 HTTP(`/api/v1/device/status`),不要为了看一眼计数去开串口;
- 双板测试时,两块板各保持一路长抓包,避免交叉 open/close。

## 验证

修复后 close 不再产生复位;`1.3版本优化.md` 1.3-60c 有本次双板 E2E 的完整记录。
