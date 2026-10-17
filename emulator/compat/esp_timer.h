#pragma once

#include <stdint.h>

#include "lvgl.h"

/* ESP-IDF's monotonic microsecond clock, mapped to LVGL's SDL-backed tick. */
static inline int64_t esp_timer_get_time(void)
{
    return (int64_t)lv_tick_get() * 1000;
}
