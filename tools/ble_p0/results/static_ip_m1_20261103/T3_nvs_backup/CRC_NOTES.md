# NVS CRC 反推验证（备份只读）

- blob data CRC：`zlib.crc32(payload[0x160:0x160+640], 0xFFFFFFFF)` = `0x0619C999`，与备份中 dataCrc32 一致。
- entry CRC：`zlib.crc32(item[0x140:0x144] + item[0x148:0x160], 0xFFFFFFFF)` = `0xEDE22313`，与备份一致。
- 即：`esp_rom_crc32_le(0xFFFFFFFF, ...)` 与 Python `zlib.crc32(buf, 0xFFFFFFFF)` 等价（无最终异或）。
