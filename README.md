# iothub

基于 ESP-IDF(v6.0,目标芯片 ESP32)的物联网网关与现场调试工具,面向地磅/汽车衡(称重)行业的联调与演示场景:通过 UART 向外部称重显示仪表发送模拟的车辆过磅重量数据(DS10 二进制帧或文本协议),并提供 Web 管理界面、ThingsBoard 云接入与 OTA 升级。

## 功能

- **称重数据模拟器**:生成"车辆上磅"完整重量曲线(前轴→爬升→振荡→稳定→离磅),支持 DS10 / 文本协议、目标重量、车速、帧间隔参数;也可手动单发或周期(keepalive)发送。
- **串口调试台**:UART 参数配置(5-8 数据位、奇偶校验、1/2 停止位、任意波特率),收发数据查看(文本/hex)、清屏。
- **Web 管理面**:
  - 操作页 `/`:称重发送操作台(手动 / keepalive / 设备端仿真);
  - 配置页 `/config`:网络、MQTT、GPIO、蓝牙、UART 配置与固件升级(Basic 认证)。
- **网络**:AP(192.168.8.1)/ STA / APSTA;最多 5 条 STA profile,RSSI 择优自动重连;纯 STA 启动 90s 失败自动降级 AP 并持久化。
- **MQTT(ThingsBoard)**:10s 周期遥测(继电器/LED/输入/堆内存)+ attributes 上报;下行 RPC `setRelay/setLed/getStatus/setState` 控 GPIO;支持 tb_cloud / tb_selfhost / 自定义 topic,TLS(证书 bundle)与 LWT。
- **OTA**:Web 上传固件(`iothub.bin`),带应用描述区校验(魔数/项目名/版本降级拒绝);网页资源 `storage.bin` 支持运行时整分区替换;升级后经健康确认窗口(业务可达才 mark valid,超时自动回滚)。
- **恢复出厂**:BOOT 键(GPIO0)长按 5s。
- **蓝牙**:BLE 广播 / SPP server(当前为占位,无数据面,见 `问题.md` 第 8 项)。

## 硬件引脚(编译期固定,`main/constants.h`)

| 功能 | GPIO |
| --- | --- |
| 继电器 | 16 |
| LED | 27 |
| 输入检测 | 33 |
| UART1 TX / RX | 17 / 18 |
| BOOT 键(恢复出厂) | 0 |

## 构建与烧录

```bash
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

Web 前端(`web/*.html`)在构建期自动做语法检查并压缩,打包为 LittleFS 镜像随固件烧录,无需手动处理。

## 分区表(`partitions.csv`)

nvs 24K / otadata / phy / **ota_0 + ota_1 各 1.875MB(双 OTA)** / storage(littlefs,网页资源)128KB。

## 首次使用

1. 设备启动后默认 AP 模式,连接 `IotHub-XXXX`(XXXX 为 MAC 后缀),浏览器访问 `http://192.168.8.1`;
2. 操作页可直接驱动发送;配置页登录账号 `admin / 123456`(硬编码于 `main/constants.h`,量产前务必修改,详见 `问题.md` 第 1 项);
3. 在配置页"网络"中填写上游 WiFi 凭据(保存的密码不会回传明文,留空表示保持不变);
4. 在"MQTT"中配置 ThingsBoard 接入,设备获取 STA IP 后自动连接并上报。

## 目录结构

```
main/            固件源码(模块划分见各 .h 接口)
web/             前端页面(操作页 index.html / 配置页 config.html)
tools/           构建期脚本(JS 语法检查、HTML/JS 压缩)
docs/debug/      UART 排查记录(遗留问题见文档内状态)
问题.md           代码审查问题清单与处理记录
```

## 已知问题与安全提示

- Web 凭据硬编码、部分 UART/sim 写接口未认证、全站无 TLS —— 安全项见 `问题.md` P0 清单;
- 蓝牙功能为占位实现;
- UART RX 电气层问题排查记录见 `docs/debug/`。
