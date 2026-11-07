# T3 阶段 A 补充证据：坐标、CRC 字节、偏移来源（只读）

> 本轮只读；未生成故障镜像、未写 NVS、未重启进入 T3。

## 1. 坐标体系澄清

此前表格中的偏移均为 **payload 相对偏移**（payload 起始=分区绝对 `0x160`）。绝对/相对对应关系：

| 字段 | payload 相对 | 分区绝对 | 说明 |
| --- | --- | --- | --- |
| BLOB payload 起点 | 0x000 | 0x160 | 640 字节，连续存放 |
| `sta_use_static` | 0x21F | 0x37F | 当前 `0x01` |
| `sta_static_ip[16]` | 0x220 | 0x380 | 当前 `192.168.1.50\0` + 2 字节残留 + `\0` |
| `sta_static_gateway[16]` | 0x230 | 0x390 | |
| `sta_static_netmask[16]` | 0x240 | 0x3A0 | |
| `sta_static_dns1[16]` | 0x250 | 0x3B0 | |
| `sta_static_dns2[16]` | 0x260 | 0x3C0 | |
| blob `dataCrc32` | — | 0x15C..0x15F | entry 8 data 内 |
| entry 8 `crc32` | — | 0x144..0x147 | |

拟清零范围：**绝对 0x380..0x38F（相对 0x220..0x22F）**，16 字节。

## 2. 偏移来源（不是字符串搜索推断）

用固件 ELF 的 DWARF 直接读取 `app_config_t` 布局：

```text
$ xtensa-esp32-elf-gdb -batch -ex 'ptype /o app_config_t' build/iothub.elf
/*      0      |       4 */    uint32_t magic;
/*      4      |       2 */    uint16_t version;
/*    542      |       1 */    uint8_t bt_role;
/*    543      |       1 */    uint8_t sta_use_static;
/*    544      |      16 */    char sta_static_ip[16];
/*    560      |      16 */    char sta_static_gateway[16];
/*    576      |      16 */    char sta_static_netmask[16];
/*    592      |      16 */    char sta_static_dns1[16];
/*    608      |      16 */    char sta_static_dns2[16];
                               /* total size (bytes):  640 */
```

543=0x21F、544=0x220……与 BLOB 内字符串位置逐项吻合；total 640 与 BLOB `dataSize` 一致。

## 3. CRC 原始字节与预期新值

| 项 | 原始字节（LE） | 原始值 | 修改后预期字节（LE） | 预期值 |
| --- | --- | --- | --- | --- |
| blob `dataCrc32`（0x15C） | `99 C9 19 06` | `0x0619C999` | `22 D9 4C F4` | `0xF44CD922` |
| entry 8 `crc32`（0x144） | `13 23 E2 ED` | `0xEDE22313` | `D9 73 C8 AF` | `0xAFC873D9` |

`sta_use_static`（0x37F）保持 `0x01` 不变。

## 4. 变更范围 vs 实际差异

- **允许变化字段区域（24 字节）**：`0x144..0x147`（entry CRC）、`0x15C..0x15F`（blob CRC）、`0x380..0x38F`（ip[16]）。
- **实际变化 22 字节**（`ip` 内 12 字符 + 2 字节残留 `6f 40` 被清零 + 两处 CRC 4+4）：

```text
abs 0x144..0x147  before=1323e2ed  after=d973c8af
abs 0x15c..0x15f  before=99c91906  after=22d94cf4
abs 0x380..0x38b  before=3139322e3136382e312e3530  after=000000000000000000000000
abs 0x38d..0x38e  before=6f40  after=0000
```

`0x38C` 与 `0x38F` 原本即 `00`，不变。所有差异均落在允许区域内（脚本断言 True）。

## 5. CRC 计算代码（与备份反推一致）

```python
import zlib
orig = bytearray(open('nvs_backup_1.bin','rb').read())
mod = bytearray(orig)
ABS = 0x160 + 0x220            # 0x380
for i in range(ABS, ABS+16):
    mod[i] = 0
payload = bytes(mod[0x160:0x160+640])
blob_crc = zlib.crc32(payload, 0xFFFFFFFF) & 0xFFFFFFFF
mod[0x15C:0x160] = blob_crc.to_bytes(4, 'little')
item = bytes(mod[0x140:0x160])
entry_crc = zlib.crc32(item[0:4] + item[8:32], 0xFFFFFFFF) & 0xFFFFFFFF
mod[0x144:0x148] = entry_crc.to_bytes(4, 'little')
```

反推校验：原镜像上同一算法得到 `0x0619C999` / `0xEDE22313`，与备份一致（见 `CRC_NOTES.md`）。

## 6. 写入语义（待批准的阶段 B）

- NVS 页 = 4 KiB 扇区：page0 `0x9000`、page1 `0xA000`……；当前有效配置在 page0（seq=118）。
- 阶段 B 拟**只写 page0 的 4 KiB**（`write_flash 0x9000 nvs_page0_bad.bin`）：esptool 擦除该 4 KiB 扇区后写回；page1（旧 43.x 副本）与其他分区不动。
- page0 的页头（state=`0xFFFFFFFE`、seq=118）、entry bitmap、BLOB_IDX 均保持原值；仅 entry 8 的 data CRC 与 entry CRC 改变，二者已重算。
- 写入前先生成 page0 镜像并做离线校验：页头 CRC 不变、entry8 CRC 可复算、差异仅上述 22 字节。
- 写入后立即读回 4 KiB 比对；不一致即写回 `nvs_backup_1.bin` 的 page0 并停止。
- 恢复：写回原始 page0（必要时整 16 KiB），读回比对，重启确认静态 `.50`。
- 风险：若 page0 校验失败，NVS 可能回退 page1（旧 43.x），T3 会失败；届时立即恢复备份，不继续测试。

## 7. 当前状态

未生成 `nvs_bad_static.bin` / page0 镜像；未写设备；C8E4 仍为静态 `192.168.1.50`。等待阶段 B 批准。
