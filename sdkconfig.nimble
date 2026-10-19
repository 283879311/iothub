# ============================================================================
# NimBLE profile 参数(1.3-62 起已并入默认 sdkconfig/sdkconfig.defaults;
# 本文件保留作为参考/显式覆盖用)
# 1.3-62 起 NimBLE 已是默认,默认构建无需本文件;这里保留参数作参考/显式覆盖。
# 本文件与 sdkconfig.defaults 叠加;BT_HOST 是 choice,这里切主机栈并裁剪 NimBLE 侧,
# BTDM_CTRL_* 控制器配置由 sdkconfig.defaults 保留(两栈共用同一控制器)。
# ============================================================================

# ---- 主机栈:NimBLE(BLE-only),关 Bluedroid ----
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=n

# ---- 角色与容量:数据面只服务 1 个对端,双角色保留(peripheral/central 运行期切换) ----
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y      # central 扫描需要 observer 角色
# CONFIG_BT_NIMBLE_GATT_CACHING is not set       # Espressif 扩展:Bluedroid 形状的发现缓存 API(评估 §2 #18)

# ---- 安全:本机无配对(CMD 壳为应用层 token 鉴权),与 Bluedroid profile 的 SMP=n 对齐 ----
CONFIG_BT_NIMBLE_SECURITY_ENABLE=y
CONFIG_BT_NIMBLE_MAX_BONDS=3

# ---- MTU 与 host 任务 ----
# MTU 256:数据面单块 256B(与 DATA_PIPE_CHUNK_MAX 对齐),msys 池(24×256B)
# 单包 ≤1 块,碎片压力最小;517 大 MTU 对本场景无收益(数据源是 9600 波特 UART)
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=32
CONFIG_BT_NIMBLE_LOG_LEVEL_INFO=y
