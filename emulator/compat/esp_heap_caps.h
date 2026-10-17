#pragma once

#include <stdint.h>

#define MALLOC_CAP_8BIT 0

static inline uint32_t esp_get_free_heap_size(void)
{
    return 1024 * 1024;
}

static inline uint32_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 1024 * 1024;
}

static inline uint32_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 1024 * 1024;
}
