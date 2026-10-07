# iothub

基于 ESP-IDF(v6.0,目标芯片 ESP32)的物联网控制与现场调试网关:提供 Web 管理界面、UART 串口调试台、ThingsBoard 云接入(MQTT)、GPIO 控制与 HTTP OTA,定位为"底层核心应用与控制"的薄设备端,业务逻辑由手机/电脑端承载。

## 功能

- **串口调试台**:UART 参数配置(5-8 数据位、奇偶校验、1/2 停止位、任意波特率),收发数据查看(文本/hex)、清屏、手动发送。
- **Web 管理页 `/config`**:网络、MQTT、GPIO、蓝牙、UART 配置,管理员账号修改与固件升级(Basic 认证,可在页面修改口令)。
- **mDNS**:局域网内 `http://iothub-XXXX.local`(主机名随 AP 名)直接访问,STA 模式免查设备 IP。
- **强制门户**:AP 模式下手机/电脑接入后自动弹出配置页(本地 DNS 劫持 + OS 连通性探测 302)。
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

1. 设备启动后默认 AP 模式,连接 `IotHub-XXXX`(XXXX 为 MAC 后缀),浏览器访问 `http://192.168.8.1`(自动进入配置页);
2. 登录账号 `admin / 123456` 为出厂默认(可经 `idf.py menuconfig → Iothub Configuration` 按部署批次修改),登录后在"管理员账号"面板改密,详见 `问题.md` 第 1 项;
3. 在"网络"中填写上游 WiFi 凭据(保存的密码不会回传明文,留空表示保持不变);
4. 在"MQTT"中配置 ThingsBoard 接入,设备获取 STA IP 后自动连接并上报。

## 目录结构

```
main/            固件源码(模块划分见各 .h 接口)
web/             前端页面(配置页 config.html,单页形态)
tools/           构建期脚本(JS 语法检查、HTML/JS 压缩)
docs/debug/      UART 排查记录(遗留问题见文档内状态)
问题.md           代码审查问题清单与处理记录
```

## 已知问题与安全提示

- 全站无 TLS(Basic 凭据明文传输)—— 见 `问题.md` 第 1 项遗留部分;
- 蓝牙功能为占位实现;
- UART RX 电气层问题排查记录见 `docs/debug/`。
