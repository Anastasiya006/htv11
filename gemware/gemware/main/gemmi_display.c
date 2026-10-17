/*
 * ILI9341 2.8" SPI TFT (240x320) + XPT2046 resistive touch, driven via esp_lcd + LVGL.
 * Adapted from the ESP-IDF "spi_lcd_touch" example (originally ST7789) for ILI9341,
 * with GPIOs chosen to avoid UART0 (flashing/console), on-board flash, and strapping pins.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_xpt2046.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "gemmi_display.h"
#include "gemmi_ui.h"

static const char *TAG = "gemmi_display";

// Set to 0 to test the display with the touch controller fully untouched by firmware
#define ENABLE_TOUCH 1

// LCD and touch are on two independent SPI buses/pin sets — no shared wires needed
#define LCD_HOST    SPI2_HOST
#define TOUCH_HOST  SPI3_HOST

#define LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)
#define LCD_BK_LIGHT_ON_LEVEL  1   // flip to 0 if backlight behaves inverted on your board
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL

#define PIN_NUM_SCLK            18
#define PIN_NUM_MOSI            23
#define PIN_NUM_MISO            19
#define PIN_NUM_LCD_DC          4
#define PIN_NUM_LCD_RST         22
#define PIN_NUM_LCD_CS          5
#define PIN_NUM_BK_LIGHT        21

#define PIN_NUM_TOUCH_SCLK       33
#define PIN_NUM_TOUCH_MOSI       25   // -> display's T_DIN
#define PIN_NUM_TOUCH_MISO       26   // -> display's T_DO / T_OUT
#define PIN_NUM_TOUCH_CS         32

#define LCD_H_RES               240
#define LCD_V_RES               320
#define LCD_CMD_BITS            8
#define LCD_PARAM_BITS          8

#define LVGL_DRAW_BUF_LINES     40
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_TASK_MAX_DELAY_MS  500
#define LVGL_TASK_MIN_DELAY_MS  (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_TASK_STACK_SIZE    (12 * 1024)
#define LVGL_TASK_PRIORITY      2

static _lock_t lvgl_api_lock;

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_port_update_callback(lv_display_t *disp)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);

    switch (rotation) {
    case LV_DISPLAY_ROTATION_0:
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISPLAY_ROTATION_90:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISPLAY_ROTATION_180:
        esp_lcd_panel_swap_xy(panel_handle, false);
        /* LVGL does not rotate the framebuffer (matrix rotation is off), so
         * the hardware does the whole 180: both axes mirrored. */
        esp_lcd_panel_mirror(panel_handle, true, true);
        break;
    case LV_DISPLAY_ROTATION_270:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, true);
        break;
    }
}

/* This panel's MADCTL MX bit does not flip the columns (observed: both
 * mirror_x values render identically), so the last step of the 180 is done
 * by reversing each flushed row in software and drawing it mirrored. */
static void flip_rows_x(uint8_t *px_map, int width, int height)
{
    uint16_t *pixels = (uint16_t *)px_map;
    for (int row = 0; row < height; row++) {
        uint16_t *line = pixels + row * width;
        for (int left = 0, right = width - 1; left < right; left++, right--) {
            uint16_t swap = line[left];
            line[left] = line[right];
            line[right] = swap;
        }
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lvgl_port_update_callback(disp);
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    int width = area->x2 - area->x1 + 1;
    int height = offsety2 - offsety1 + 1;
    flip_rows_x(px_map, width, height);
    int offsetx1 = LCD_H_RES - 1 - area->x2;
    int offsetx2 = LCD_H_RES - 1 - area->x1;
    // LVGL renders directly in the ILI9341's big-endian RGB565 byte order.
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;

    esp_lcd_touch_handle_t touch_pad = lv_indev_get_user_data(indev);
    esp_err_t read_err = esp_lcd_touch_read_data(touch_pad);
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(touch_pad, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

    if (read_err != ESP_OK) {
        ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(read_err));
    }

    if (touchpad_pressed && touchpad_cnt > 0) {
        /* mirror_x is inert on this XPT2046 (a left press lands on the right
         * side), so the X flip is applied here instead. LVGL then rotates the
         * point by the display's 180. */
        data->point.x = LCD_H_RES - 1 - touchpad_x[0];
        data->point.y = touchpad_y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

    /* Debug: drop a dot at the mapped touch point to verify alignment. */
    static lv_obj_t *dot;
    if (data->state == LV_INDEV_STATE_PRESSED) {
        if (!dot) {
            dot = lv_obj_create(lv_layer_top());
            lv_obj_set_size(dot, 12, 12);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(0xE03030), 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_set_pos(dot, data->point.x - 6, data->point.y - 6);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(dot);
    }
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

/* The network task uses these around its small UI updates. LVGL itself stays
 * owned by the display task; this only prevents concurrent object changes. */
void gemmi_display_lock(void)
{
    _lock_acquire(&lvgl_api_lock);
}

void gemmi_display_unlock(void)
{
    _lock_release(&lvgl_api_lock);
}

void gemmi_display_init(void)
{
    esp_log_level_set("xpt2046", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL);

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Install ILI9341 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    /* Rotate the complete canvas in hardware. Do not rotate individual LVGL
     * objects: the ESP32 software ARGB transform path can panic under redraw. */
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_180);

    size_t draw_buffer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Register panel IO event callback for LVGL flush ready");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

#if ENABLE_TOUCH
    ESP_LOGI(TAG, "Initialize touch SPI bus (independent of the LCD bus)");
    spi_bus_config_t touch_buscfg = {
        .sclk_io_num = PIN_NUM_TOUCH_SCLK,
        .mosi_io_num = PIN_NUM_TOUCH_MOSI,
        .miso_io_num = PIN_NUM_TOUCH_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_HOST, &touch_buscfg, SPI_DMA_DISABLED));

    ESP_LOGI(TAG, "Initialize touch controller XPT2046");
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_config = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_NUM_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TOUCH_HOST, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            /* LVGL rotates indev points by the display rotation (180), so the
             * touch layer must supply the panel's own 180 to land on identity. */
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &tp));

    static lv_indev_t *indev;
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_user_data(indev, tp);
    lv_indev_set_read_cb(indev, lvgl_touch_cb);
#else
    ESP_LOGW(TAG, "Touch disabled (ENABLE_TOUCH=0) - testing display only");
#endif

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Display LVGL demo UI");
    _lock_acquire(&lvgl_api_lock);
    gemmi_ui_init(display);
    _lock_release(&lvgl_api_lock);
}
