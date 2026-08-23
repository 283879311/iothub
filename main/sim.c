#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "common.h"
#include "http_utils.h"
#include "sim.h"
#include "uart.h"
#include "uart_internal.h"

#define SIM_PROTOCOL_SIZE 8
#define SIM_PHASE_SIZE 24
#define SIM_STATUS_SIZE 4
#define SIM_TARGET_TEXT_SIZE 32
#define SIM_WEIGHT_TEXT_SIZE 24
#define SIM_TARGET_WEIGHT_MIN 1000.0
#define SIM_TARGET_WEIGHT_MAX 100000.0

typedef struct {
    char phase[SIM_PHASE_SIZE];
    char status[SIM_STATUS_SIZE];
    double weight;
} app_sim_frame_t;

typedef struct {
    bool active;
    bool exit_waiting;
    bool exit_requested;
    bool allow_decimal;
    uint16_t decimal_places;
    uint16_t weight_step_kg;
    uint32_t speed_kmh;
    uint32_t interval_ms;
    uint32_t send_count;
    int64_t started_at_ms;
    int64_t auto_exit_at_ms;
    int64_t countdown_ends_at_ms;
    double target_weight;
    double current_weight;
    char protocol[SIM_PROTOCOL_SIZE];
    char phase[SIM_PHASE_SIZE];
    char target_weight_text[SIM_TARGET_TEXT_SIZE];
    app_sim_frame_t *frames;
    size_t frame_count;
    size_t frame_index;
    app_sim_frame_t *exit_frames;
    size_t exit_frame_count;
    size_t exit_frame_index;
} app_sim_runtime_t;

typedef struct {
    double target_weight;
    bool allow_decimal;
    uint16_t decimal_places;
    uint16_t weight_step_kg;
    uint32_t speed_kmh;
    uint32_t interval_ms;
    char protocol[SIM_PROTOCOL_SIZE];
    char target_weight_text[SIM_TARGET_TEXT_SIZE];
} app_sim_request_t;

static const char *TAG = "sim";

static app_sim_runtime_t s_sim = {0};
static SemaphoreHandle_t s_sim_mutex = NULL;
static TaskHandle_t s_sim_task = NULL;

static double app_sim_clamp(double value, double min_value, double max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static double app_sim_random_unit(void)
{
    return (double)esp_random() / (double)UINT32_MAX;
}

static double app_sim_random_between(double min_value, double max_value)
{
    return min_value + app_sim_random_unit() * (max_value - min_value);
}

static double app_sim_ease_in_out_quad(double progress)
{
    if (progress < 0.5) {
        return 2.0 * progress * progress;
    }
    return 1.0 - pow(-2.0 * progress + 2.0, 2.0) / 2.0;
}

static uint32_t app_sim_frame_count_from_duration(double duration_ms, uint32_t interval_ms, uint32_t minimum)
{
    uint32_t frames = (uint32_t)llround(duration_ms / (double)interval_ms);
    return frames > minimum ? frames : minimum;
}

static bool app_sim_is_valid_target_weight_text(const char *value, uint32_t expected_weight)
{
    char *end = NULL;
    unsigned long parsed = 0;
    size_t index;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (index = 0; value[index] != '\0'; index++) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }

    parsed = strtoul(value, &end, 10);
    return end != NULL && *end == '\0' && parsed == (unsigned long)expected_weight;
}

static double app_sim_round_to_places(double value, uint16_t decimal_places)
{
    double factor = pow(10.0, (double)decimal_places);
    return round(value * factor) / factor;
}

static double app_sim_normalize_output_weight(const app_sim_request_t *request, double value)
{
    if (strcmp(request->protocol, "ds10") == 0 && request->allow_decimal) {
        return app_sim_round_to_places(value, request->decimal_places);
    }
    if (request->allow_decimal) {
        return app_sim_round_to_places(value, 1);
    }
    if (request->weight_step_kg == 0) {
        return round(value);
    }
    return round(value / (double)request->weight_step_kg) * (double)request->weight_step_kg;
}

static void app_sim_format_weight_text(const app_sim_request_t *request, double value, char *buffer, size_t buffer_size)
{
    double normalized = app_sim_normalize_output_weight(request, value);

    if (!request->allow_decimal) {
        snprintf(buffer, buffer_size, "%.0f", normalized);
        return;
    }

    if (strcmp(request->protocol, "ds10") == 0) {
        if (request->decimal_places > 0) {
            snprintf(buffer, buffer_size, "%.*f", (int)request->decimal_places, normalized);
        } else {
            snprintf(buffer, buffer_size, "%.0f", round(normalized));
        }
        return;
    }

    snprintf(buffer, buffer_size, "%.1f", normalized);
}

static void app_sim_build_payload(const app_sim_request_t *request,
                                  const char *status,
                                  double weight,
                                  char *encoding,
                                  size_t encoding_size,
                                  char *data,
                                  size_t data_size)
{
    char weight_text[SIM_WEIGHT_TEXT_SIZE];

    app_sim_format_weight_text(request, weight, weight_text, sizeof(weight_text));
    if (strcmp(request->protocol, "ds10") == 0) {
        uint8_t raw[12];
        char xor_source[9];
        char checksum_text[3];
        char sign_char = weight < 0 ? '-' : '+';
        double normalized = fabs(app_sim_normalize_output_weight(request, weight));
        unsigned scaled = (unsigned)llround(normalized * pow(10.0, (double)request->decimal_places));
        char digits[7];
        unsigned index;
        uint8_t checksum = 0;

        snprintf(digits, sizeof(digits), "%06u", scaled % 1000000U);
        snprintf(xor_source, sizeof(xor_source), "%c%s%1u", sign_char, digits, (unsigned)(request->decimal_places % 10));
        for (index = 0; index < strlen(xor_source); index++) {
            checksum ^= (uint8_t)xor_source[index];
        }
        snprintf(checksum_text, sizeof(checksum_text), "%02X", checksum);

        raw[0] = 0x02;
        memcpy(&raw[1], xor_source, 8);
        raw[9] = (uint8_t)checksum_text[0];
        raw[10] = (uint8_t)checksum_text[1];
        raw[11] = 0x03;

        app_copy_string(encoding, encoding_size, "hex");
        app_format_hex_bytes(raw, sizeof(raw), data, data_size, ' ');
        return;
    }

    app_copy_string(encoding, encoding_size, "plain");
    snprintf(data, data_size, "%s,NT,%8s kg\r\n", status, weight_text);
}

static bool app_sim_append_frame(app_sim_frame_t *frames, size_t capacity, size_t *count,
                                 const char *phase, const char *status, double weight)
{
    if (*count >= capacity) {
        return false;
    }

    app_copy_string(frames[*count].phase, sizeof(frames[*count].phase), phase);
    app_copy_string(frames[*count].status, sizeof(frames[*count].status), status);
    frames[*count].weight = weight < 0 ? 0 : weight;
    *count += 1;
    return true;
}

static bool app_sim_build_vehicle_frames(const app_sim_request_t *request,
                                         app_sim_frame_t **out_frames,
                                         size_t *out_count,
                                         app_sim_frame_t **out_exit_frames,
                                         size_t *out_exit_count)
{
    const uint32_t minimum_front = 2;
    const uint32_t minimum_ramp = 20;
    const uint32_t minimum_oscillation = 8;
    const uint32_t minimum_settle = 6;
    const uint32_t minimum_hold = 12;
    uint32_t total_frames = app_sim_frame_count_from_duration(
        app_sim_random_between(20000.0, 50000.0), request->interval_ms, 20);
    uint32_t front_frames = minimum_front;
    uint32_t ramp_frames = minimum_ramp;
    uint32_t oscillation_frames = minimum_oscillation;
    uint32_t settle_frames = minimum_settle;
    uint32_t hold_frames = minimum_hold;
    uint32_t minimum_total = front_frames + ramp_frames + oscillation_frames + settle_frames + hold_frames;
    uint32_t extra_frames = total_frames > minimum_total ? total_frames - minimum_total : 0;
    size_t count = 0;
    size_t exit_count = 0;
    size_t capacity;
    app_sim_frame_t *frames;
    app_sim_frame_t *exit_frames;
    double speed_factor = app_sim_clamp((double)request->speed_kmh / 8.0, 0.35, 1.8);
    double front_ratio = app_sim_random_between(0.32, 0.38);
    double front_weight = request->target_weight * front_ratio;
    double approach_weight = request->target_weight * app_sim_random_between(0.95, 0.985);
    uint32_t index;

    front_frames += extra_frames * 3 / 73;
    ramp_frames += extra_frames * 32 / 73;
    oscillation_frames += extra_frames * 10 / 73;
    settle_frames += extra_frames * 8 / 73;
    hold_frames += extra_frames * 20 / 73;
    while ((front_frames + ramp_frames + oscillation_frames + settle_frames + hold_frames) < total_frames) {
        ramp_frames += 1;
    }

    capacity = front_frames + ramp_frames + oscillation_frames + settle_frames + hold_frames;
    frames = calloc(capacity, sizeof(app_sim_frame_t));
    if (frames == NULL) {
        return false;
    }

    for (index = 0; index < front_frames; index++) {
        double micro_shift = index == 0 ? 0.0 : app_sim_random_between(-request->target_weight * 0.002,
                                                                        request->target_weight * 0.002);
        if (!app_sim_append_frame(frames, capacity, &count, "front_axle", "US", front_weight + micro_shift)) {
            free(frames);
            return false;
        }
    }

    for (index = 1; index <= ramp_frames; index++) {
        double progress = (double)index / (double)ramp_frames;
        double eased = app_sim_ease_in_out_quad(progress);
        double weight = front_weight + (approach_weight - front_weight) * eased;
        if (!app_sim_append_frame(frames, capacity, &count, "ramp_up", "US", weight)) {
            free(frames);
            return false;
        }
    }

    for (index = 0; index < oscillation_frames; index++) {
        double progress = (double)index / (double)(oscillation_frames > 1 ? (oscillation_frames - 1) : 1);
        double decay = 1.0 - progress * 0.65;
        double amplitude = (request->target_weight * (0.01 + 0.02 * speed_factor)) * decay;
        double wave = sin(index * (1.1 + 0.22 * speed_factor)) * amplitude;
        double noise = app_sim_random_between(-amplitude * 0.18, amplitude * 0.18);
        if (!app_sim_append_frame(frames, capacity, &count, "oscillation", "US", request->target_weight + wave + noise)) {
            free(frames);
            return false;
        }
    }

    {
        double oscillation_end_weight = count > 0 ? frames[count - 1].weight : request->target_weight;
        for (index = 1; index <= settle_frames; index++) {
            double progress = (double)index / (double)settle_frames;
            double eased = 1.0 - pow(1.0 - progress, 3.0);
            double weight = oscillation_end_weight + (request->target_weight - oscillation_end_weight) * eased;
            if (!app_sim_append_frame(frames, capacity, &count, "settled_with_driver",
                                      index < settle_frames ? "US" : "ST", weight)) {
                free(frames);
                return false;
            }
        }
    }

    for (index = 0; index < hold_frames; index++) {
        if (!app_sim_append_frame(frames, capacity, &count, "settled_with_driver", "ST", request->target_weight)) {
            free(frames);
            return false;
        }
    }

    {
        uint32_t exit_total_frames = app_sim_frame_count_from_duration(
            app_sim_random_between(20000.0, 50000.0), request->interval_ms, 20);
        uint32_t zero_hold_frames = exit_total_frames * 15 / 100;
        uint32_t move_frames;
        uint32_t exit_front_frames;
        uint32_t exit_ramp_frames;
        size_t exit_capacity;

        if (zero_hold_frames < 3) {
            zero_hold_frames = 3;
        }
        move_frames = exit_total_frames > zero_hold_frames ? exit_total_frames - zero_hold_frames : 2;
        exit_front_frames = move_frames * front_frames / (front_frames + ramp_frames);
        if (exit_front_frames < 1) {
            exit_front_frames = 1;
        }
        exit_ramp_frames = move_frames > exit_front_frames ? move_frames - exit_front_frames : 1;
        exit_capacity = exit_ramp_frames + exit_front_frames + zero_hold_frames;
        exit_frames = calloc(exit_capacity, sizeof(app_sim_frame_t));
        if (exit_frames == NULL) {
            free(frames);
            return false;
        }

        for (index = 1; index <= exit_ramp_frames; index++) {
            double progress = (double)index / (double)exit_ramp_frames;
            double eased = app_sim_ease_in_out_quad(progress);
            double weight = request->target_weight - (request->target_weight - front_weight) * eased;
            if (!app_sim_append_frame(exit_frames, exit_capacity, &exit_count, "ramp_down", "US", weight)) {
                free(frames);
                free(exit_frames);
                return false;
            }
        }

        for (index = 1; index <= exit_front_frames; index++) {
            double progress = (double)index / (double)exit_front_frames;
            double micro_shift = app_sim_random_between(-request->target_weight * 0.002,
                                                        request->target_weight * 0.002);
            double weight = front_weight * (1.0 - progress) + micro_shift;
            if (!app_sim_append_frame(exit_frames, exit_capacity, &exit_count, "ramp_down",
                                      index < exit_front_frames ? "US" : "ST", weight > 0 ? weight : 0)) {
                free(frames);
                free(exit_frames);
                return false;
            }
        }

        for (index = 0; index < zero_hold_frames; index++) {
            if (!app_sim_append_frame(exit_frames, exit_capacity, &exit_count, "final_stable", "ST", 0)) {
                free(frames);
                free(exit_frames);
                return false;
            }
        }
    }

    *out_frames = frames;
    *out_count = count;
    *out_exit_frames = exit_frames;
    *out_exit_count = exit_count;
    return true;
}

static void app_sim_free_frames_locked(void)
{
    free(s_sim.frames);
    free(s_sim.exit_frames);
    s_sim.frames = NULL;
    s_sim.exit_frames = NULL;
    s_sim.frame_count = 0;
    s_sim.exit_frame_count = 0;
    s_sim.frame_index = 0;
    s_sim.exit_frame_index = 0;
}

static void app_sim_reset_locked(void)
{
    app_sim_free_frames_locked();
    memset(&s_sim, 0, sizeof(s_sim));
}

static void app_sim_notify_task(void)
{
    if (s_sim_task != NULL) {
        xTaskNotifyGive(s_sim_task);
    }
}

static bool app_sim_lock(TickType_t wait_ticks)
{
    if (s_sim_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_sim_mutex, wait_ticks) == pdTRUE;
}

static void app_sim_unlock(void)
{
    if (s_sim_mutex != NULL) {
        xSemaphoreGive(s_sim_mutex);
    }
}

static void app_sim_set_phase_locked(const char *phase, double weight)
{
    app_copy_string(s_sim.phase, sizeof(s_sim.phase), phase);
    s_sim.current_weight = weight < 0 ? 0 : weight;
}

static void app_sim_set_countdown_locked(int64_t ends_at_ms)
{
    s_sim.countdown_ends_at_ms = ends_at_ms > 0 ? ends_at_ms : 0;
}

static void app_sim_send_one_frame(const app_sim_request_t *request, const char *phase, const char *status, double weight)
{
    char encoding[8];
    char data[UART_TX_BUFFER_SIZE];
    unsigned ignored = 0;
    esp_err_t err;

    app_sim_build_payload(request, status, weight, encoding, sizeof(encoding), data, sizeof(data));
    err = app_uart_send_data(encoding, data, &ignored);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "sim send failed for %s: %s", phase, esp_err_to_name(err));
    }
}

static void app_sim_fill_request_from_runtime(app_sim_request_t *request)
{
    memset(request, 0, sizeof(*request));
    request->target_weight = s_sim.target_weight;
    request->allow_decimal = s_sim.allow_decimal;
    request->decimal_places = s_sim.decimal_places;
    request->weight_step_kg = s_sim.weight_step_kg;
    request->speed_kmh = s_sim.speed_kmh;
    request->interval_ms = s_sim.interval_ms;
    app_copy_string(request->protocol, sizeof(request->protocol), s_sim.protocol);
    app_copy_string(request->target_weight_text, sizeof(request->target_weight_text), s_sim.target_weight_text);
}

static void app_sim_task_main(void *arg)
{
    app_sim_request_t request;
    TickType_t next_wait = portMAX_DELAY;

    (void)arg;

    while (true) {
        if (ulTaskNotifyTake(pdTRUE, next_wait) > 0) {
            next_wait = 0;
            continue;
        }

        if (!app_sim_lock(pdMS_TO_TICKS(50))) {
            next_wait = pdMS_TO_TICKS(50);
            continue;
        }

        if (!s_sim.active) {
            app_sim_unlock();
            next_wait = portMAX_DELAY;
            continue;
        }

        app_sim_fill_request_from_runtime(&request);
        if (s_sim.frame_index < s_sim.frame_count) {
            app_sim_frame_t frame = s_sim.frames[s_sim.frame_index++];
            bool just_completed_main = s_sim.frame_index >= s_sim.frame_count;

            app_sim_set_phase_locked(frame.phase, frame.weight);
            s_sim.send_count += 1;
            app_sim_unlock();
            app_sim_send_one_frame(&request, frame.phase, frame.status, frame.weight);

            if (just_completed_main) {
                if (app_sim_lock(pdMS_TO_TICKS(50))) {
                    s_sim.exit_waiting = true;
                    s_sim.exit_requested = false;
                    s_sim.auto_exit_at_ms = (esp_timer_get_time() / 1000) + (int64_t)app_sim_random_between(180000.0, 300000.0);
                    app_sim_set_countdown_locked(s_sim.auto_exit_at_ms);
                    app_sim_unlock();
                }
            }
            next_wait = pdMS_TO_TICKS(request.interval_ms);
            continue;
        }

        if (s_sim.exit_waiting) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (s_sim.exit_requested || (s_sim.auto_exit_at_ms > 0 && now_ms >= s_sim.auto_exit_at_ms)) {
                s_sim.exit_waiting = false;
                s_sim.exit_requested = false;
                app_sim_set_countdown_locked(now_ms + (int64_t)s_sim.exit_frame_count * (int64_t)s_sim.interval_ms);
                if (s_sim.exit_frame_index < s_sim.exit_frame_count) {
                    app_sim_frame_t frame = s_sim.exit_frames[s_sim.exit_frame_index++];
                    bool exit_done = s_sim.exit_frame_index >= s_sim.exit_frame_count;

                    app_sim_set_phase_locked(frame.phase, frame.weight);
                    s_sim.send_count += 1;
                    app_sim_unlock();
                    app_sim_send_one_frame(&request, frame.phase, frame.status, frame.weight);

                    if (exit_done && app_sim_lock(pdMS_TO_TICKS(50))) {
                        s_sim.target_weight = 0;
                        s_sim.target_weight_text[0] = '\0';
                        app_sim_set_countdown_locked(0);
                        app_sim_unlock();
                    }
                    next_wait = pdMS_TO_TICKS(request.interval_ms);
                    continue;
                }
            } else {
                app_sim_set_phase_locked("settled_with_driver", request.target_weight);
                s_sim.send_count += 1;
                app_sim_unlock();
                app_sim_send_one_frame(&request, "settled_with_driver", "ST", request.target_weight);
                next_wait = pdMS_TO_TICKS(request.interval_ms);
                continue;
            }
        }

        if (s_sim.exit_frame_index < s_sim.exit_frame_count) {
            app_sim_frame_t frame = s_sim.exit_frames[s_sim.exit_frame_index++];
            bool exit_done = s_sim.exit_frame_index >= s_sim.exit_frame_count;

            app_sim_set_phase_locked(frame.phase, frame.weight);
            s_sim.send_count += 1;
            app_sim_unlock();
            app_sim_send_one_frame(&request, frame.phase, frame.status, frame.weight);

            if (exit_done && app_sim_lock(pdMS_TO_TICKS(50))) {
                s_sim.target_weight = 0;
                s_sim.target_weight_text[0] = '\0';
                app_sim_set_countdown_locked(0);
                app_sim_unlock();
            }
            next_wait = pdMS_TO_TICKS(request.interval_ms);
            continue;
        }

        app_sim_set_phase_locked("final_stable", 0);
        app_sim_set_countdown_locked(0);
        s_sim.send_count += 1;
        app_sim_unlock();
        app_sim_send_one_frame(&request, "final_stable", "ST", 0);
        next_wait = pdMS_TO_TICKS(request.interval_ms);
    }
}

void app_sim_init(void)
{
    s_sim_mutex = xSemaphoreCreateMutex();
    if (s_sim_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sim mutex");
        return;
    }
    if (xTaskCreate(app_sim_task_main, "sim_task", 6144, NULL, 5, &s_sim_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sim task");
        s_sim_task = NULL;
    }
}

esp_err_t app_sim_send_status_json(httpd_req_t *req, const char *message)
{
    char escaped_phase[SIM_PHASE_SIZE * 2];
    char escaped_target[SIM_TARGET_TEXT_SIZE * 2];
    char escaped_preview[UART_TX_PREVIEW_SIZE * 2];
    char target_text[SIM_TARGET_TEXT_SIZE];
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t countdown_remaining_ms = 0;

    if (!app_sim_lock(pdMS_TO_TICKS(100))) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"sim_busy\"}");
    }

    app_copy_string(target_text, sizeof(target_text),
                    s_sim.target_weight_text[0] != '\0' ? s_sim.target_weight_text : "-");
    app_json_escape_string(escaped_phase, sizeof(escaped_phase), s_sim.phase[0] != '\0' ? s_sim.phase : "idle");
    app_json_escape_string(escaped_target, sizeof(escaped_target), target_text);

    escaped_preview[0] = '\0';
    if (app_uart_lock(pdMS_TO_TICKS(200))) {
        app_json_escape_string(escaped_preview, sizeof(escaped_preview), s_uart.last_tx_preview);
        app_uart_unlock();
    }

    if (s_sim.countdown_ends_at_ms > now_ms) {
        countdown_remaining_ms = s_sim.countdown_ends_at_ms - now_ms;
    }

    {
        esp_err_t err = app_http_send_jsonf(
            req, NULL,
            "{\"status\":\"ok\",\"message\":\"%s\","
            "\"active\":%s,\"phase\":\"%s\",\"target_weight\":%.3f,\"target_weight_text\":\"%s\","
            "\"current_weight\":%.3f,\"send_count\":%u,\"interval_ms\":%u,"
              "\"exit_waiting\":%s,\"auto_exit_at_ms\":%lld,"
              "\"countdown_ends_at_ms\":%lld,\"countdown_remaining_ms\":%lld,"
            "\"last_payload_preview\":\"%s\"}",
            message != NULL ? message : "",
            s_sim.active ? "true" : "false",
            escaped_phase,
            s_sim.target_weight,
            escaped_target,
            s_sim.current_weight,
            (unsigned)s_sim.send_count,
            (unsigned)s_sim.interval_ms,
            s_sim.exit_waiting ? "true" : "false",
            (long long)s_sim.auto_exit_at_ms,
              (long long)s_sim.countdown_ends_at_ms,
              (long long)countdown_remaining_ms,
            escaped_preview);
        app_sim_unlock();
        return err;
    }
}

esp_err_t app_sim_handle_start_request(httpd_req_t *req, const char *body)
{
    app_sim_request_t request;
    app_sim_frame_t *frames = NULL;
    app_sim_frame_t *exit_frames = NULL;
    size_t frame_count = 0;
    size_t exit_count = 0;
    bool allow_decimal = false;
    uint16_t speed_kmh = 0;
    uint16_t decimal_places = 0;
    uint16_t weight_step_kg = 5;
    uint32_t interval_ms = 1000;

    memset(&request, 0, sizeof(request));
    if (!app_json_find_double(body, "target_weight", &request.target_weight)
        || request.target_weight < SIM_TARGET_WEIGHT_MIN
        || request.target_weight > SIM_TARGET_WEIGHT_MAX
        || fabs(request.target_weight - round(request.target_weight)) > 0.000001) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_target_weight\"}");
    }
    if (!app_json_find_string(body, "target_weight_text", request.target_weight_text, sizeof(request.target_weight_text))) {
        snprintf(request.target_weight_text, sizeof(request.target_weight_text), "%.0f", request.target_weight);
    }
    if (!app_sim_is_valid_target_weight_text(request.target_weight_text, (uint32_t)llround(request.target_weight))) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_target_weight_text\"}");
    }
    if (!app_json_find_string(body, "protocol", request.protocol, sizeof(request.protocol))) {
        app_copy_string(request.protocol, sizeof(request.protocol), "ds10");
    }
    app_json_find_bool(body, "allow_decimal", &allow_decimal);
    request.allow_decimal = allow_decimal;
    if (app_json_find_u16(body, "decimal_places", &decimal_places)) {
        request.decimal_places = decimal_places > 4 ? 4 : decimal_places;
    }
    if (app_json_find_u16(body, "weight_step_kg", &weight_step_kg) && weight_step_kg > 0) {
        request.weight_step_kg = weight_step_kg;
    } else {
        request.weight_step_kg = 5;
    }
    if (app_json_find_u16(body, "speed_kmh", &speed_kmh)) {
        request.speed_kmh = speed_kmh;
    } else {
        request.speed_kmh = 6;
    }
    if (app_json_find_u32(body, "interval_ms", &interval_ms)) {
        request.interval_ms = interval_ms;
    }

    if (!(request.interval_ms == 200 || request.interval_ms == 500 || request.interval_ms == 1000)) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_interval_ms\"}");
    }
    if (request.speed_kmh < 1 || request.speed_kmh > 15) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_speed_kmh\"}");
    }

    if (!app_sim_build_vehicle_frames(&request, &frames, &frame_count, &exit_frames, &exit_count)) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"sim_plan_failed\"}");
    }

    app_uart_stop_keepalive();
    if (!app_sim_lock(pdMS_TO_TICKS(200))) {
        free(frames);
        free(exit_frames);
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"sim_busy\"}");
    }

    app_sim_reset_locked();
    s_sim.active = true;
    s_sim.exit_waiting = false;
    s_sim.exit_requested = false;
    s_sim.allow_decimal = request.allow_decimal;
    s_sim.decimal_places = request.decimal_places;
    s_sim.weight_step_kg = request.weight_step_kg;
    s_sim.speed_kmh = request.speed_kmh;
    s_sim.interval_ms = request.interval_ms;
    s_sim.send_count = 0;
    s_sim.started_at_ms = esp_timer_get_time() / 1000;
    s_sim.auto_exit_at_ms = 0;
    s_sim.countdown_ends_at_ms = s_sim.started_at_ms + (int64_t)frame_count * (int64_t)request.interval_ms;
    s_sim.target_weight = request.target_weight;
    s_sim.current_weight = 0;
    app_copy_string(s_sim.protocol, sizeof(s_sim.protocol), request.protocol);
    app_copy_string(s_sim.phase, sizeof(s_sim.phase), "front_axle");
    app_copy_string(s_sim.target_weight_text, sizeof(s_sim.target_weight_text), request.target_weight_text);
    s_sim.frames = frames;
    s_sim.frame_count = frame_count;
    s_sim.exit_frames = exit_frames;
    s_sim.exit_frame_count = exit_count;
    s_sim.frame_index = 0;
    s_sim.exit_frame_index = 0;
    app_sim_unlock();

    app_sim_notify_task();
    return app_sim_send_status_json(req, "sim_started");
}

esp_err_t app_sim_handle_exit_request(httpd_req_t *req)
{
    if (!app_sim_lock(pdMS_TO_TICKS(200))) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"sim_busy\"}");
    }
    if (!s_sim.active || !s_sim.exit_waiting) {
        app_sim_unlock();
        return app_http_send_json_text(req, "409 Conflict",
                                   "{\"status\":\"error\",\"message\":\"sim_exit_unavailable\"}");
    }

    s_sim.exit_waiting = false;
    s_sim.exit_requested = true;
    app_sim_set_countdown_locked((esp_timer_get_time() / 1000) + (int64_t)s_sim.exit_frame_count * (int64_t)s_sim.interval_ms);
    app_copy_string(s_sim.phase, sizeof(s_sim.phase), "ramp_down");
    app_sim_unlock();
    app_sim_notify_task();
    return app_sim_send_status_json(req, "sim_exit_requested");
}

esp_err_t app_sim_handle_stop_request(httpd_req_t *req)
{
    if (!app_sim_lock(pdMS_TO_TICKS(200))) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"sim_busy\"}");
    }
    app_sim_reset_locked();
    app_sim_unlock();
    app_sim_notify_task();
    return app_sim_send_status_json(req, "sim_stopped");
}
