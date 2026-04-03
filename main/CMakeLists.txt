idf_component_register(SRCS "blink_example_main.c" "ina226.c" "dns_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2c esp_driver_gpio esp_wifi esp_http_server nvs_flash esp_netif esp_event)
