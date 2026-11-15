#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_DEFAULT 0
#ifdef __cplusplus
extern "C" {
#endif
size_t heap_caps_get_largest_free_block(uint32_t caps);
#ifdef __cplusplus
}
#endif
