include(${CMAKE_CURRENT_LIST_DIR}/../../../ApplicationSources.cmake)
list(REMOVE_ITEM srcs
    "boards/MyPaperP4/BoardP4.cpp"
    "frontlight/frontlight.cpp"
    "usb/usb_modem_probe.c"
    "modem/ml307_sms.cpp"
    "audio/audio_radio.c" "audio/m5_i2c_codec_ctrl.cpp")
list(TRANSFORM srcs PREPEND "${CMAKE_CURRENT_LIST_DIR}/../../../")
list(APPEND srcs "../BoardS31.cpp" "../FrontlightS31.cpp" "../UnavailableHardware.cpp" "../KeysS31.cpp" "../StandbyS31.cpp" "../UsbAudioPort.cpp"
                "../../../audio/audio_radio_s31.cpp" "../../../audio/file_duration.cpp")

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "../../.." "../../../ui" "../../../parser" "../../../renderer"
                 "../../../storage" "../../../activities" "../../../input"
                 "../../../wifi" "../../../usb" "../../../modem"
    REQUIRES app_update M5GFX M5Unified freetype fatfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_ledc
             esp_driver_i2s esp_driver_uart
             nvs_flash esp_common esp_system esp_psram esp_hw_support log freertos
             esp_wifi esp_event esp_netif esp_http_client esp_http_server esp-tls
             esp_driver_jpeg esp_driver_ppa esp_adc esp_timer mbedtls
             esp_tinyusb usb uac2_host espressif__usb_host_uac
             espressif__libjpeg-turbo espressif__esp_audio_codec
             espressif__esp_audio_effects espressif__gmf_fft
             espressif__gmf_core espressif__gmf_audio espressif__gmf_io
             espressif__esp_audio_simple_player espressif__esp_hls_stream
             espressif__esp_extractor espressif__media_lib_sal
             espressif__gmf_core espressif__gmf_audio espressif__gmf_io
             espressif__esp_audio_simple_player espressif__esp_hls_stream
             espressif__esp_extractor espressif__media_lib_sal
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
