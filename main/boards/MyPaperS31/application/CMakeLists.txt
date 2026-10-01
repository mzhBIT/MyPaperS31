idf_component_register(
    SRCS "app_main.cpp" "../../Board.cpp" "../BoardS31.cpp"
         "../../../waveforms/my_waveform.c"
    INCLUDE_DIRS "../../.."
    REQUIRES M5GFX nvs_flash esp_common esp_system esp_psram log freertos)
