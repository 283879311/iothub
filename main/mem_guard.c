#include "mem_guard.h"

#include "esp_heap_caps.h"
#include "esp_system.h"

/* 见 mem_guard.h 的阈值依据。两项都要过线才降档：free 反映总量，
 * largest 反映"最紧的连续块"（分片后的真实可用度）。 */
#define APP_MEM_PRESSED_FREE     4096
#define APP_MEM_PRESSED_LARGEST  2560
#define APP_MEM_CRITICAL_FREE    3072
#define APP_MEM_CRITICAL_LARGEST 2048

app_mem_level_t app_mem_level(void)
{
    const size_t free_now = esp_get_free_heap_size();
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    if (free_now < APP_MEM_CRITICAL_FREE || largest < APP_MEM_CRITICAL_LARGEST) {
        return APP_MEM_CRITICAL;
    }
    if (free_now < APP_MEM_PRESSED_FREE || largest < APP_MEM_PRESSED_LARGEST) {
        return APP_MEM_PRESSED;
    }
    return APP_MEM_NORMAL;
}

const char *app_mem_level_str(app_mem_level_t level)
{
    switch (level) {
    case APP_MEM_CRITICAL:
        return "critical";
    case APP_MEM_PRESSED:
        return "pressed";
    case APP_MEM_NORMAL:
    default:
        return "normal";
    }
}
