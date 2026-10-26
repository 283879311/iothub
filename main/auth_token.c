#include <ctype.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "auth.h"

/* ============================================================================
 * token 槽位实现。
 *
 * 单独成文件(而非并入 auth.c)的原因:auth.c 依赖 config.h / common.h(口令
 * 哈希路径),把它拉进 token store 会让"地基单测"必须伪造整套配置结构体。
 * 本文件只依赖 auth.h + 3 个 ESP 基础头,单测可直接编译真实源码。
 * ========================================================================== */

typedef struct {
    char    hex[APP_TOKEN_HEX_LEN + 1]; /* 32 字符 + NUL,空槽首字节为 '\0' */
    int64_t expire_ms;                  /* esp_timer 毫秒,绝对时刻 */
} token_slot_t;

static token_slot_t s_slots[TOKEN_SRC_MAX];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool token_src_valid(token_source_t src)
{
    return (unsigned)src < (unsigned)TOKEN_SRC_MAX;
}

/* 锁内清槽:固定 33 字节写入,不调用 memset/字符串函数。
 * 调用者必须已持 s_lock。 */
static void token_slot_clear_locked(token_slot_t *slot)
{
    size_t i;

    for (i = 0; i <= APP_TOKEN_HEX_LEN; i++) {
        slot->hex[i] = '\0';
    }
    slot->expire_ms = 0;
}

/* 锁外格式检查:恰好 32 个十六进制字符 + 结束符。
 * 先逐字符判 isxdigit(遇 '\0' 天然为假),所以走到 tok[32] 时前 32 字符
 * 都不是结束符,该位置必在对象内,不存在越界读。 */
static bool token_format_ok(const char *tok)
{
    size_t i;

    for (i = 0; i < APP_TOKEN_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)tok[i])) {
            return false;
        }
    }
    return tok[APP_TOKEN_HEX_LEN] == '\0';
}

bool app_token_store_issue(token_source_t src, char *out, size_t out_size)
{
    static const char kHex[] = "0123456789ABCDEF";
    uint8_t raw[APP_TOKEN_HEX_LEN / 2];
    char local[APP_TOKEN_HEX_LEN + 1];
    int64_t expire_ms;
    size_t i;

    if (!token_src_valid(src) || out == NULL || out_size < sizeof(local)) {
        return false;
    }

    /* 随机数、编码、取时间:全在锁外,临界区不碰非确定耗时操作。 */
    esp_fill_random(raw, sizeof(raw));
    for (i = 0; i < sizeof(raw); i++) {
        local[i * 2] = kHex[raw[i] >> 4];
        local[i * 2 + 1] = kHex[raw[i] & 0x0f];
    }
    local[APP_TOKEN_HEX_LEN] = '\0';
    expire_ms = esp_timer_get_time() / 1000 + APP_TOKEN_TTL_MS;

    portENTER_CRITICAL(&s_lock);
    memcpy(s_slots[src].hex, local, sizeof(local));
    s_slots[src].expire_ms = expire_ms;
    portEXIT_CRITICAL(&s_lock);

    /* 回包只用局部缓冲:并发 clear 不会把刚签发的 token 打成空串。 */
    memcpy(out, local, sizeof(local));
    return true;
}

bool app_token_store_validate(token_source_t src, const char *token)
{
    bool ok = false;
    int64_t now_ms;

    if (!token_src_valid(src) || token == NULL || !token_format_ok(token)) {
        return false;
    }
    now_ms = esp_timer_get_time() / 1000;

    portENTER_CRITICAL(&s_lock);
    if (s_slots[src].hex[0] == '\0' || now_ms >= s_slots[src].expire_ms) {
        /* 空槽或已过期:锁内顺手清掉,避免"过期但内容还在"的半状态。 */
        token_slot_clear_locked(&s_slots[src]);
    } else {
        uint8_t diff = 0;
        size_t i;

        /* 定长 32 次异或:不提前退出,耗时与匹配长度无关。 */
        for (i = 0; i < APP_TOKEN_HEX_LEN; i++) {
            diff |= (uint8_t)((uint8_t)token[i] ^ (uint8_t)s_slots[src].hex[i]);
        }
        /* 不匹配不清槽:否则一个错误 token 就能把有效会话踢下线。 */
        ok = (diff == 0);
    }
    portEXIT_CRITICAL(&s_lock);

    return ok;
}

void app_token_store_clear(token_source_t src)
{
    if (!token_src_valid(src)) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    token_slot_clear_locked(&s_slots[src]);
    portEXIT_CRITICAL(&s_lock);
}

void app_token_store_clear_all(void)
{
    size_t src;

    /* 单次临界区内清全部槽:不允许"清了一半被打断"留下部分旧 token。 */
    portENTER_CRITICAL(&s_lock);
    for (src = 0; src < TOKEN_SRC_MAX; src++) {
        token_slot_clear_locked(&s_slots[src]);
    }
    portEXIT_CRITICAL(&s_lock);
}
