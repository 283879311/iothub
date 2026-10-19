#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

#include "common.h"
#include "config.h"
#include "constants.h"
#include "form_url.h"
#include "http.h"
#include "http_priv.h"
#include "http_utils.h"
#include "mqtt.h"
#include "identity.h"
#include "mqtt.h"
#include "ota.h"
#include "state.h"
#include "status.h"
#include "uart.h"
#include "wifi.h"

typedef struct {
    httpd_handle_t server;
    char scratch[APP_SCRATCH_SIZE];
} web_context_t;

static const char *TAG = "iothub";
static web_context_t s_web;

/* HTTP 并发连接上限:1.3-29 曾为省堆压到 max=1 + lru_purge,但 lru 踢旧不区分
 * 在途请求,任何新连接都会掐断正在发的响应,表现为"登录后部分模块一直加载/报错"。
 * 1.3-36 起:max=4 + lru_purge 关闭 —— 槽位覆盖"页面在发请求 + 手机门户探测 +
 * 另一个客户端",槽位满时新连接在 backlog 排队而非踢旧;前端 getJson 改为全局
 * 单飞队列,自身只占用 1 条连接。固定开销仅多 ~2–3KB。
 * 配套:idle 会话收割任务(见下,1.3-73 起为无条件 3 分钟)关闭空闲连接,
 * 避免"老页面的闲置连接占满槽位、新客户端永远排队"。 */
#define APP_HTTPD_MAX_OPEN_SOCKETS 4

char *app_http_scratch_buf(void)
{
    return s_web.scratch;
}

size_t app_http_scratch_size(void)
{
    return sizeof(s_web.scratch);
}

/* ---------------------------------------------------------------------------
 * 会话活跃度追踪 + 空闲收割(1.3-36 建立;1.3-73 改为无条件 3 分钟)
 * lru_purge 关闭后,槽位满时新连接在 backlog 排队;浏览器 keep-alive 的闲置
 * 连接却不会自行释放。为此:
 *  - 每个请求入口 touch 记录 fd 最近活跃时刻;
 *  - 收割任务对所有连接生效:空闲 >3 分钟关闭;未到达处理器的空连接从首次
 *    发现开始计时,同样 3 分钟回收。
 * 阈值 3min 远大于状态轮询 10s/请求时长(处理器毫秒级),不会误伤在途请求;
 * 被关的只是闲置连接,下个请求由浏览器透明重连。
 * ------------------------------------------------------------------------- */
/* 1.3-73:任意 HTTP 连接空闲 3 分钟自动关闭(不再只在槽位紧张时回收),
 * 覆盖浏览器 keep-alive、老页面连接和"从未到达处理器"的空连接,及时归还
 * lwIP/httpd 连接与堆。页面自身状态轮询 10s,正常使用不会被误杀。 */
#define APP_HTTP_IDLE_CLOSE_MS  (3 * 60 * 1000)
#define APP_HTTP_REAPER_PERIOD_MS 5000
#define APP_HTTP_ACTIVITY_SLOTS (APP_HTTPD_MAX_OPEN_SOCKETS + 4)

typedef struct {
    int fd;
    int64_t last_active_ms;
} http_sess_activity_t;

static http_sess_activity_t s_http_activity[APP_HTTP_ACTIVITY_SLOTS];

static void app_http_touch_session(httpd_req_t *req)
{
    int fd = (req != NULL) ? httpd_req_to_sockfd(req) : -1;
    int64_t now = esp_timer_get_time() / 1000;
    size_t i;
    int free_slot = -1;

    if (fd < 0) {
        return;
    }
    for (i = 0; i < APP_HTTP_ACTIVITY_SLOTS; i++) {
        if (s_http_activity[i].fd == fd) {
            s_http_activity[i].last_active_ms = now;
            return;
        }
        if (free_slot < 0 && s_http_activity[i].fd <= 0) {
            free_slot = (int)i;
        }
    }
    if (free_slot >= 0) {
        s_http_activity[free_slot].fd = fd;
        s_http_activity[free_slot].last_active_ms = now;
    }
}

static void app_http_activity_clear(int fd)
{
    size_t i;

    for (i = 0; i < APP_HTTP_ACTIVITY_SLOTS; i++) {
        if (s_http_activity[i].fd == fd) {
            s_http_activity[i].fd = 0;
            s_http_activity[i].last_active_ms = 0;
        }
    }
}

/* 1.3-73:登录接管 —— 关闭除当前登录连接外的所有 HTTP 客户端。
 * httpd 单任务串行,登录处理期间其它连接都是空闲等待态,关闭安全;
 * 旧会话 token 由 web_session_drop_others() 同步作废,旧页面下一请求 401→/portal。 */
static void app_http_close_other_clients(int keep_fd)
{
    int fds[APP_HTTPD_MAX_OPEN_SOCKETS + 2];
    size_t count = sizeof(fds) / sizeof(fds[0]);
    unsigned closed = 0;
    size_t i;

    if (s_web.server == NULL ||
        httpd_get_client_list(s_web.server, &count, fds) != ESP_OK) {
        return;
    }
    for (i = 0; i < count; i++) {
        if (fds[i] == keep_fd || fds[i] <= 0) {
            continue;
        }
        httpd_sess_trigger_close(s_web.server, fds[i]);
        app_http_activity_clear(fds[i]);
        closed++;
    }
    if (closed > 0) {
        ESP_LOGI(TAG, "login takeover: closed %u other http connection(s)", closed);
    }
}

static void app_http_reaper_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_HTTP_REAPER_PERIOD_MS));
        if (s_web.server == NULL) {
            continue;
        }
        {
            int fds[APP_HTTPD_MAX_OPEN_SOCKETS + 2];
            size_t count = sizeof(fds) / sizeof(fds[0]);
            int64_t now = esp_timer_get_time() / 1000;
            size_t i;

            if (httpd_get_client_list(s_web.server, &count, fds) != ESP_OK) {
                continue;
            }
            for (i = 0; i < count; i++) {
                size_t j;
                int64_t last = 0;

                for (j = 0; j < APP_HTTP_ACTIVITY_SLOTS; j++) {
                    if (s_http_activity[j].fd == fds[i]) {
                        last = s_http_activity[j].last_active_ms;
                        break;
                    }
                }
                if (last <= 0) {
                    /* 尚未到达处理器的连接(门户探测/半开连接):从首次发现
                     * 开始计时,3 分钟内没有任何请求同样回收。 */
                    for (j = 0; j < APP_HTTP_ACTIVITY_SLOTS; j++) {
                        if (s_http_activity[j].fd <= 0) {
                            s_http_activity[j].fd = fds[i];
                            s_http_activity[j].last_active_ms = now;
                            last = now;
                            break;
                        }
                    }
                    if (last <= 0) {
                        continue;
                    }
                }
                if (now - last > APP_HTTP_IDLE_CLOSE_MS) {
                    ESP_LOGW(TAG, "http idle session reap: fd=%d idle_ms=%lld",
                             fds[i], (long long)(now - last));
                    httpd_sess_trigger_close(s_web.server, fds[i]);
                    app_http_activity_clear(fds[i]);
                }
            }
        }
    }
}

/* ============================================================================
 * Web 会话认证(Cookie)—— 强制门户登录表单的落点(问题与版本优化.md 1.3-01)
 *
 * 背景:手机系统的强制门户小窗(CNA)能弹出并接受 Basic 挑战,但登录后的跳转与
 *   页面子请求不再可靠携带凭据 → 表现为"能登录,却停在'正在打开配置页'"。
 * 方案:落地页改登录表单,POST /api/v1/auth/login 校验后下发会话 Cookie,再
 *   302 到 /config;HTML 页面未认证也改 302 落地页(不再发 Basic 挑战)。
 *   Basic 通道保留(is_authorized 回落),curl/脚本与既有自动化不受影响。
 * 会话:仅内存(重启即失效),4 槽,TTL 1 小时,命中即滑动续期。
 * ========================================================================== */
#define WEB_SESSION_MAX 4
#define WEB_SESSION_TOKEN_HEX 32                    /* 16 字节随机数的 hex 长度 */
#define WEB_SESSION_TTL_MS (60 * 60 * 1000)         /* 1 小时 */
#define WEB_SESSION_COOKIE "iothub_sid"

typedef struct {
    bool used;
    char token[WEB_SESSION_TOKEN_HEX + 1];
    int64_t expires_ms;
} web_session_t;

static web_session_t s_web_sessions[WEB_SESSION_MAX];

static int64_t web_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void web_session_drop_all(void)
{
    memset(s_web_sessions, 0, sizeof(s_web_sessions));
}

/* 新开 /config 时只允许当前会话存活,其余会话全部作废,
 * 老页面下一轮询就会拿到 401,被前端重定向回 /portal 重新登录。
 * keep_token == NULL 或找不到时,直接清空全部会话。 */
static void web_session_drop_others(const char *keep_token)
{
    size_t i;
    unsigned dropped = 0;

    for (i = 0; i < WEB_SESSION_MAX; i++) {
        if (!s_web_sessions[i].used) {
            continue;
        }
        if (keep_token != NULL && strcmp(s_web_sessions[i].token, keep_token) == 0) {
            continue;
        }
        s_web_sessions[i].used = false;
        dropped++;
    }
    if (dropped > 0) {
        ESP_LOGI(TAG, "config session takeover, dropped %u other session(s)", dropped);
    }
}

/* 校验 token;命中即滑动续期,过期槽顺带回收 */
static bool web_session_valid(const char *token)
{
    int64_t now = web_now_ms();
    size_t i;

    if (token == NULL || strlen(token) != WEB_SESSION_TOKEN_HEX) {
        return false;
    }
    for (i = 0; i < WEB_SESSION_MAX; i++) {
        if (!s_web_sessions[i].used) {
            continue;
        }
        if (s_web_sessions[i].expires_ms <= now) {
            s_web_sessions[i].used = false;
            continue;
        }
        if (strcmp(s_web_sessions[i].token, token) == 0) {
            s_web_sessions[i].expires_ms = now + WEB_SESSION_TTL_MS;
            return true;
        }
    }
    return false;
}

/* 新建会话:优先空槽/过期槽,全满则淘汰最早过期者 */
static bool web_session_create(char *out, size_t out_size)
{
    uint8_t raw[16];
    size_t i;
    size_t slot = WEB_SESSION_MAX;
    int64_t now = web_now_ms();

    if (out_size < WEB_SESSION_TOKEN_HEX + 1) {
        return false;
    }
    for (i = 0; i < WEB_SESSION_MAX; i++) {
        if (!s_web_sessions[i].used || s_web_sessions[i].expires_ms <= now) {
            slot = i;
            break;
        }
    }
    if (slot == WEB_SESSION_MAX) {
        slot = 0;
        for (i = 1; i < WEB_SESSION_MAX; i++) {
            if (s_web_sessions[i].expires_ms < s_web_sessions[slot].expires_ms) {
                slot = i;
            }
        }
    }

    esp_fill_random(raw, sizeof(raw));
    for (i = 0; i < sizeof(raw); i++) {
        snprintf(out + (i * 2), 3, "%02x", raw[i]);
    }
    out[WEB_SESSION_TOKEN_HEX] = '\0';

    s_web_sessions[slot].used = true;
    app_copy_string(s_web_sessions[slot].token, sizeof(s_web_sessions[slot].token), out);
    s_web_sessions[slot].expires_ms = now + WEB_SESSION_TTL_MS;
    return true;
}

/* 从 Cookie 头里取本会话 token(键须为头首或 "; " 起始,避免误匹配子串) */
static bool web_cookie_token(httpd_req_t *req, char *out, size_t out_size)
{
    char cookie[192];
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");

    if (len == 0 || len >= sizeof(cookie)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    return app_cookie_value(cookie, WEB_SESSION_COOKIE, out, out_size);
}

esp_err_t app_http_ignore_client_disconnect(esp_err_t err, const char *context)
{
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Client disconnected during %s: %s", context, esp_err_to_name(err));
    return ESP_OK;
}

static app_http_route_t s_registered_routes[APP_HTTP_MAX_HANDLERS];
static size_t s_registered_count = 0;

static bool route_already_seen(const char *uri, httpd_method_t method)
{
    size_t i;
    for (i = 0; i < s_registered_count; i++) {
        if (s_registered_routes[i].method == method &&
            strcmp(s_registered_routes[i].uri, uri) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t app_http_register_uri_handler(httpd_handle_t server,
                                        const char *uri,
                                        httpd_method_t method,
                                        esp_err_t (*handler)(httpd_req_t *req))
{
    if (route_already_seen(uri, method)) {
        return ESP_OK;
    }
    httpd_uri_t cfg = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &cfg);
    if (err == ESP_OK || err == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        if (!route_already_seen(uri, method) &&
            s_registered_count < APP_HTTP_MAX_HANDLERS) {
            s_registered_routes[s_registered_count].uri = uri;
            s_registered_routes[s_registered_count].method = method;
            s_registered_routes[s_registered_count].handler = handler;
            s_registered_count++;
        }
        return ESP_OK;
    }
    if (s_registered_count >= APP_HTTP_MAX_HANDLERS) {
        const char *mstr = method == HTTP_GET ? "GET" :
                          method == HTTP_POST ? "POST" :
                          method == HTTP_PUT ? "PUT" :
                          method == HTTP_DELETE ? "DELETE" : "OTHER";
        ESP_LOGE(TAG, "registered routes table full (%u), cannot register %s %s",
                 (unsigned)s_registered_count, mstr, uri);
        return ESP_ERR_NO_MEM;
    }
    {
        const char *mstr = method == HTTP_GET ? "GET" :
                          method == HTTP_POST ? "POST" :
                          method == HTTP_PUT ? "PUT" :
                          method == HTTP_DELETE ? "DELETE" : "OTHER";
        ESP_LOGE(TAG, "register %s %s failed: %s", mstr, uri, esp_err_to_name(err));
    }
    return err;
}

/* 校验口令:SHA-256(盐||口令) 与配置中的哈希比较。明文口令不落在固件或 NVS。 */
static bool app_http_verify_password(const char *password)
{
    uint8_t salt[16];
    uint8_t stored[32];
    uint8_t calc[32];

    app_config_lock();
    memcpy(salt, s_config.web_pass_salt, sizeof(salt));
    memcpy(stored, s_config.web_pass_hash, sizeof(stored));
    app_config_unlock();

    app_web_hash_password(salt, sizeof(salt), password, calc);
    /* 常数时间比较:按字节差值累加,避免 memcmp 前缀短路泄漏时序信息 */
    {
        volatile uint8_t diff = 0;
        size_t i;
        for (i = 0; i < sizeof(calc); i++) {
            diff |= (uint8_t)(calc[i] ^ stored[i]);
        }
        return diff == 0;
    }
}

/* 当前已建立的 HTTP 连接数(供 /api/v1/device/status 观测 socket 压力,见 1.3-19) */
size_t app_http_client_count(void)
{
    int fds[16];
    size_t count = sizeof(fds) / sizeof(fds[0]);

    if (s_web.server == NULL ||
        httpd_get_client_list(s_web.server, &count, fds) != ESP_OK) {
        return 0;
    }
    return count;
}

size_t app_http_client_max(void)
{
    return APP_HTTPD_MAX_OPEN_SOCKETS;
}

/* 连接数上限:由 lru_purge 自管理 —— 槽位满时新连接在 accept 阶段就顶掉最久
 * 未用的老会话,handler 运行时看到的 client_count 已含自身,再按
 * "count < max" 判满会把合法请求误判 503(1.3-36 实测)。故始终放行。 */
static bool app_http_capacity_ok(httpd_req_t *req)
{
    (void)req;
    return true;
}

/* 客户端视角的**绝对**重定向 URL:优先用请求的 Host(仅接受 IP 字面量),
 * 被 DNS 劫持的探测域名、`.local` 名(mDNS 已移除,1.3-27)或异常 Host 一律回退到 AP 网关。
 * 为什么必须绝对:①部分强制门户小窗(CNA)不跟随相对 Location;
 *                ②局域网侧客户端只能用它的 Host(192.168.1.x)访问,不能被指回 192.168.8.1。
 * 见 问题与版本优化.md 1.3-19 */
static bool app_http_host_is_local(const char *host)
{
    const char *p;

    if (host == NULL || host[0] == '\0') {
        return false;
    }
    for (p = host; *p != '\0'; p++) {
        if (*p != '.' && *p != ':' && (*p < '0' || *p > '9')) {
            return false;                      /* 非 IP 字面量(如探测域名)→ 不可信 */
        }
    }
    return strchr(host, '.') != NULL;
}

static void app_http_absolute_url(httpd_req_t *req, const char *path,
                                  char *out, size_t out_size)
{
    char host[48];

    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
        app_http_host_is_local(host)) {
        snprintf(out, out_size, "http://%s%s", host, path);
        return;
    }
    snprintf(out, out_size, "http://" APP_AP_GATEWAY_IP "%s", path);
}

/* 用户名 + 口令校验(口令为恒定时间比较,见 app_http_verify_password);
 * Basic 与登录表单两条入口共用,避免出现第二份凭据比对逻辑 */
bool app_http_check_credentials(const char *username, const char *password)
{
    char stored_username[33];

    if (username == NULL || password == NULL || username[0] == '\0') {
        return false;
    }
    app_config_lock();
    app_copy_string(stored_username, sizeof(stored_username), s_config.web_username);
    app_config_unlock();
    if (stored_username[0] == '\0' || strcmp(username, stored_username) != 0) {
        return false;
    }
    return app_http_verify_password(password);
}

static bool app_http_is_authorized(httpd_req_t *req)
{
    size_t header_len;
    char auth_header[80];
    unsigned char decoded[128];
    size_t decoded_len = 0;
    const char *separator;
    char username[33];
    char cookie_token[WEB_SESSION_TOKEN_HEX + 1];
    size_t user_len;

    /* ① 会话 Cookie(强制门户登录表单建立)优先 */
    if (web_cookie_token(req, cookie_token, sizeof(cookie_token)) &&
        web_session_valid(cookie_token)) {
        return true;
    }

    /* ② 回落 Basic:兼容 curl/脚本与既有自动化 */
    header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len == 0 || header_len >= sizeof(auth_header)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        return false;
    }
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)auth_header + 6,
                              strlen(auth_header) - 6) != 0 ||
        decoded_len == 0 || decoded_len >= sizeof(decoded)) {
        return false;
    }
    decoded[decoded_len] = '\0';
    separator = memchr(decoded, ':', decoded_len);
    if (separator == NULL) {
        return false;
    }
    user_len = (size_t)(separator - (const char *)decoded);
    if (user_len == 0 || user_len >= sizeof(username)) {
        return false;
    }
    memcpy(username, decoded, user_len);
    username[user_len] = '\0';

    return app_http_check_credentials(username, separator + 1);
}

esp_err_t app_http_send_auth_challenge(httpd_req_t *req)
{
    static int64_t s_last_401_log_ms = 0;

    /* 观测:API 路径此前完全没有日志,导致"页面一直加载中"时无法判断是认证还是连不上。
     * 限频 3s,避免无凭据轮询把串口刷满(见 问题与版本优化.md 1.3-19) */
    if (esp_timer_get_time() / 1000 - s_last_401_log_ms > 3000) {
        s_last_401_log_ms = esp_timer_get_time() / 1000;
        ESP_LOGW(TAG, "auth 401: %.64s", req->uri);
    }

    if (strncmp(req->uri, "/api/", 5) == 0) {
        /* 产品决策(1.3-22):**不再下发 WWW-Authenticate** —— 浏览器原生 Basic 登录框
         * 与"页面内登录表单"重复,用户会反复看到系统认证框(且旧凭据缓存会陷入
         * 输一次弹一次的循环)。统一走 页面内表单 POST → 会话 Cookie。
         * 注意:`Authorization: Basic` 的**接受**逻辑保持不变,curl/自动化脚本不受影响。 */
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return app_http_send_json_text(req, "401 Unauthorized",
                                   "{\"status\":\"error\",\"message\":\"auth_required\"}");
    }

    /* HTML 页面:不发 Basic 挑战 —— 手机强制门户小窗(CNA)无法可靠完成"挑战→
     * 登录→带凭据跳转"这条链路(实测:能弹框、能通过,却停在落地页)。
     * 统一 302 到免认证的登录表单页,由表单 POST 建立会话 Cookie 后再进 /config。
     * 见 问题与版本优化.md 1.3-01 */
    {
        char target[96];

        app_http_absolute_url(req, APP_WEB_PORTAL_PATH, target, sizeof(target));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", target);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return app_http_ignore_client_disconnect(httpd_resp_sendstr(req, "Redirect to portal"),
                                             "auth redirect");
}

bool app_http_require_auth(httpd_req_t *req)
{
    app_http_touch_session(req);
    if (app_http_is_authorized(req)) {
        return true;
    }
    app_http_send_auth_challenge(req);
    return false;
}

/* 仅接受 Cookie 会话:Basic 认证对管理页/轮询无效。
 * 这是为了让"单连接会话接管"对所有浏览器生效 —— 否则某些浏览器(如 Edge)
 * 会缓存并持续发送 Basic 凭据,绕过会话作废机制,导致老页面无法被踢出。 */
bool app_http_require_auth_cookie(httpd_req_t *req)
{
    char cookie_token[WEB_SESSION_TOKEN_HEX + 1];

    app_http_touch_session(req);
    if (web_cookie_token(req, cookie_token, sizeof(cookie_token)) &&
        web_session_valid(cookie_token)) {
        return true;
    }
    app_http_send_auth_challenge(req);
    return false;
}

bool app_http_body_read_finished(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE;
}

esp_err_t http_read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int remaining = req->content_len;
    int received = 0;

    if (remaining >= (int)buf_len) {
        return app_http_send_json_text(req, "413 Payload Too Large",
                                   "{\"status\":\"error\",\"message\":\"payload_too_large\"}");
    }

    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret == 0 || ret == HTTPD_SOCK_ERR_TIMEOUT || ret == HTTPD_SOCK_ERR_FAIL) {
            app_http_ignore_client_disconnect(ESP_FAIL, "request body");
            return ESP_ERR_INVALID_STATE;
        }
        if (ret == HTTPD_SOCK_ERR_INVALID) {
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"body_read_failed\"}");
        }
        received += ret;
        remaining -= ret;
    }

    buf[received] = '\0';
    return ESP_OK;
}

/* 页面资产构建期已 gzip 预压缩,按 Accept-Encoding 决定能否出 .gz */
static bool http_accepts_gzip(httpd_req_t *req)
{
    char value[128];
    size_t len = httpd_req_get_hdr_value_len(req, "Accept-Encoding");
    size_t i;

    if (len == 0 || len >= sizeof(value)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    for (i = 0; i < len; i++) {
        value[i] = (char)tolower((unsigned char)value[i]);
    }
    return strstr(value, "gzip") != NULL;
}

/* 单个页面资产上限:storage 分区 32K,单文件不可能超过;留余量防止异常文件吃光堆 */
#define HTTP_HTML_MAX_BYTES (32 * 1024)
/* 流式发送的分块大小(基准值)。取值必须远小于堆上"最大连续块":BLE 常驻时实测仅 ~17KB,
 * 而整页需要 16KB —— 一次性 malloc 会随机失败(表现为网页转圈/白页)。
 * 批 0 上机(2026-10-14)发现:全常驻(AP+STA+BLE+MQTT)时 largest 会跌到 2-5KB,
 * 4KB chunk 在发送中途申请 lwIP pbuf 仍会失败,改为 2KB 以提高内存弹性。
 * 2KB 仍与 littlefs 读粒度(4096)整除,读缓存命中不变。
 * 1.3-36 追记:2KB 仍会在 largest 1-2KB 时中断发送(实测"html send failed …
 * largest=1152"、页面半截、面板缺失)—— 现按 largest 自适应降档:>=6144 用 2KB,
 * >=3072 用 1KB,再低直接 503 让前端重试,绝不再出半截页面。 */
#define HTTP_HTML_CHUNK_BYTES 2048
/* A/B: chunk 完成后的主动让出时间(ms);当前基线 20。 */
#define HTTP_HTML_CHUNK_GAP_MS 20
/* 实验 B:每块发送前若 largest<2048,有界等待。0=关闭(B0)。 */
#define HTTP_HTML_LOW_HEAP_WAIT_MS 0
#define HTTP_HTML_LOW_HEAP_WAIT_MAX_MS 0
/* A/B 实验:页面首次发送的分块上限。默认 1024;512/256 用于对比首次
 * send() 对 Wi-Fi TX 池的冲击。数组容量仍是 HTTP_HTML_CHUNK_BYTES。 */
#define HTTP_HTML_INITIAL_CHUNK_BYTES 512

/* ---------------------------------------------------------------------------
 * 页面文件句柄缓存(2026-10-15 实测):MQTT 连接态下堆只剩 ~5KB、largest ~2.9KB,
 * 每次请求 fopen/fclose 的分配(newlib FILE 缓冲 + littlefs 文件结构 ~1.2KB)会
 * 在低堆下随机失败,失败模式是"fread 干净返回 0"(EOF 假象)→ page_read_failed。
 * 修复:启动后首次打开成功即**常驻句柄**,后续请求只 fseek+fread,不再 open/close。
 * httpd 单任务串行,无并发问题;storage OTA 更新后调用 app_http_html_cache_reset()。 */
static FILE *s_html_cached = NULL;
static long s_html_cached_size = 0;
static char s_html_cached_path[96];
static bool s_html_cached_gz = false;   /* 缓存句柄对应内容是否 gzip 预压缩 */
static volatile bool s_html_serving = false; /* 页面流式发送中(供 MQTT 重连避让,1.3-36) */

/* 1.3-89 页面发送观测:只统计,不改变分块/协调行为。
 * send() 失败重试发生在 app_http_send_override,计数器在每页开始时清零。 */
static uint32_t s_page_send_retries;
static uint32_t s_page_send_eagain;
static uint32_t s_page_send_enobufs;
static uint32_t s_page_send_enomem;
static uint32_t s_page_send_timeouts;
static uint32_t s_page_send_errors;
static uint32_t s_page_send_calls;
static uint32_t s_page_send_attempts;
static uint32_t s_page_send_ok;
static uint32_t s_page_send_partial;
static uint32_t s_page_send_len_le16;
static uint32_t s_page_send_len_le128;
static uint32_t s_page_send_len_gt128;
static uint32_t s_page_send_eagain_le16;
static uint32_t s_page_send_eagain_le128;
static uint32_t s_page_send_eagain_gt128;
static uint64_t s_page_send_fn_max_us;
static uint64_t s_page_send_override_max_us;
static esp_timer_handle_t s_post_page_timer;
static volatile uint32_t s_page_seq;
static volatile uint32_t s_post_seq;

typedef struct {
    long fsize;
    size_t chunk_bytes;
    size_t bytes_sent;
    uint32_t seq;
    uint32_t chunks;
    uint32_t downshift_512;
    uint32_t downshift_256;
    uint32_t low_wait_count;
    uint32_t low_wait_total_ms;
    uint32_t low_wait_max_ms;
    uint32_t low_abort;
    uint32_t send_retries;
    uint32_t send_eagain;
    uint32_t send_enobufs;
    uint32_t send_enomem;
    uint32_t send_timeouts;
    uint32_t send_errors;
    uint32_t send_calls;
    uint32_t send_attempts;
    uint32_t send_ok;
    uint32_t send_partial;
    uint32_t send_len_le16;
    uint32_t send_len_le128;
    uint32_t send_len_gt128;
    uint32_t send_eagain_le16;
    uint32_t send_eagain_le128;
    uint32_t send_eagain_gt128;
    uint64_t chunk_max_us;
    uint64_t send_fn_max_us;
    uint64_t override_max_us;
    uint64_t page_total_us;
    int64_t page_start_us;
    size_t start_free, start_largest;
    size_t min_free, min_largest;
    size_t end_free, end_largest;
    size_t start_free8, start_largest8;
    size_t min_free8, min_largest8;
    size_t end_free8, end_largest8;
    size_t global_min8_start, global_min8_end;
    int outbox_start, outbox_end;
} http_page_stats_t;

static void app_http_page_sample(http_page_stats_t *s)
{
    const size_t free_default = esp_get_free_heap_size();
    const size_t largest_default = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if (free_default < s->min_free) {
        s->min_free = free_default;
    }
    if (largest_default < s->min_largest) {
        s->min_largest = largest_default;
    }
    if (free8 < s->min_free8) {
        s->min_free8 = free8;
    }
    if (largest8 < s->min_largest8) {
        s->min_largest8 = largest8;
    }
}

static void app_http_log_page_stats(const http_page_stats_t *s, esp_err_t err)
{
    ESP_LOGI(TAG,
             "html page: fsize=%ld sent=%u chunk=%u start=%u/%u min=%u/%u end=%u/%u "
             "| 8bit start=%u/%u min=%u/%u end=%u/%u | min8_all=%u->%u "
             "| outbox=%d->%d | down=%u/%u | send retry=%u eagain=%u "
             "enobufs=%u enomem=%u timeout=%u err=%s",
             s->fsize, (unsigned)s->bytes_sent, (unsigned)s->chunk_bytes,
             (unsigned)s->start_free, (unsigned)s->start_largest,
             (unsigned)s->min_free, (unsigned)s->min_largest,
             (unsigned)s->end_free, (unsigned)s->end_largest,
             (unsigned)s->start_free8, (unsigned)s->start_largest8,
             (unsigned)s->min_free8, (unsigned)s->min_largest8,
             (unsigned)s->end_free8, (unsigned)s->end_largest8,
             (unsigned)s->global_min8_start, (unsigned)s->global_min8_end,
             s->outbox_start, s->outbox_end,
             (unsigned)s->downshift_512, (unsigned)s->downshift_256,
             s->send_retries, s->send_eagain, s->send_enobufs,
             s->send_enomem, s->send_timeouts, esp_err_to_name(err));
}

static void app_http_post_page_cb(void *arg)
{
    uint32_t seq = s_post_seq;
    size_t free_default;
    size_t largest_default;
    size_t free8;
    size_t largest8;

    (void)arg;
    if (s_html_serving || seq != s_page_seq) {
        ESP_LOGI(TAG,
                 "html page post: seq=%u skipped (page=%d current_seq=%u)",
                 (unsigned)seq, s_html_serving ? 1 : 0, (unsigned)s_page_seq);
        return;
    }
    free_default = esp_get_free_heap_size();
    largest_default = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "html page post: seq=%u delay_ms=500 free=%u largest=%u "
             "free8=%u largest8=%u",
             (unsigned)seq, (unsigned)free_default, (unsigned)largest_default,
             (unsigned)free8, (unsigned)largest8);
}

static void app_http_post_page_init(void)
{
    esp_timer_create_args_t args = {
        .callback = app_http_post_page_cb,
        .arg = NULL,
        .name = "html_post",
    };

    if (s_post_page_timer != NULL) {
        return;
    }
    if (esp_timer_create(&args, &s_post_page_timer) != ESP_OK) {
        s_post_page_timer = NULL;
        ESP_LOGW(TAG, "html post-page timer create failed");
    }
}

static void app_http_log_page_timing(const http_page_stats_t *s, esp_err_t err)
{
    ESP_LOGI(TAG,
             "html page timing: seq=%u page_ms=%llu chunk_max_us=%llu send_fn_max_us=%llu "
             "override_max_us=%llu | calls=%u attempts=%u ok=%u partial=%u "
             "retries=%u eagain=%u | len<=16=%u <=128=%u >128=%u "
             "eagain_len<=16=%u <=128=%u >128=%u "
             "| wait=%u wait_ms=%u wait_max=%u abort=%u err=%s",
             (unsigned)s->seq, (unsigned long long)(s->page_total_us / 1000u),
             (unsigned long long)s->chunk_max_us,
             (unsigned long long)s->send_fn_max_us,
             (unsigned long long)s->override_max_us,
             (unsigned)s->send_calls, (unsigned)s->send_attempts,
             (unsigned)s->send_ok, (unsigned)s->send_partial,
             (unsigned)s->send_retries, (unsigned)s->send_eagain,
             (unsigned)s->send_len_le16, (unsigned)s->send_len_le128,
             (unsigned)s->send_len_gt128,
             (unsigned)s->send_eagain_le16, (unsigned)s->send_eagain_le128,
             (unsigned)s->send_eagain_gt128,
             (unsigned)s->low_wait_count, (unsigned)s->low_wait_total_ms,
             (unsigned)s->low_wait_max_ms, (unsigned)s->low_abort,
             esp_err_to_name(err));
}

bool app_http_serving_page(void)
{
    return s_html_serving;
}

/* 开机预热:在堆健康的启动阶段就把页面句柄打开常驻,
 * 避免"首个请求既要 open(分配 ~1.2KB)又要发送"在低堆态翻车。 */
void app_http_html_cache_warmup(void)
{
    const char *path = APP_BASE_PATH "/config.html.gz";

    if (s_html_cached != NULL) {
        return;
    }
    snprintf(s_html_cached_path, sizeof(s_html_cached_path), "%s", path);
    s_html_cached = fopen(path, "rb");
    if (s_html_cached == NULL) {
        s_html_cached_path[0] = '\0';
        return;
    }
    (void)setvbuf(s_html_cached, NULL, _IONBF, 0);
    if (fseek(s_html_cached, 0, SEEK_END) != 0 ||
        (s_html_cached_size = ftell(s_html_cached)) <= 0 ||
        s_html_cached_size > HTTP_HTML_MAX_BYTES) {
        app_http_html_cache_reset();
        return;
    }
    fseek(s_html_cached, 0, SEEK_SET);
    s_html_cached_gz = true;
    ESP_LOGI(TAG, "html cache warmup ok (%s, %ld bytes)", path, s_html_cached_size);
}

void app_http_html_cache_reset(void)
{
    if (s_html_cached != NULL) {
        fclose(s_html_cached);
        s_html_cached = NULL;
    }
    s_html_cached_size = 0;
    s_html_cached_path[0] = '\0';
    s_html_cached_gz = false;
    ESP_LOGI(TAG, "html cache reset");
}

static void app_http_send_note_len(size_t len)
{
    if (!s_html_serving) {
        return;
    }
    s_page_send_calls++;
    if (len <= 16) {
        s_page_send_len_le16++;
    } else if (len <= 128) {
        s_page_send_len_le128++;
    } else {
        s_page_send_len_gt128++;
    }
}

static void app_http_send_note_eagain_len(size_t len)
{
    if (!s_html_serving) {
        return;
    }
    if (len <= 16) {
        s_page_send_eagain_le16++;
    } else if (len <= 128) {
        s_page_send_eagain_le128++;
    } else {
        s_page_send_eagain_gt128++;
    }
}

static void app_http_send_note_override(int64_t start_us)
{
    uint64_t elapsed;

    if (!s_html_serving) {
        return;
    }
    elapsed = (uint64_t)(esp_timer_get_time() - start_us);
    if (elapsed > s_page_send_override_max_us) {
        s_page_send_override_max_us = elapsed;
    }
}

/* 1.3-80/P1:默认 httpd send 在 EAGAIN/ENOBUFS 时直接失败;这里做有界等待,
 * 让 WiFi TX buffer 先排空。总等待 ~400ms,超时仍返回 HTTPD_SOCK_ERR_TIMEOUT,
 * 由 httpd 关闭会话,避免单任务被无限阻塞(适用于明文 HTTP,不用于 HTTPS)。
 * 1.3-89:只增加调用/耗时/按长度分桶统计,不改变重试与延时行为。 */
static int app_http_send_override(httpd_handle_t hd, int sockfd, const char *buf,
                                  size_t buf_len, int flags)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
    int64_t override_start = esp_timer_get_time();
    int attempt;

    (void)hd;
    if (buf == NULL) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    app_http_send_note_len(buf_len);
    (void)setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    for (attempt = 0; attempt < 20; attempt++) {
        int64_t send_start = esp_timer_get_time();
        int ret = send(sockfd, buf, buf_len, flags);
        uint64_t send_us = (uint64_t)(esp_timer_get_time() - send_start);

        if (s_html_serving) {
            s_page_send_attempts++;
            if (send_us > s_page_send_fn_max_us) {
                s_page_send_fn_max_us = send_us;
            }
        }
        if (ret > 0) {
            if (s_html_serving) {
                s_page_send_ok++;
                if ((size_t)ret < buf_len) {
                    s_page_send_partial++;
                }
            }
            app_http_send_note_override(override_start);
            return ret;
        }
        if (ret == 0) {
            if (s_html_serving) {
                s_page_send_errors++;
            }
            app_http_send_note_override(override_start);
            return HTTPD_SOCK_ERR_FAIL;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == ENOMEM) {
            if (s_html_serving) {
                s_page_send_retries++;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    s_page_send_eagain++;
                    app_http_send_note_eagain_len(buf_len);
                } else if (errno == ENOBUFS) {
                    s_page_send_enobufs++;
                } else {
                    s_page_send_enomem++;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (errno == EINTR) {
            if (s_html_serving) {
                s_page_send_retries++;
            }
            continue;
        }
        if (s_html_serving) {
            s_page_send_errors++;
        }
        app_http_send_note_override(override_start);
        return (errno == EBADF || errno == EINVAL) ? HTTPD_SOCK_ERR_INVALID
                                                   : HTTPD_SOCK_ERR_FAIL;
    }
    if (s_html_serving) {
        s_page_send_timeouts++;
    }
    app_http_send_note_override(override_start);
    return HTTPD_SOCK_ERR_TIMEOUT;
}

static esp_err_t http_serve_file(httpd_req_t *req, const char *file_name,
                                  const char *content_type, bool require_auth,
                                  bool cache_immutable)
{
    char path[96];
    char etag[48];
    char if_none_match[64];
    FILE *file = NULL;
    long fsize;
    long i;
    uint32_t hash = 2166136261u;                /* FNV-1a 32bit */
    bool accepts_gzip = http_accepts_gzip(req);
    bool use_gzip = false;
    size_t chunk_bytes = HTTP_HTML_CHUNK_BYTES; /* 自适应分块:见下方堆预检 */
    esp_err_t err;
    http_page_stats_t page = {
        .min_free = SIZE_MAX,
        .min_largest = SIZE_MAX,
        .min_free8 = SIZE_MAX,
        .min_largest8 = SIZE_MAX,
    };

    /* 管理页认证按文件级统一控制,新增路由只要经此函数出 config.html 就自动覆盖。
     * 管理页必须走 Cookie 会话(拒绝 Basic),否则某些浏览器持续发 Basic 凭据会
     * 绕过下面的单连接会话接管。 */
    if (require_auth) {
        char cookie_token[WEB_SESSION_TOKEN_HEX + 1];

        if (!app_http_require_auth_cookie(req)) {
            return ESP_OK;
        }
        /* 单连接接管:新 /config 会话生效时,作废其它所有会话,
         * 老页面下一轮询会 401,被前端重定向回 /portal 重新登录。 */
        if (web_cookie_token(req, cookie_token, sizeof(cookie_token)) &&
            web_session_valid(cookie_token)) {
            web_session_drop_others(cookie_token);
        } else {
            web_session_drop_all();
        }
    }

    /* 优先出 .gz;旧布局 storage.bin 只有未压缩文件时自动回落。
     * 句柄常驻缓存:首次打开成功后跨请求复用,后续请求零 open/close 分配。 */
    if (accepts_gzip) {
        snprintf(path, sizeof(path), "%s/%s.gz", APP_BASE_PATH, file_name);
    } else {
        snprintf(path, sizeof(path), "%s/%s", APP_BASE_PATH, file_name);
    }
    if (s_html_cached == NULL || strcmp(s_html_cached_path, path) != 0) {
        app_http_html_cache_reset();
        snprintf(s_html_cached_path, sizeof(s_html_cached_path), "%s", path);
        s_html_cached = fopen(path, "rb");
        if (s_html_cached != NULL) {
            (void)setvbuf(s_html_cached, NULL, _IONBF, 0);
            if (fseek(s_html_cached, 0, SEEK_END) != 0 ||
                (s_html_cached_size = ftell(s_html_cached)) <= 0 ||
                s_html_cached_size > HTTP_HTML_MAX_BYTES) {
                app_http_html_cache_reset();
            } else {
                fseek(s_html_cached, 0, SEEK_SET);
                use_gzip = true;
                s_html_cached_gz = true;
            }
        }
    }
    if (s_html_cached == NULL) {
        /* .gz 不存在时回落未压缩文件(老布局) */
        if (accepts_gzip) {
            snprintf(path, sizeof(path), "%s/%s", APP_BASE_PATH, file_name);
            snprintf(s_html_cached_path, sizeof(s_html_cached_path), "%s", path);
            s_html_cached = fopen(path, "rb");
            if (s_html_cached != NULL) {
                (void)setvbuf(s_html_cached, NULL, _IONBF, 0);
                if (fseek(s_html_cached, 0, SEEK_END) != 0 ||
                    (s_html_cached_size = ftell(s_html_cached)) <= 0 ||
                    s_html_cached_size > HTTP_HTML_MAX_BYTES) {
                    app_http_html_cache_reset();
                } else {
                    fseek(s_html_cached, 0, SEEK_SET);
                    use_gzip = false;
                    s_html_cached_gz = false;
                }
            }
        }
        if (s_html_cached == NULL) {
            if (!accepts_gzip) {
                return app_http_send_json_text(req, "406 Not Acceptable",
                                           "{\"status\":\"error\",\"message\":\"gzip_required\"}");
            }
            return app_http_send_json_text(req, "500 Internal Server Error",
                                       "{\"status\":\"error\",\"message\":\"page_open_failed\"}");
        }
    }
    file = s_html_cached;
    fsize = s_html_cached_size;
    use_gzip = s_html_cached_gz;   /* 缓存命中路径必须恢复此值,否则不发 Content-Encoding */

    /* 两遍 4KB 流式(问题与版本优化.md:H2 堆紧张):
     *   第一遍只算大小 + FNV-1a(内容不驻留),第二遍边读边发。
     * 这样峰值内存需求从"整页 16KB"降到 4KB,同时**保留 ETag/304 语义**。
     * 代价:响应改回 chunked(无 Content-Length),浏览器仍按到达进度渲染。
     * 静态缓冲:httpd 单任务按请求串行执行,不存在重入。 */
    static char chunk[HTTP_HTML_CHUNK_BYTES];

    if (fsize <= 0 || fsize > HTTP_HTML_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"page_size_invalid\"}");
    }

    /* 堆预检(见 问题与版本优化.md 1.3-19):分块发送本身还要申请 lwIP 发送 pbuf,
     * 若最大连续块已不足,就**不要开始**注定失败的流 —— 否则客户端只拿到
     * 半截响应,浏览器那条连接会僵死,后续请求全部排不上队(面板"一直加载中")。
     * 这里直接回一个小体积 503,让前端按可重试错误处理。
     * 1.3-36:分块大小按 largest 自适应(2048/1024/512),门槛从 3072 降到 2048 ——
     * 实测 broker 不可达时 MQTT 重连把堆压到 free≈3.2KB/largest≈2.5KB,3072 门槛
     * 把配置页挡死在 503;512B 块的 pbuf+余量 ~1.5KB,largest≥2048 即可完整送页,
     * 云端挂掉时管理页必须仍然可用(这正是用户要修配置的时刻)。 */
    chunk_bytes = HTTP_HTML_INITIAL_CHUNK_BYTES;
    {
        const size_t free_bytes = esp_get_free_heap_size();
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

        if (largest < 2048) {
            ESP_LOGW(TAG, "html send skipped (heap low): free=%u largest=%u",
                     (unsigned)free_bytes, (unsigned)largest);
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Retry-After", "2");
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            return app_http_ignore_client_disconnect(
                httpd_resp_sendstr(req,
                    "<!doctype html><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                    "<meta http-equiv=\"refresh\" content=\"2\">"
                    "<h3>设备内存紧张,请稍后刷新</h3>"
                    "<p><a href=\"/config\">点此重试</a>(本页 2 秒后自动重试)</p>"),
                "html busy page");
        }
        /* 1.3-80:2KB 分块在 free 仅 14-16KB 时会把 WiFi 动态 TX 池(约 14KB)
         * 一起撑满,largest 跌到 <1KB 后 httpd send 只能 EAGAIN。先按 free 与
         * largest 双门槛降档;只有 free/largest 都充裕才用 2KB。 */
        if (free_bytes < (20 * 1024) || largest < (8 * 1024)) {
            chunk_bytes = (HTTP_HTML_INITIAL_CHUNK_BYTES < 512)
                              ? HTTP_HTML_INITIAL_CHUNK_BYTES : 512;
        }
        if (free_bytes < (8 * 1024) || largest < (3 * 1024)) {
            chunk_bytes = (HTTP_HTML_INITIAL_CHUNK_BYTES < 256)
                              ? HTTP_HTML_INITIAL_CHUNK_BYTES : 256;
        }
        ESP_LOGI(TAG, "html send chunk=%u free=%u largest=%u",
                 (unsigned)chunk_bytes, (unsigned)free_bytes, (unsigned)largest);

        {
            long remain = fsize;

            while (remain > 0) {
                size_t want = (remain > (long)chunk_bytes) ? chunk_bytes : (size_t)remain;
                size_t got = fread(chunk, 1, want, file);

                if (got != want) {
                    /* 读取失败过去是静默的(外场只见 500/白页)。附堆水位与最大可分配块,
                     * 一眼区分"内存不足"(largest 偏小)与"文件系统读失败"。 */
                    ESP_LOGW(TAG,
                             "html read failed (%ld bytes): fread at %ld got=%d want=%u "
                             "errno=%d ferror=%d ftell=%ld | free=%u largest=%u",
                             fsize, fsize - remain, (int)got, (unsigned)want,
                             errno, ferror(file), ftell(file),
                             (unsigned)esp_get_free_heap_size(),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
                    return app_http_send_json_text(req, "500 Internal Server Error",
                                               "{\"status\":\"error\",\"message\":\"page_read_failed\"}");
                }
                for (i = 0; i < (long)got; i++) {
                    hash ^= (uint8_t)chunk[i];
                    hash *= 16777619u;
                }
                remain -= (long)got;
            }
        }
    }

    /* ETag = 大小 + 内容 FNV-1a。资产只在重烧/OTA 后变化,客户端刷新时带
     * If-None-Match 回来即可命中 304 —— 刷新几乎零传输(MCP/官方文档:
     * ETag 计算的代价远小于重新发送内容)。 */
    snprintf(etag, sizeof(etag), "\"%ld-%08x\"", fsize, (unsigned)hash);

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", cache_immutable
                       ? "public, max-age=31536000, immutable"
                       : (strcmp(file_name, "config.html") == 0
                          ? "private, max-age=60"
                          : "no-cache"));   /* bundle 长期缓存;配置页私有缓存 60s;en 每次校验 */
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");     /* gz / 未压缩是两个变体 */
    if (use_gzip) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    /* 条件请求:命中即 304 空响应(弱校验前缀 W/ 一并容忍) */
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match,
                                    sizeof(if_none_match)) == ESP_OK) {
        const char *cand = if_none_match;
        if (strncmp(cand, "W/", 2) == 0) {
            cand += 2;
        }
        if (strcmp(cand, etag) == 0) {
            httpd_resp_set_status(req, "304 Not Modified");
            return httpd_resp_send(req, NULL, 0);
        }
    }

    /* 第二遍:分块发送,峰值只需一块(4KB)。
     * 先在有限时间内等待"正在进行的 MQTT 连接尝试"结束:连接期间的 ~6KB
     * 堆抖动会把页面掐成半截;对侧(MQTT 重连任务)同样在等本流结束,
     * 双方互避让、无死锁 —— 本等待有界(连接尝试自身 ≤5s 网络超时)。
     * 置位"页面发送中"标志,让后续新连接尝试继续推迟。 */
    {
        int wait_ms = 0;
        while (app_mqtt_connecting() && wait_ms < 4500) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }
        if (wait_ms >= 4500) {
            ESP_LOGW(TAG, "html serve: mqtt connect still in flight, serving anyway");
        }
    }
    {
        int fd = httpd_req_to_sockfd(req);
        if (fd >= 0) {
            (void)httpd_sess_set_send_override(req->handle, fd, app_http_send_override);
        }
    }
    page.fsize = fsize;
    page.chunk_bytes = chunk_bytes;
    page.start_free = esp_get_free_heap_size();
    page.start_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    page.start_free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    page.start_largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    page.global_min8_start = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    page.outbox_start = app_mqtt_outbox_size();
    s_page_send_retries = 0;
    s_page_send_eagain = 0;
    s_page_send_enobufs = 0;
    s_page_send_enomem = 0;
    s_page_send_timeouts = 0;
    s_page_send_errors = 0;
    s_page_send_calls = 0;
    s_page_send_attempts = 0;
    s_page_send_ok = 0;
    s_page_send_partial = 0;
    s_page_send_len_le16 = 0;
    s_page_send_len_le128 = 0;
    s_page_send_len_gt128 = 0;
    s_page_send_eagain_le16 = 0;
    s_page_send_eagain_le128 = 0;
    s_page_send_eagain_gt128 = 0;
    s_page_send_fn_max_us = 0;
    s_page_send_override_max_us = 0;
    page.chunk_max_us = 0;
    s_page_seq++;
    page.seq = s_page_seq;
    page.page_start_us = esp_timer_get_time();
    app_http_page_sample(&page);
    s_html_serving = true;
    app_http_tcp_nodelay(req);   /* 分块流式直发,见 1.3-36 */
    err = ESP_OK;
    if (fseek(file, 0, SEEK_SET) != 0) {
        err = ESP_FAIL;
    } else {
        long remain = fsize;

        while (remain > 0) {
            size_t live_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
            size_t live_free = esp_get_free_heap_size();

            app_http_page_sample(&page);
            if (live_largest < chunk_bytes + 1024) {
                if (chunk_bytes > 512 && live_largest >= 1536) {
                    chunk_bytes = 512;
                    page.downshift_512++;
                    ESP_LOGW(TAG, "html send chunk downshift ->512 (free=%u largest=%u)",
                             (unsigned)live_free, (unsigned)live_largest);
                } else if (chunk_bytes > 256 && live_largest >= 512) {
                    chunk_bytes = 256;
                    page.downshift_256++;
                    ESP_LOGW(TAG, "html send chunk downshift ->256 (free=%u largest=%u)",
                             (unsigned)live_free, (unsigned)live_largest);
                }
            }

#if HTTP_HTML_LOW_HEAP_WAIT_MS > 0
            if (live_largest < 2048) {
                uint32_t waited = 0;

                page.low_wait_count++;
                while (waited < HTTP_HTML_LOW_HEAP_WAIT_MAX_MS && live_largest < 2048) {
                    vTaskDelay(pdMS_TO_TICKS(HTTP_HTML_LOW_HEAP_WAIT_MS));
                    waited += HTTP_HTML_LOW_HEAP_WAIT_MS;
                    live_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
                    live_free = esp_get_free_heap_size();
                }
                page.low_wait_total_ms += waited;
                if (waited > page.low_wait_max_ms) {
                    page.low_wait_max_ms = waited;
                }
                app_http_page_sample(&page);
                if (live_largest < 1024) {
                    page.low_abort++;
                    ESP_LOGW(TAG,
                             "html send aborted: largest<1024 after wait (free=%u largest=%u)",
                             (unsigned)live_free, (unsigned)live_largest);
                    err = ESP_ERR_NO_MEM;
                    break;
                }
                if (live_largest < 2048 && chunk_bytes > 256) {
                    chunk_bytes = 256;
                    page.downshift_256++;
                }
            }
#endif

            size_t want = (remain > (long)chunk_bytes) ? chunk_bytes : (size_t)remain;
            size_t got = fread(chunk, 1, want, file);

            if (got == 0) {
                err = ESP_FAIL;
                break;
            }
            {
                int64_t chunk_start = esp_timer_get_time();
                uint64_t chunk_us;

                err = httpd_resp_send_chunk(req, chunk, got);
                chunk_us = (uint64_t)(esp_timer_get_time() - chunk_start);
                if (chunk_us > page.chunk_max_us) {
                    page.chunk_max_us = chunk_us;
                }
            }
            if (err != ESP_OK) {
                /* 短断流补救(1.3-36):手机在 AP 侧关联/探测会让 STA 空口中断
                 * 几秒,send 超时失败时该 chunk 通常尚未上链,稍候重发同一 chunk;
                 * 补救再失败才放弃(最坏与直接放弃同果:客户端报错/刷新)。 */
                vTaskDelay(pdMS_TO_TICKS(500));
                {
                    int64_t chunk_start = esp_timer_get_time();
                    uint64_t chunk_us;

                    err = httpd_resp_send_chunk(req, chunk, got);
                    chunk_us = (uint64_t)(esp_timer_get_time() - chunk_start);
                    if (chunk_us > page.chunk_max_us) {
                        page.chunk_max_us = chunk_us;
                    }
                }
                if (err != ESP_OK) {
                    break;
                }
            }
            page.chunks++;
            page.bytes_sent += got;
            app_http_page_sample(&page);
            remain -= (long)got;
            /* 1.3-67:分块间让出 10ms。页面本身是常驻 FILE + 2KB 静态块(无整页
             * 堆分配),但 21KB 页面连续灌给 WiFi 时,动态 TX 缓冲池会按需扩到
             * ~14KB 并常驻,把 largest 从 19KB 压到 ~7.9KB(1h 页面负载实测)。
             * 让出后 TX 池先排空/复用,避免为整页一次性扩容;11 块约 +110ms。 */
            vTaskDelay(pdMS_TO_TICKS(HTTP_HTML_CHUNK_GAP_MS));
        }
        if (err == ESP_OK) {
            int64_t chunk_start = esp_timer_get_time();
            uint64_t chunk_us;

            err = httpd_resp_send_chunk(req, NULL, 0);   /* 收尾:0 长度块结束响应 */
            chunk_us = (uint64_t)(esp_timer_get_time() - chunk_start);
            if (chunk_us > page.chunk_max_us) {
                page.chunk_max_us = chunk_us;
            }
        }
    }
    page.page_total_us = (uint64_t)(esp_timer_get_time() - page.page_start_us);
    s_html_serving = false;
    page.chunk_bytes = chunk_bytes;
    page.end_free = esp_get_free_heap_size();
    page.end_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    page.end_free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    page.end_largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    page.global_min8_end = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    page.outbox_end = app_mqtt_outbox_size();
    page.send_retries = s_page_send_retries;
    page.send_eagain = s_page_send_eagain;
    page.send_enobufs = s_page_send_enobufs;
    page.send_enomem = s_page_send_enomem;
    page.send_timeouts = s_page_send_timeouts;
    page.send_errors = s_page_send_errors;
    page.send_calls = s_page_send_calls;
    page.send_attempts = s_page_send_attempts;
    page.send_ok = s_page_send_ok;
    page.send_partial = s_page_send_partial;
    page.send_len_le16 = s_page_send_len_le16;
    page.send_len_le128 = s_page_send_len_le128;
    page.send_len_gt128 = s_page_send_len_gt128;
    page.send_eagain_le16 = s_page_send_eagain_le16;
    page.send_eagain_le128 = s_page_send_eagain_le128;
    page.send_eagain_gt128 = s_page_send_eagain_gt128;
    page.send_fn_max_us = s_page_send_fn_max_us;
    page.override_max_us = s_page_send_override_max_us;
    app_http_log_page_stats(&page, err);
    app_http_log_page_timing(&page, err);
    if (s_post_page_timer != NULL) {
        s_post_seq = page.seq;
        (void)esp_timer_stop(s_post_page_timer);
        if (esp_timer_start_once(s_post_page_timer, 500000) != ESP_OK) {
            ESP_LOGW(TAG, "html post-page timer start failed");
        }
    }
    if (err != ESP_OK) {
        /* 不再静默吞掉:截断必须在外场可见(问题与版本优化.md 第 34 项)。
         * 附堆水位:区分"链路被掐/客户端断开"(堆正常)与"内存已见底"。
         * 1.3-36:流已不完整,返回 ESP_FAIL 让 httpd 关闭会话 —— 客户端立刻
         * 看到截断并报错/重试,而不是对着僵尸 chunked 流干等(实测 curl 挂 40s)。 */
        ESP_LOGW(TAG, "html send failed (%ld bytes): %s | free=%u largest=%u",
                 fsize, esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t http_serve_html(httpd_req_t *req, const char *file_name)
{
    return http_serve_file(req, file_name, "text/html; charset=utf-8",
                           strcmp(file_name, "config.html") == 0, false);
}

static esp_err_t http_serve_asset(httpd_req_t *req, const char *file_name)
{
    const bool immutable = strncmp(file_name, "config-bundle-", 14) == 0;

    return http_serve_file(req, file_name, "application/javascript", false, immutable);
}

static esp_err_t device_info_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth_cookie(req)) {
        return ESP_OK;
    }
    return app_status_send_device_info(req);
}

static esp_err_t device_status_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth_cookie(req)) {
        return ESP_OK;
    }
    return app_status_send_device_status(req);
}

static bool app_uri_ends_with(const char *uri, size_t uri_len, const char *suffix)
{
    size_t suffix_len = strlen(suffix);

    return uri_len >= suffix_len &&
           memcmp(uri + uri_len - suffix_len, suffix, suffix_len) == 0;
}

/* ---------------------------------------------------------------------------
 * 门户登录后的客户端 IP 登记(4 槽,TTL 10 分钟)。
 * 背景(荣耀 300 Ultra / Android 16 实测):登录成功后手机的网络栈会重新发
 * 连通性探测,而该探测由 ConnectivityService 自己发出,**不带 WebView 的会话
 * Cookie** —— 探测永远命中"未登录 → 302 落地页",手机判定仍困在强制门户里,
 * 于是小窗关闭/反复弹,甚至主动断开 AP(实测 reason=3 离开),表现为"无法完成跳转"。
 * 因此对**已登录客户端 IP** 的探测回 204,让重校验通过、小窗关闭、AP 保持。
 * ------------------------------------------------------------------------- */
#define APP_CAPTIVE_AUTH_SLOTS 4
#define APP_CAPTIVE_AUTH_TTL_MS 600000
static char s_captive_authed_ip[APP_CAPTIVE_AUTH_SLOTS][16];
static int64_t s_captive_authed_at_ms[APP_CAPTIVE_AUTH_SLOTS];

bool app_http_peer_ip(httpd_req_t *req, char *out, size_t out_size)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    const char *ip;
    int fd;

    if (req == NULL || out == NULL || out_size == 0) {
        return false;
    }
    fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&ss, &len) != 0 ||
        ss.ss_family != AF_INET) {
        return false;
    }
    ip = inet_ntoa(((struct sockaddr_in *)&ss)->sin_addr);
    if (ip == NULL || strlen(ip) >= out_size) {
        return false;
    }
    app_copy_string(out, out_size, ip);
    return true;
}

static void web_portal_mark_authed(httpd_req_t *req)
{
    char ip[16];
    int64_t now = esp_timer_get_time() / 1000;
    int slot = -1;
    int oldest = 0;
    int i;

    if (!app_http_peer_ip(req, ip, sizeof(ip))) {
        return;
    }
    for (i = 0; i < APP_CAPTIVE_AUTH_SLOTS; i++) {
        if (s_captive_authed_ip[i][0] == '\0') {
            slot = i;                       /* 空槽 */
            break;
        }
        if (strcmp(s_captive_authed_ip[i], ip) == 0) {
            s_captive_authed_at_ms[i] = now; /* 已登记:续期 */
            return;
        }
        if (s_captive_authed_at_ms[i] < s_captive_authed_at_ms[oldest]) {
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;                      /* 无空槽:挤掉最旧 */
    }
    app_copy_string(s_captive_authed_ip[slot], sizeof(s_captive_authed_ip[slot]), ip);
    s_captive_authed_at_ms[slot] = now;
}

static bool web_portal_ip_authed(const char *ip)
{
    int64_t now = esp_timer_get_time() / 1000;
    int i;

    if (ip == NULL || ip[0] == '\0') {
        return false;
    }
    for (i = 0; i < APP_CAPTIVE_AUTH_SLOTS; i++) {
        if (strcmp(s_captive_authed_ip[i], ip) == 0 &&
            now - s_captive_authed_at_ms[i] < APP_CAPTIVE_AUTH_TTL_MS) {
            return true;
        }
    }
    return false;
}

/* AP 模式强制门户:各 OS 的连通性探测端点统一 302 到落地页。
 * 探测发生在登录前,故不要求认证 */
static bool app_http_is_captive_probe(const char *uri)
{
    static const char *const probes[] = {
        "generate_204",                 /* 安卓各厂商 */
        "gen_204",
        "hotspot-detect.html",          /* iOS/macOS */
        "library/test/success.html",    /* 老版本 iOS */
        "connecttest.txt",              /* Windows 10/11 */
        "ncsi.txt",                     /* 老版本 Windows */
        "success.txt",                  /* Firefox */
    };
    const char *path = uri;
    size_t path_len;
    size_t i;

    /* 兼容 absolute-form 请求行(GET http://host/path):跳过 scheme+authority */
    {
        const char *scheme = strstr(uri, "://");
        if (scheme != NULL) {
            const char *slash = strchr(scheme + 3, '/');
            path = slash != NULL ? slash : "/";
        }
    }
    /* 探测可能带 query string,截断后再匹配 */
    path_len = strlen(path);
    {
        const char *query = memchr(path, '?', path_len);
        if (query != NULL) {
            path_len = (size_t)(query - path);
        }
    }
    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (app_uri_ends_with(path, path_len, probes[i])) {
            return true;
        }
    }
    return false;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *file_name = NULL;
    char host[48];

    if (!app_http_capacity_ok(req)) {
        return ESP_OK;
    }

    /* 全量请求日志:外场定位强制门户问题(此前 401/404 分支静默,手机行为不可见)。
     * URI 截断到 96 字符:被 DNS 劫持的业务请求 query 可达数百字节,全量打印会
     * 造成串口日志洪水(问题与版本优化.md 第 30 项)。
     * 1.3-49:在截断之上再加**限频** —— 手机侧被劫持请求的重试风暴实测可达
     * ~250 请求/秒(双板观测,串口被打满成"i_len=50"残片),全量逐条打印会白烧
     * CPU/串口并淹没真实日志。前 8 条全打,其后每 25 条打一条(带累计计数);
     * 被跳过的请求连 Host 头都不再查询(省一次 httpd 解析)。 */
    {
        static uint32_t s_get_seen;
        static uint32_t s_get_logged;

        s_get_seen++;
        if (s_get_logged < 8u || (s_get_seen % 25u) == 0u) {
            s_get_logged++;
            if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
                host[0] = '\0';
            }
            ESP_LOGI(TAG, "http get #%u %.96s%s (host=%.47s) uri_len=%u",
                     (unsigned)s_get_seen,
                     req->uri, strlen(req->uri) > 96 ? "..." : "",
                     host[0] != '\0' ? host : "-", (unsigned)strlen(req->uri));
        }
    }

    {
        static const struct {
            const char *uri;
            const char *file;
        } assets[] = {
            {"/config-bundle-i18n.js", "config-bundle-i18n.js"},
            {"/config-bundle-core.js", "config-bundle-core.js"},
            {"/config-bundle-app.js",  "config-bundle-app.js"},
            {"/config-i18n-en.js",     "config-i18n-en.js"},
        };
        size_t i;

        const size_t uri_len = strcspn(req->uri, "?");

        for (i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {
            const size_t asset_len = strlen(assets[i].uri);

            if (uri_len == asset_len &&
                strncmp(req->uri, assets[i].uri, asset_len) == 0) {
                return http_serve_asset(req, assets[i].file);
            }
        }
    }

    if (app_http_is_captive_probe(req->uri)) {
        /* 已登录客户端:回 204 让手机连通性重校验通过(见 web_portal_mark_authed
         * 头注释,荣耀/Android 16 的重校验探测不带 Cookie,只能按 IP 认人) */
        char peer_ip[16];
        if (app_http_peer_ip(req, peer_ip, sizeof(peer_ip)) &&
            web_portal_ip_authed(peer_ip)) {
            ESP_LOGI(TAG, "captive probe %s -> 204 (client already authed)", req->uri);
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return app_http_ignore_client_disconnect(httpd_resp_send(req, NULL, 0),
                                                     "captive 204");
        }
        /* 302 目标必须免认证 200:CNA 弹窗无法渲染 Basic 挑战,指到 / 会 401 关窗 */
        char target[96];

        ESP_LOGI(TAG, "captive probe %s -> 302 " APP_WEB_PORTAL_PATH, req->uri);
        app_http_absolute_url(req, APP_WEB_PORTAL_PATH, target, sizeof(target));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", target);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Redirect to captive portal");
    }

    /* 单页形态:所有页面入口(含根路径)都出配置页,
     * config.html 的认证在 http_serve_html 内按文件名统一控制 */
    if (strcmp(req->uri, "/") == 0 || strcmp(req->uri, "/index.html") == 0 ||
        strcmp(req->uri, APP_WEB_CONFIG_PATH) == 0 ||
        strcmp(req->uri, APP_WEB_CONFIG_PATH "/") == 0 ||
        strcmp(req->uri, "/config.html") == 0) {
        file_name = "config.html";
    } else if (strncmp(req->uri, "/api/", 5) == 0) {
        /* API 语义不变:未知接口仍是 404 JSON */
        return app_http_send_json_text(req, "404 Not Found",
                                   "{\"status\":\"error\",\"message\":\"not_found\"}");
    } else {
        /* 强制门户兜底:非 /api 的未知路径统一 302 到落地页。客户端(如被 DNS
         * 劫持的业务 App)拿到 302 后跟随一次即收敛,不再对 404 反复重试,
         * 避免连接风暴打满 socket(问题与版本优化.md 第 30 项) */
        char target[96];

        app_http_absolute_url(req, APP_WEB_PORTAL_PATH, target, sizeof(target));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", target);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Redirect to captive portal");
    }
    return http_serve_html(req, file_name);
}

/* ---- 强制门户落地页 = 登录表单(问题与版本优化.md 1.3-01) ----
 * 免认证 200:各 OS 探测(302)与 DHCP 选项 114 都指向这里。原实现是"纯静态页 +
 * meta refresh 跳 /config",但 /config 需要 Basic 认证,而手机强制门户小窗(CNA)
 * 无法可靠完成这条交接(实测:能弹框、能通过,却停在落地页)。现改为表单登录:
 *   POST /api/v1/auth/login → 校验 → Set-Cookie 会话 → 302 /config
 * 落地页本身不读配置、不取锁;凭据校验只发生在 POST 路径 */

/* urlencoded 解码:就地处理 '+' → 空格、%XX → 字节(非法转义原样保留) */
/* urlencoded / 表单字段 / Cookie 解析已移至 main/form_url.c
 * (零 IDF 依赖 -> 可被 tools/form_url/ 的宿主单测直接编译,见 问题与版本优化.md 1.3-01) */

/* 登录页的凭据回显口径(1.3-23 补充;判定逻辑见 `app_http_send_login_page`):
 *   ① 用户名与口令**都**是出厂默认 → 两个框都预填;
 *   ② 只改了用户名            → 用户名照实回显,口令仍填默认值;
 *   ③ 口令改过(无论用户名)    → 用户名照实回显,口令留空(设备只存加盐哈希,无法回显)。
 * 安全说明:门户落地页免认证,但回显的均为**无需保密**的信息(出厂默认值本就写在
 * README/Kconfig 中;自定义用户名虽会被展示,但门户仅 AP/局域网可达),且页面明确提示
 * 尽快修改凭据。校验用单次 SHA-256(非 PBKDF2),每请求一次开销可忽略,不会把免认证
 * 接口变成可被拖慢的入口。 */

/* 把用户可控字符串放进 HTML 之前必须转义(1.3-23 补充)。
 * 现实:改密接口只拒绝了 ':'(`web_auth_put_handler`),**没有**限制 `"` `<` `>` `&`
 * ⇒ 用户名若被直接回显到 `value="…"`,存在属性逃逸/注入面。
 * ⚠️ 踩过的坑:各 HTML 实体长度**并不相同**(&amp;=5、&lt;=4、&gt;=4、&quot;=6、&#39;=5)。
 * 曾按固定 5 字节拷贝 ⇒ 处理 `<` 时把 `&lt;` 之后的 NUL 一并写入,字符串**在此提前终止**
 * (现象:页面只剩 `value="&lt;"`)。必须按 `strlen(rep)` 逐条长度推进。
 * 只处理这 5 个字符,其余字节原样保留以免破坏 UTF-8;调用方按最坏 6 倍备缓冲。 */
static void app_html_escape(const char *in, char *out, size_t out_size)
{
    size_t j = 0;

    if (out_size == 0) {
        return;
    }
    for (size_t i = 0; in != NULL && in[i] != '\0'; i++) {
        const char *rep = NULL;

        switch (in[i]) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default:   break;
        }
        if (rep != NULL) {
            size_t rep_len = strlen(rep);   /* 见上:长度不一致,绝不能写死 */

            if (j + rep_len >= out_size) {
                break;
            }
            memcpy(out + j, rep, rep_len);
            j += rep_len;
        } else {
            if (j + 1 >= out_size) {
                break;
            }
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

/* 登录页:分片发送(head → 可选错误 → 表单尾),避免大栈缓冲与 format-truncation */
static esp_err_t app_http_send_login_page(httpd_req_t *req, const char *error)
{
    static const char page_head[] =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>IotHub</title><style>"
        "body{font-family:sans-serif;margin:0;min-height:100vh;display:flex;align-items:center;"
        "justify-content:center;background:#f2f4f8;color:#1f2933}"
        ".card{background:#fff;border-radius:12px;padding:28px 24px;max-width:320px;width:100%;"
        "box-shadow:0 4px 16px rgba(0,0,0,.08)}"
        "h2{margin:0 0 16px;font-size:20px;text-align:center}"
        "input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;border:1px solid #cbd2d9;"
        "border-radius:8px;font-size:16px}"
        "button{width:100%;margin-top:12px;padding:12px;border:0;border-radius:8px;"
        "background:#2563eb;color:#fff;font-size:16px}"
        ".err{color:#b91c1c;font-size:14px;margin:0 0 8px;text-align:center}"
        ".hint{color:#6b7280;font-size:12px;margin:14px 0 0;text-align:center}"
        "</style></head><body><div class=\"card\"><h2>IotHub 配置</h2>";
    /* 表单改为**运行期构造**(1.3-23 补充):用户名取配置里的**当前实际值**(明文存储,
     * 含用户自定义),HTML 转义后回显;口令仅当**仍是出厂默认**时预填 —— 设备只保存
     * 加盐 SHA-256,自定义口令**无法回显**(见 问题与版本优化.md 1.3-23)。 */
    char username[33];
    char username_html[6 * sizeof(username)];   /* 转义最坏 5 倍膨胀 + NUL */
    char form[768];
    esp_err_t err;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    err = httpd_resp_send_chunk(req, page_head, sizeof(page_head) - 1);
    if (err == ESP_OK && error != NULL) {
        err = httpd_resp_send_chunk(req, error, strlen(error));
    }
    if (err == ESP_OK) {
        int form_len;
        bool password_is_default;
        bool username_is_default;

        app_config_lock();
        app_copy_string(username, sizeof(username), s_config.web_username);
        app_config_unlock();
        app_html_escape(username, username_html, sizeof(username_html));

        /* 分别判定两半:**用户名**是否仍是默认(明文比对,零开销);
         * **口令**是否仍是默认(用"当前用户名 + 默认口令"去校验,单次 SHA-256)。
         * 分开判定才能给出准确提示 —— 否则"只改了用户名"会被说成"口令已自定义"。 */
        username_is_default = (strcmp(username, CONFIG_WEB_DEFAULT_USERNAME) == 0);
        password_is_default = app_http_check_credentials(username, CONFIG_WEB_DEFAULT_PASSWORD);

        if (username_is_default && password_is_default) {
            /* 两半都未改 → 两个框都填好,点一下即可进 */
            form_len = snprintf(form, sizeof(form),
                "<form method=\"post\" action=\"/api/v1/auth/login\">"
                "<input name=\"username\" value=\"%.198s\" autocomplete=\"username\""
                " placeholder=\"用户名\" required>"
                "<input name=\"password\" type=\"password\" value=\"%.64s\""
                " autocomplete=\"current-password\" placeholder=\"口令\" required>"
                "<button type=\"submit\">登 录</button></form>"
                "<p class=\"hint\">已填入<b>出厂默认</b>凭据,直接点「登 录」即可;<br>"
                "建议进入配置页后尽快修改用户名与口令</p>",
                username_html, CONFIG_WEB_DEFAULT_PASSWORD);
        } else if (password_is_default) {
            /* 只改了用户名:用户名照实回显(转义后),口令仍填出厂默认值 */
            form_len = snprintf(form, sizeof(form),
                "<form method=\"post\" action=\"/api/v1/auth/login\">"
                "<input name=\"username\" value=\"%.198s\" autocomplete=\"username\""
                " placeholder=\"用户名\" required>"
                "<input name=\"password\" type=\"password\" value=\"%.64s\""
                " autocomplete=\"current-password\" placeholder=\"口令\" required>"
                "<button type=\"submit\">登 录</button></form>"
                "<p class=\"hint\">用户名已<b>自定义</b>,口令仍是<b>出厂默认</b>(已填入);<br>"
                "建议进入配置页后尽快修改口令</p>",
                username_html, CONFIG_WEB_DEFAULT_PASSWORD);
        } else {
            /* 口令已自定义(与用户名是否自定义无关):口令留空并说明原因 */
            form_len = snprintf(form, sizeof(form),
                "<form method=\"post\" action=\"/api/v1/auth/login\">"
                "<input name=\"username\" value=\"%.198s\" autocomplete=\"username\""
                " placeholder=\"用户名\" required>"
                "<input name=\"password\" type=\"password\""
                " autocomplete=\"current-password\" placeholder=\"口令\" required>"
                "<button type=\"submit\">登 录</button></form>"
                "<p class=\"hint\">口令已自定义(设备只保存哈希,无法回显),请输入原口令</p>",
                username_html);
        }
        if (form_len < 0) {
            form_len = 0;
        } else if ((size_t)form_len >= sizeof(form)) {
            form_len = (int)sizeof(form) - 1;
        }
        err = httpd_resp_send_chunk(req, form, (size_t)form_len);
    }
    if (err == ESP_OK) {
        static const char page_close[] = "</div></body></html>";
        err = httpd_resp_send_chunk(req, page_close, sizeof(page_close) - 1);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);   /* 收尾 */
    }
    return app_http_ignore_client_disconnect(err, "login page");
}

static esp_err_t portal_landing_handler(httpd_req_t *req)
{
    if (!app_http_capacity_ok(req)) {
        return ESP_OK;
    }
    /* 已登录的客户端再被指到落地页(重校验成功前 CNA 反复开 114 选项地址)时,
     * 直接带去 /config —— 但必须有有效会话 Cookie 才能跳,否则无 Cookie 的
     * 系统浏览器会在 /config 与 /portal 之间无限 302 循环 */
    {
        char peer_ip[16];
        char cookie_token[WEB_SESSION_TOKEN_HEX + 1];
        if (app_http_peer_ip(req, peer_ip, sizeof(peer_ip)) &&
            web_portal_ip_authed(peer_ip) &&
            web_cookie_token(req, cookie_token, sizeof(cookie_token)) &&
            web_session_valid(cookie_token)) {
            char target[96];

            app_http_absolute_url(req, APP_WEB_CONFIG_PATH, target, sizeof(target));
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Location", target);
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(req, "Redirect to config");
        }
    }
    return app_http_send_login_page(req, NULL);
}

/* 登录提交:免认证 —— 全站唯一放开认证的 /api 路由(它是"拿凭据"的入口)。
 * 成功:下发会话 Cookie + 302 /config;失败:200 + 登录页(刻意不发 401,
 * 避免门户小窗反复弹 Basic 框) */
static esp_err_t web_login_post_handler(httpd_req_t *req)
{
    char username[33] = "";
    char password[65] = "";
    char token[WEB_SESSION_TOKEN_HEX + 1];
    char cookie[128];
    char *body = app_http_scratch_buf();
    esp_err_t err = http_read_body(req, body, 512);

    if (err != ESP_OK) {
        return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
    }

    /* ⚠️ 顺序很重要:**先切字段、再解码**(`app_form_lookup_raw`)。
     * 旧流程是 `app_form_decode_all()` + `app_form_lookup()` —— 先把整段解码,值里的 `%26`
     * 变成裸 '&',随后按 '&' 切字段就把该值**拦腰截断**(实测用户名含 & 时只收到前半段,
     * user_len=5 → 永远登不进去)。真实用户口令里出现 & 也是同一场景。见 问题与版本优化.md 1.3-23。 */
    if (!app_form_lookup_raw(body, "username", username, sizeof(username)) ||
        !app_form_lookup_raw(body, "password", password, sizeof(password)) ||
        !app_http_check_credentials(username, password)) {
        ESP_LOGW(TAG, "portal login failed (user_len=%u)", (unsigned)strlen(username));
        return app_http_send_login_page(req, "<p class=\"err\">用户名或口令不正确</p>");
    }

    if (web_session_create(token, sizeof(token))) {
        /* 1.3-73:新登录立即接管 —— 旧 token 全部作废,并关闭除本次登录
         * 连接外的所有 HTTP 客户端(旧页面/旧标签页/门户探测连接),只保留
         * 最后一个登录;旧页面下一请求会 401 并被前端重定向回 /portal。 */
        web_session_drop_others(token);
        app_http_close_other_clients(httpd_req_to_sockfd(req));
        snprintf(cookie, sizeof(cookie),
                 WEB_SESSION_COOKIE "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
                 token, (int)(WEB_SESSION_TTL_MS / 1000));
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    }

    /* 登记客户端 IP:手机随后的连通性重校验(不带 Cookie)才能被认作已登录回 204 */
    web_portal_mark_authed(req);

    ESP_LOGI(TAG, "portal login ok -> redirect " APP_WEB_CONFIG_PATH);
    {
        char target[96];

        app_http_absolute_url(req, APP_WEB_CONFIG_PATH, target, sizeof(target));
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", target);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return app_http_ignore_client_disconnect(httpd_resp_sendstr(req, "Login OK"), "login redirect");
}

/* 登出:作废全部会话(本机只服务少量使用者,不做按会话精细注销) */
static esp_err_t web_logout_post_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    web_session_drop_all();
    httpd_resp_set_hdr(req, "Set-Cookie",
                       WEB_SESSION_COOKIE "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
    return app_http_send_json_text(req, NULL, "{\"status\":\"ok\",\"message\":\"logged_out\"}");
}


/* 修改 Web 登录凭据:验旧口令 → 可选改用户名 → 新盐+新口令哈希写入配置。
 * 成功后浏览器缓存的旧 Basic 凭据会 401,由页面提示重新登录。 */
static esp_err_t web_auth_put_handler(httpd_req_t *req)
{
    char old_password[65];
    char new_password[65];
    char new_username[33];
    uint8_t salt[16];
    esp_err_t save_err;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }

    if (!app_json_find_string(app_http_scratch_buf(), "old_password", old_password, sizeof(old_password)) ||
        strlen(old_password) == 0) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"old_password_required\"}");
    }
    if (!app_http_verify_password(old_password)) {
        return app_http_send_json_text(req, "403 Forbidden",
                                   "{\"status\":\"error\",\"message\":\"invalid_old_password\"}");
    }
    /* 口令强度规则(1.3-23):**用户自选**口令须 ≥8 位;但**允许显式设回出厂默认**(生产默认
     * 是 6 位),否则一旦改过口令就再也回不到登录页/文档上写的默认值 —— 与门户页"预填默认
     * 凭据"自相矛盾。默认口令本身属"首启即应修改"范畴,由 1.3-15 量产项统一处理。 */
    if (!app_json_find_string(app_http_scratch_buf(), "new_password", new_password, sizeof(new_password)) ||
        (strlen(new_password) < 8 && strcmp(new_password, CONFIG_WEB_DEFAULT_PASSWORD) != 0)) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"password_too_short\"}");
    }
    if (app_json_find_string(app_http_scratch_buf(), "new_username", new_username, sizeof(new_username)) &&
        strlen(new_username) > 0 &&
        strchr(new_username, ':') != NULL) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_username\"}");
    }

    esp_fill_random(salt, sizeof(salt));
    app_config_lock();
    if (app_json_find_string(app_http_scratch_buf(), "new_username", new_username, sizeof(new_username)) &&
        strlen(new_username) > 0) {
        app_copy_string(s_config.web_username, sizeof(s_config.web_username), new_username);
    }
    memcpy(s_config.web_pass_salt, salt, sizeof(salt));
    app_web_hash_password(salt, sizeof(salt), new_password, s_config.web_pass_hash);
    save_err = app_config_save(&s_config);
    app_config_unlock();

    if (save_err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    ESP_LOGI(TAG, "Web credentials updated via config page");
    /* 凭据已变更:作废全部 Web 会话,强制用新口令重新登录(旧 Cookie 立即失效) */
    web_session_drop_all();
    return app_http_send_json_text(req, NULL,
                               "{\"status\":\"saved\",\"message\":\"web credentials updated, please re-login\"}");
}

void app_http_start_webserver(void)
{
    static bool s_started = false;
    if (s_started) {
        return;
    }
    s_started = true;

    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = APP_HTTP_MAX_HANDLERS;
    /* 1.3-30 C:httpd 任务钉到 core 1 —— 慢客户端阻塞 send 的 2s 窗口不再与
     * core 0 上的 BT 控制器/主机抢 CPU(与 WiFi/tcpip 同核)。 */
    config.core_id = 1;
    /* 1.3-12:两个热接口改为格式串直出(零堆),但 snprintf 家族比 cJSON 更吃栈,
     * 默认 4096 偏紧,抬到 6144 留余量(与"零堆"同一目标:不靠堆换安全)。 */
    config.stack_size = 6144;
    config.backlog_conn = 8;
    config.max_open_sockets = APP_HTTPD_MAX_OPEN_SOCKETS;
    /* 1.3-36:关闭 LRU 踢旧 —— IDF 的 lru_purge 只按"最久未用"关会话,不区分
     * 该会话是否**正在发响应**:高并发(浏览器并行 fetch、手机门户探测、轮询)
     * 时新连接会把在途响应直接掐断,实测 device/status 响应头已发出、1502B
     * 正文全部丢失 → 面板"一直加载/报错"。关掉后槽位满时 httpd 不再 accept,
     * 新连接在 TCP backlog 排队,在途请求永不被杀;空闲会话由自身超时回收。 */
    config.lru_purge_enable = false;
    /* 单任务 httpd:慢客户端(手机经 AP,速率低 + WiFi 省电)收 16KB 页面时会阻塞在
     * send 上,默认 send_wait_timeout=5s 期间**所有其它请求都在排队**
     * (实测:手机连 AP 时 /config 8.0s、1KB 小接口 2.9s → 前端面板"一直加载中")。
     * 1.3-19 收到 2s;1.3-36 又放宽到 5s —— AP+STA 双模下手机在 AP 侧关联/探测
     * 会让 STA 侧空口出现 4-10s 断流,2s 超时会把正在流式发送的页面中途掐断
     * (实测 2845B 后断流、curl 干等 40s),5s 能骑过大部分短断流。 */
    config.send_wait_timeout = 5;
    /* httpd 起不来等于无管理面,保留 abort 重启重试;路由注册失败只降级对应功能组,
     * 不再 abort 整机(无人值守设备缺一组 API 好过无限重启) */
    ESP_ERROR_CHECK(httpd_start(&s_web.server, &config));
    app_http_post_page_init();
    app_http_html_cache_warmup();
    /* 空闲会话收割:lru_purge 关闭后,靠它回收占满槽位的闲置 keep-alive。
     * 1.3-44:低堆下创建可能失败(1536B 栈),失败只损失"闲置收割"
     * (槽位满时新连接在 backlog 排队),不致命 —— 但要留痕,别静默。 */
    if (xTaskCreatePinnedToCore(app_http_reaper_task, "http_reaper", 1536, NULL, 3, NULL,
                                config.core_id) != pdPASS) {
        ESP_LOGW(TAG, "http_reaper task create failed (idle sessions will not be reaped)");
    }
    {
        size_t index;
        const app_http_route_t core_routes[] = {
            {.uri = APP_WEB_PORTAL_PATH,     .method = HTTP_GET, .handler = portal_landing_handler},
            {.uri = "/api/v1/auth/login",    .method = HTTP_POST, .handler = web_login_post_handler},
            {.uri = "/api/v1/auth/logout",   .method = HTTP_POST, .handler = web_logout_post_handler},
            {.uri = "/api/v1/device/info",   .method = HTTP_GET, .handler = device_info_get_handler},
            {.uri = "/api/v1/device/status", .method = HTTP_GET, .handler = device_status_get_handler},
            {.uri = "/api/v1/config/web",    .method = HTTP_PUT, .handler = web_auth_put_handler},
        };
        for (index = 0; index < sizeof(core_routes) / sizeof(core_routes[0]); index++) {
            esp_err_t err = app_http_register_uri_handler(s_web.server,
                                                          core_routes[index].uri,
                                                          core_routes[index].method,
                                                          core_routes[index].handler);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "register %s failed: %s (route unavailable)",
                         core_routes[index].uri, esp_err_to_name(err));
            }
        }
    }

    {
        size_t index;
        static const struct {
            const char *name;
            esp_err_t (*register_fn)(httpd_handle_t);
        } route_groups[] = {
            {"network", http_network_register_routes},
            {"io",      http_io_register_routes},
            {"mqtt",    http_mqtt_register_routes},
            {"uart",    http_uart_register_routes},
            {"ota",     http_ota_register_routes},
        };
        for (index = 0; index < sizeof(route_groups) / sizeof(route_groups[0]); index++) {
            esp_err_t err = route_groups[index].register_fn(s_web.server);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "register %s routes failed: %s (group unavailable)",
                         route_groups[index].name, esp_err_to_name(err));
            }
        }
    }

    {
        esp_err_t err = app_http_register_uri_handler(s_web.server,
                                                      "/*",
                                                      HTTP_GET,
                                                      index_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register catch-all route failed: %s (web pages unavailable)",
                     esp_err_to_name(err));
        }
    }
}
