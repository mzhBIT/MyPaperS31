set(srcs
    "main.c"
    "audio/audio_radio.c"
    "storage/storage_sdcard.c"
    "wifi/wifi_setting.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "." "audio" "storage" "wifi"
                       REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_i2c esp_driver_i2s nvs_flash esp_event esp_netif esp_wifi esp_psram esp_codec_dev esp_audio_simple_player esp_hls_stream gmf_core gmf_audio media_lib_sal
                       WHOLE_ARCHIVE)

if(NOT CONFIG_SOC_SDMMC_HOST_SUPPORTED)
    fail_at_build_time(sdmmc ""
                             "Only targets with an SDMMC host are supported."
                             "Please refer README.md for more details")
endif()
