idf_component_register(SRCS "main.cpp" "llm_client.cpp" "wake_word.cpp" "asr_client.cpp"
                            "wifi_prov.cpp" "uart_link.cpp" "audio_hub.cpp" "tts_client.cpp" "local_cue.cpp" "music_player.cpp" "local_asr.cpp" "reply_player.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer
                                esp_http_client lwip cjson mbedtls
                                esp_psram esp_driver_i2s esp_driver_i2c esp_driver_gpio
                                esp_driver_uart driver
                                esp-sr
                                network_provisioning)
