idf_component_register(SRCS "gemware_main.c" "lcd_display.c" "lvgl_demo_ui.c" "gemmi_anims.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_timer esp_lcd driver
                    INCLUDE_DIRS ".")
