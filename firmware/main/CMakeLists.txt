idf_component_register(SRCS "main.c" "bridge.c" "wifi_sta.c" "setup.c" "settings.c"
                            "display.c" "status.c" "button.c" "oled.c" "font5x8.c"
                            "console.c" "serial_console.c" "rawcfg.c" "eventlog.c"
                            "svcnet.c" "telnet.c"
                       PRIV_REQUIRES esp_wifi esp_eth esp_event esp_netif esp_http_server lwip
                                     nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_uart
                                     esp_timer esp_app_format esp_pm)
