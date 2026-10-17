idf_component_register(SRCS "app_main.c" "gemmi_wifi.c" "gemmi_bridge.c" "gemmi_display.c" "gemmi_ui.c" "gemmi_pet.c" "gemmi_anims.c"
                    PRIV_REQUIRES esp_wifi wpa_supplicant nvs_flash esp_timer esp_lcd esp_http_client esp-tls driver
                    INCLUDE_DIRS ".")
