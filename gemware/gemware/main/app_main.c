#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "gemmi_bridge.h"
#include "gemmi_display.h"
#include "gemmi_wifi.h"

static const char *TAG = "gemware";

/* Most ESP32-WROOM development boards expose their blue status LED on GPIO2. */
#define STATUS_LED_GPIO GPIO_NUM_2

static void status_led_init(void)
{
    const gpio_config_t config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(STATUS_LED_GPIO, 1);
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    status_led_init();
    gemmi_display_init();
    gemmi_wifi_connect();
    gemmi_bridge_start();
}
