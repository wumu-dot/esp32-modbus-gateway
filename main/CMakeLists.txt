set(PROJECT_NAME "esp32_modbus_gateway")

idf_component_register(SRCS "master_app.c"
                                "wifi_mqtt.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash mqtt driver)