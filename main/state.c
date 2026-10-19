#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#if CONFIG_INPUT_IS_TOUCH
#include "driver/touch_sens.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cmd.h"
#include "config.h"
#include "constants.h"
#include "mqtt.h"
#include "state.h"

static const char *TAG = "state";

_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_RELAY_GPIO),
               "CONFIG_RELAY_GPIO must be an output-capable GPIO (not GPIO34-39)");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_LED_GPIO),
               "CONFIG_LED_GPIO must be an output-capable GPIO (not GPIO34-39)");
_Static_assert(GPIO_IS_VALID_GPIO(CONFIG_INPUT_GPIO),
               "CONFIG_INPUT_GPIO must be a valid GPIO");

extern app_config_t s_config;

#define APP_INPUT_POLL_MS 20
#define APP_INPUT_STABLE_SAMPLES 3

static app_io_state_t s_io_state;
static SemaphoreHandle_t s_io_lock;
static StaticSemaphore_t s_io_lock_buf;
static portMUX_TYPE s_io_state_mux = portMUX_INITIALIZER_UNLOCKED;

static app_debounce_t s_input_debounce;
static esp_timer_handle_t s_input_timer;

#if CONFIG_INPUT_IS_TOUCH
static touch_sensor_handle_t s_touch_sens;
static touch_channel_handle_t s_touch_chan;
static uint32_t s_touch_threshold;
static bool s_touch_ready;
#endif

gpio_num_t app_relay_gpio(void)
{
    return RELAY_DEFAULT_GPIO;
}

gpio_num_t app_led_gpio(void)
{
    return LED_DEFAULT_GPIO;
}

gpio_num_t app_input_gpio(void)
{
    return INPUT_DEFAULT_GPIO;
}

gpio_num_t app_uart_rx_gpio(void)
{
    return UART_RX_GPIO;
}

gpio_num_t app_uart_tx_gpio(void)
{
    return UART_TX_GPIO;
}

const char *app_io_source_name(app_io_source_t source)
{
    switch (source) {
    case APP_IO_SOURCE_BOOT:
        return "boot";
    case APP_IO_SOURCE_COMMAND:
        return "command";
    case APP_IO_SOURCE_HTTP:
        return "http";
    case APP_IO_SOURCE_INPUT:
        return "input";
    case APP_IO_SOURCE_MQTT:
        return "mqtt";
    case APP_IO_SOURCE_BLE:
        return "ble";
    default:
        return "unknown";
    }
}

esp_err_t app_apply_output(gpio_num_t gpio_num, bool active_high, bool on)
{
    return gpio_set_level(gpio_num, (active_high ? on : !on) ? 1 : 0);
}

bool app_debounce_update(app_debounce_t *db, bool sample, uint8_t required)
{
    if (db == NULL) {
        return false;
    }
    if (required == 0) {
        required = 1;
    }
    if (db->count == 0 || sample != db->candidate) {
        db->candidate = sample;
        db->count = 1;
    } else if (db->count < 255u) {
        db->count++;
    }
    if (db->count < required || db->stable == sample) {
        return false;
    }
    db->stable = sample;
    return true;
}

static bool app_input_read_sample(void)
{
#if CONFIG_INPUT_IS_TOUCH
    uint32_t smooth[TOUCH_SAMPLE_CFG_NUM] = {0};

    if (!s_touch_ready ||
        touch_channel_read_data(s_touch_chan, TOUCH_CHAN_DATA_TYPE_SMOOTH, smooth) != ESP_OK) {
        return s_input_debounce.stable;
    }
    /* ESP32 touch V1: 触摸使 smooth 值下降,低于绝对阈值判定为按下。 */
    return smooth[0] <= s_touch_threshold;
#else
    int raw = gpio_get_level(app_input_gpio());
    return s_config.input_active_low ? (raw == 0) : (raw != 0);
#endif
}

static bool app_input_toggle_led_via_command(void)
{
#if CONFIG_INPUT_IS_TOUCH && CONFIG_INPUT_TOUCH_TOGGLES_LED
    cJSON *frame = cJSON_CreateObject();
    cJSON *resp = NULL;
    app_cmd_result_t out = {0};
    const char *err = NULL;
    bool ok;

    if (frame == NULL) {
        return false;
    }
    cJSON_AddStringToObject(frame, "method", "led.toggle");
    cJSON_AddObjectToObject(frame, "params");
    ok = (app_cmd_execute_ex(frame, APP_CMD_SOURCE_TOUCH, &resp, &out, &err) == ESP_OK);
    if (!ok) {
        ESP_LOGW(TAG, "touch led.toggle failed: %s", err != NULL ? err : "unknown");
    }
    cJSON_Delete(resp);
    cJSON_Delete(frame);
    return ok;
#else
    return false;
#endif
}

static void app_input_timer_cb(void *arg)
{
    bool sample;
    bool was_active = false;
    bool changed = false;

    (void)arg;
    sample = app_input_read_sample();
    if (!app_debounce_update(&s_input_debounce, sample, APP_INPUT_STABLE_SAMPLES)) {
        return;
    }
    portENTER_CRITICAL(&s_io_state_mux);
    if (s_io_state.input_active != sample) {
        was_active = s_io_state.input_active;
        s_io_state.input_active = sample;
        s_io_state.seq++;
        s_io_state.last_change_ms = esp_timer_get_time() / 1000;
        s_io_state.last_source = APP_IO_SOURCE_INPUT;
        changed = true;
    }
    portEXIT_CRITICAL(&s_io_state_mux);
    if (changed) {
        ESP_LOGI(TAG, "input %s (seq=%u)", sample ? "active" : "inactive",
                 (unsigned)s_io_state.seq);
        if (!(sample && !was_active && app_input_toggle_led_via_command())) {
            /* 触摸命令成功时内部已上报;其它输入/失败在这里补一次上报。 */
            app_mqtt_on_io_change();
        }
    }
}

static void app_input_init_state(void)
{
    bool sample = app_input_read_sample();

    s_input_debounce.stable = sample;
    s_input_debounce.candidate = sample;
    s_input_debounce.count = 0;
    portENTER_CRITICAL(&s_io_state_mux);
    s_io_state.input_active = sample;
    portEXIT_CRITICAL(&s_io_state_mux);
    ESP_LOGI(TAG, "input initial state: %s", sample ? "active" : "inactive");
}

static void app_input_start_timer(void)
{
    const esp_timer_create_args_t args = {
        .callback = app_input_timer_cb,
        .name = "input_poll",
    };

    if (s_input_timer == NULL &&
        esp_timer_create(&args, &s_input_timer) != ESP_OK) {
        ESP_LOGE(TAG, "input poll timer create failed");
        return;
    }
    if (esp_timer_start_periodic(s_input_timer, APP_INPUT_POLL_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "input poll timer start failed");
    }
}

#if CONFIG_INPUT_IS_TOUCH
static esp_err_t app_touch_input_init(void)
{
    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7)
    };
    touch_sensor_config_t sens_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample_cfg);
    touch_channel_config_t chan_cfg = {
        .abs_active_thresh = {1000},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    touch_chan_info_t info = {0};
    uint32_t smooth[TOUCH_SAMPLE_CFG_NUM] = {0};
    esp_err_t err;
    int i;

    err = touch_sensor_new_controller(&sens_cfg, &s_touch_sens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch controller init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = touch_sensor_new_channel(s_touch_sens, CONFIG_INPUT_TOUCH_CHANNEL, &chan_cfg,
                                   &s_touch_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch channel %d init failed: %s",
                 CONFIG_INPUT_TOUCH_CHANNEL, esp_err_to_name(err));
        return err;
    }
    if (touch_sensor_get_channel_info(s_touch_chan, &info) == ESP_OK) {
        ESP_LOGI(TAG, "touch channel %d -> GPIO%d (expected GPIO%d)",
                 info.chan_id, info.chan_gpio, (int)app_input_gpio());
        if (info.chan_gpio != (int)app_input_gpio()) {
            ESP_LOGW(TAG, "touch channel/GPIO mismatch: check Kconfig");
        }
    }
    err = touch_sensor_config_filter(s_touch_sens, &filter_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch filter init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = touch_sensor_enable(s_touch_sens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch enable failed: %s", esp_err_to_name(err));
        return err;
    }
    for (i = 0; i < 5; i++) {
        err = touch_sensor_trigger_oneshot_scanning(s_touch_sens, 2000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch initial scan %d failed: %s", i, esp_err_to_name(err));
        }
    }
    err = touch_sensor_disable(s_touch_sens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch disable after calibration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = touch_channel_read_data(s_touch_chan, TOUCH_CHAN_DATA_TYPE_SMOOTH, smooth);
    if (err == ESP_OK && smooth[0] > 0) {
        s_touch_threshold = (uint32_t)((uint64_t)smooth[0] *
                                      (100u - CONFIG_INPUT_TOUCH_ACTIVE_PERCENT) / 100u);
        if (s_touch_threshold == 0) {
            s_touch_threshold = 1;
        }
    } else {
        s_touch_threshold = 1000;
        ESP_LOGW(TAG, "touch baseline invalid (err=%s smooth=%u), use threshold=%u",
                 esp_err_to_name(err), (unsigned)smooth[0], (unsigned)s_touch_threshold);
    }
    chan_cfg.abs_active_thresh[0] = s_touch_threshold;
    err = touch_sensor_reconfig_channel(s_touch_chan, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch threshold apply failed: %s", esp_err_to_name(err));
        return err;
    }
    err = touch_sensor_enable(s_touch_sens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch re-enable failed: %s", esp_err_to_name(err));
        return err;
    }
    err = touch_sensor_start_continuous_scanning(s_touch_sens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch continuous scan start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_touch_ready = true;
    ESP_LOGI(TAG, "touch ready: channel=%d baseline=%u threshold=%u (%u%%)",
             CONFIG_INPUT_TOUCH_CHANNEL, (unsigned)smooth[0], (unsigned)s_touch_threshold,
             CONFIG_INPUT_TOUCH_ACTIVE_PERCENT);
    return ESP_OK;
}
#endif

bool app_get_input_state(void)
{
    return s_input_debounce.stable;
}

static void app_io_ensure_lock(void)
{
    if (s_io_lock == NULL) {
        s_io_lock = xSemaphoreCreateMutexStatic(&s_io_lock_buf);
    }
}

static uint8_t app_io_policy(bool relay)
{
    uint8_t policy;

    app_config_lock();
    policy = relay ? s_config.relay_power_policy : s_config.led_power_policy;
    app_config_unlock();
    return policy <= APP_GPIO_POWER_RESTORE ? policy : APP_GPIO_POWER_FIXED_OFF;
}

static bool app_io_restore_last(bool relay)
{
    bool on = false;

    if (!app_config_load_output_last(relay, &on)) {
        ESP_LOGW(TAG, "%s state missing/unreadable, fallback off", relay ? "relay" : "led");
        return false;
    }
    return on;
}

static void app_io_boot_apply(void)
{
    uint8_t relay_policy = app_io_policy(true);
    uint8_t led_policy = app_io_policy(false);
    bool relay_on = false;
    bool led_on = false;

    switch (relay_policy) {
    case APP_GPIO_POWER_FIXED_ON:
        relay_on = true;
        break;
    case APP_GPIO_POWER_RESTORE:
        relay_on = app_io_restore_last(true);
        break;
    case APP_GPIO_POWER_FIXED_OFF:
    default:
        relay_on = false;
        break;
    }
    switch (led_policy) {
    case APP_GPIO_POWER_FIXED_ON:
        led_on = true;
        break;
    case APP_GPIO_POWER_RESTORE:
        led_on = app_io_restore_last(false);
        break;
    case APP_GPIO_POWER_FIXED_OFF:
    default:
        led_on = false;
        break;
    }

    if (app_apply_output(app_relay_gpio(), s_config.relay_active_high, relay_on) != ESP_OK) {
        ESP_LOGE(TAG, "relay boot apply failed, force off");
        relay_on = false;
        (void)app_apply_output(app_relay_gpio(), s_config.relay_active_high, false);
    }
    if (app_apply_output(app_led_gpio(), s_config.led_active_high, led_on) != ESP_OK) {
        ESP_LOGE(TAG, "led boot apply failed, force off");
        led_on = false;
        (void)app_apply_output(app_led_gpio(), s_config.led_active_high, false);
    }

    portENTER_CRITICAL(&s_io_state_mux);
    s_io_state.relay_on = relay_on;
    s_io_state.led_on = led_on;
    s_io_state.input_active = false;
    s_io_state.seq = 0;
    s_io_state.last_change_ms = 0;
    s_io_state.last_source = APP_IO_SOURCE_BOOT;
    portEXIT_CRITICAL(&s_io_state_mux);

    ESP_LOGI(TAG, "boot policy: relay=%u(%s) led=%u(%s), outputs relay=%d led=%d",
             (unsigned)relay_policy,
             relay_policy == APP_GPIO_POWER_FIXED_OFF ? "off" :
             relay_policy == APP_GPIO_POWER_FIXED_ON ? "on" : "restore",
             (unsigned)led_policy,
             led_policy == APP_GPIO_POWER_FIXED_OFF ? "off" :
             led_policy == APP_GPIO_POWER_FIXED_ON ? "on" : "restore",
             (int)relay_on, (int)led_on);
}

void app_io_get_state(app_io_state_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_io_state_mux);
    *out = s_io_state;
    portEXIT_CRITICAL(&s_io_state_mux);
}

esp_err_t app_io_set_outputs(bool set_relay, bool relay_on,
                             bool set_led, bool led_on,
                             app_io_source_t source)
{
    bool new_relay;
    bool new_led;
    bool changed_relay;
    bool changed_led;
    uint8_t relay_policy;
    uint8_t led_policy;

    app_io_ensure_lock();
    if (s_io_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_io_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    new_relay = set_relay ? relay_on : s_io_state.relay_on;
    new_led = set_led ? led_on : s_io_state.led_on;
    changed_relay = (new_relay != s_io_state.relay_on);
    changed_led = (new_led != s_io_state.led_on);

    if (!changed_relay && !changed_led) {
        xSemaphoreGive(s_io_lock);
        return ESP_OK;
    }

    relay_policy = app_io_policy(true);
    led_policy = app_io_policy(false);

    if (changed_relay &&
        app_apply_output(app_relay_gpio(), s_config.relay_active_high, new_relay) != ESP_OK) {
        ESP_LOGE(TAG, "relay apply failed");
        xSemaphoreGive(s_io_lock);
        return ESP_FAIL;
    }
    if (changed_led &&
        app_apply_output(app_led_gpio(), s_config.led_active_high, new_led) != ESP_OK) {
        ESP_LOGE(TAG, "led apply failed");
        if (changed_relay) {
            (void)app_apply_output(app_relay_gpio(), s_config.relay_active_high,
                                   s_io_state.relay_on);
        }
        xSemaphoreGive(s_io_lock);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_io_state_mux);
    s_io_state.relay_on = new_relay;
    s_io_state.led_on = new_led;
    s_io_state.seq++;
    s_io_state.last_change_ms = esp_timer_get_time() / 1000;
    s_io_state.last_source = source;
    portEXIT_CRITICAL(&s_io_state_mux);

    if (changed_relay && relay_policy == APP_GPIO_POWER_RESTORE) {
        (void)app_config_save_output_last(true, new_relay);
    }
    if (changed_led && led_policy == APP_GPIO_POWER_RESTORE) {
        (void)app_config_save_output_last(false, new_led);
    }

    ESP_LOGI(TAG, "outputs relay=%d led=%d seq=%u source=%s",
             (int)new_relay, (int)new_led, (unsigned)s_io_state.seq,
             app_io_source_name(source));
    xSemaphoreGive(s_io_lock);
    app_mqtt_on_io_change();
    return ESP_OK;
}

void app_io_apply_policies_at_runtime(void)
{
    app_io_state_t state;
    uint8_t relay_policy = app_io_policy(true);
    uint8_t led_policy = app_io_policy(false);
    bool target_relay;
    bool target_led;
    bool set_relay = false;
    bool set_led = false;

    app_io_get_state(&state);
    target_relay = state.relay_on;
    target_led = state.led_on;

    if (relay_policy == APP_GPIO_POWER_FIXED_OFF) {
        target_relay = false;
        set_relay = true;
    } else if (relay_policy == APP_GPIO_POWER_FIXED_ON) {
        target_relay = true;
        set_relay = true;
    }
    if (led_policy == APP_GPIO_POWER_FIXED_OFF) {
        target_led = false;
        set_led = true;
    } else if (led_policy == APP_GPIO_POWER_FIXED_ON) {
        target_led = true;
        set_led = true;
    }

    if (set_relay || set_led) {
        (void)app_io_set_outputs(set_relay, target_relay, set_led, target_led,
                                 APP_IO_SOURCE_COMMAND);
    }
    app_io_get_state(&state);
    if (relay_policy == APP_GPIO_POWER_RESTORE) {
        (void)app_config_save_output_last(true, state.relay_on);
    }
    if (led_policy == APP_GPIO_POWER_RESTORE) {
        (void)app_config_save_output_last(false, state.led_on);
    }
}

void app_configure_gpio(void)
{
    const gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << app_relay_gpio()) | (1ULL << app_led_gpio()),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
#if !CONFIG_INPUT_IS_TOUCH
    const gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << app_input_gpio()),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = app_input_gpio() >= GPIO_NUM_34 ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
#endif
    const gpio_config_t boot_button_cfg = {
        .pin_bit_mask = (1ULL << APP_FACTORY_RESET_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_cfg));
#if !CONFIG_INPUT_IS_TOUCH
    ESP_ERROR_CHECK(gpio_config(&input_cfg));
#endif
    ESP_ERROR_CHECK(gpio_config(&boot_button_cfg));

    app_io_ensure_lock();
    app_io_boot_apply();
#if CONFIG_INPUT_IS_TOUCH
    if (app_touch_input_init() != ESP_OK) {
        ESP_LOGE(TAG, "touch input disabled after init failure");
    }
#endif
    app_input_init_state();
    app_input_start_timer();
}
