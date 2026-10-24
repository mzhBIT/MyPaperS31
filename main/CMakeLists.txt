include(${CMAKE_CURRENT_LIST_DIR}/ApplicationSources.cmake)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "." "activities" "input" "modem" "parser" "renderer" "storage" "ui" "usb" "wifi"
                       REQUIRES fatfs sdmmc esp_driver_sdmmc esp_hw_support esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_jpeg esp_driver_ppa esp_adc esp_timer freertos freetype M5GFX M5Unified nvs_flash esp_event esp_netif esp_wifi esp_http_server esp_http_client esp-tls esp_psram usb esp_codec_dev esp_audio_simple_player esp_extractor esp_hls_stream gmf_core gmf_audio media_lib_sal espressif__eppp_link espressif__esp_hosted espressif__esp_wifi_remote espressif__esp_tinyusb espressif__libjpeg-turbo espressif__usb_host_uac espressif__iot_eth espressif__iot_usbh_cdc uac2_host ml307_rndis
                       WHOLE_ARCHIVE)

set_source_files_properties("weread/qrcodegen.cpp" "weread/WereadQr.cpp"
                            PROPERTIES COMPILE_OPTIONS "-fexceptions")

target_add_binary_data(${COMPONENT_LIB} "spiffs/fonts/reader.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/system-ui.ttf" BINARY)

if(NOT CONFIG_SOC_SDMMC_HOST_SUPPORTED)
    fail_at_build_time(sdmmc ""
                             "Only targets with an SDMMC host are supported."
                             "Please refer README.md for more details")
endif()
