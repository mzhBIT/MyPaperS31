include(${CMAKE_CURRENT_LIST_DIR}/../../../ApplicationSources.cmake)
list(REMOVE_ITEM srcs
    "boards/MyPaperP4/BoardP4.cpp"
    "storage/storage_sdcard.c"
    "frontlight/frontlight.cpp"
    "usb/usb_transfer_boot.c" "usb/usb_msc_mode.cpp" "usb/usb_modem_probe.c"
    "modem/ml307_sms.cpp"
    "audio/audio_radio.c" "audio/usb_audio_output.c" "audio/m5_i2c_codec_ctrl.cpp")
list(TRANSFORM srcs PREPEND "${CMAKE_CURRENT_LIST_DIR}/../../../")
list(APPEND srcs "../BoardS31.cpp" "../StorageS31.cpp" "../FrontlightS31.cpp" "../UnavailableHardware.cpp")

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "../../.." "../../../ui" "../../../parser" "../../../renderer"
                 "../../../storage" "../../../activities" "../../../input"
                 "../../../wifi" "../../../usb" "../../../modem"
    REQUIRES M5GFX M5Unified freetype fatfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_ledc
             nvs_flash esp_common esp_system esp_psram esp_hw_support log freertos
             esp_wifi esp_event esp_netif esp_http_client esp_http_server esp-tls
             esp_driver_jpeg esp_driver_ppa esp_adc esp_timer mbedtls
             espressif__libjpeg-turbo
    # Unlike IDF's special "main" component, this custom application archive
    # is not retained automatically. Keep the strong waveform provider even
    # though M5GFX supplies a weak fallback that otherwise satisfies linking.
    WHOLE_ARCHIVE)
set_source_files_properties("../../../weread/qrcodegen.cpp" "../../../weread/WereadQr.cpp"
                            PROPERTIES COMPILE_OPTIONS "-fexceptions")
# GCC 15 conservatively warns about the unchanged libjpeg setjmp label argument.
set_source_files_properties("../../../renderer/TurboJpegDecoder.cpp"
                            PROPERTIES COMPILE_OPTIONS "-Wno-error=clobbered")
target_add_binary_data(${COMPONENT_LIB} "../../../assets/system-ui.ttf" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../../../spiffs/fonts/reader.bin" BINARY)
