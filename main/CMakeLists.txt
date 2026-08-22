set(srcs
    "main.cpp"
    "input/adc_keys.cpp"
    "input/touch_input.cpp"
    "ui/reader_font.cpp"
    "ui/reader_style.cpp"
    "ui/SystemFont.cpp"
    "ui/home_icons_1bpp.cpp"
    "storage/app_paths.cpp"
    "storage/storage_sdcard.c"
    "wifi/wifi_setting.c"
    "audio/audio_radio.c"
    "activities/HomeActivity.cpp"
    "activities/FileBrowserActivity.cpp"
    "activities/ImageViewerActivity.cpp"
    "activities/ReaderActivity.cpp"
    "activities/EpubReaderActivity.cpp"
    "activities/ReaderSettingsActivity.cpp"
    "activities/RadioActivity.cpp"
    "activities/WifiTransferActivity.cpp"
    "activities/RssReaderActivity.cpp"
    "activities/recent_books.cpp"
    "renderer/TextRenderer.cpp"
    "renderer/P4JpegDecoder.cpp"
    "renderer/TurboJpegDecoder.cpp"
    "parser/txt_parser.cpp"
    "parser/epub_zip.cpp"
    "parser/epub_parser.cpp"
    "parser/epub_html.cpp")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "." "activities" "input" "parser" "renderer" "storage" "ui" "wifi"
                       REQUIRES fatfs sdmmc esp_driver_sdmmc esp_hw_support esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_jpeg esp_driver_ppa esp_adc esp_timer freertos freetype M5GFX M5Unified nvs_flash esp_event esp_netif esp_wifi esp_http_server esp_http_client esp-tls esp_psram esp_codec_dev esp_audio_simple_player esp_hls_stream gmf_core gmf_audio media_lib_sal espressif__eppp_link espressif__esp_hosted espressif__esp_wifi_remote espressif__libjpeg-turbo
                       WHOLE_ARCHIVE)

target_add_binary_data(${COMPONENT_LIB} "spiffs/fonts/reader.bin" BINARY)

if(NOT CONFIG_SOC_SDMMC_HOST_SUPPORTED)
    fail_at_build_time(sdmmc ""
                             "Only targets with an SDMMC host are supported."
                             "Please refer README.md for more details")
endif()
