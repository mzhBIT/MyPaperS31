if(NOT IDF_TARGET STREQUAL "esp32s31")
    message(FATAL_ERROR "Independent emulator firmware supports ESP32-S31 only")
endif()
idf_component_register(
    SRCS "app_main.cpp" "S31Hardware.cpp" "PaperDisplay.cpp" "RomStorage.cpp" "gb_core.c"
         "GameAudioOutput.cpp" "vendor/minigb_apu/minigb_apu.c"
         "../../main/boot/firmware_switch.cpp" "../../main/boards/MyPaperS31/FrontlightS31.cpp"
    INCLUDE_DIRS "." "vendor" "../../main"
    REQUIRES M5GFX app_update nvs_flash esp_timer esp_driver_gpio esp_driver_ledc
             esp_driver_i2c esp_driver_i2s esp_driver_uart esp_driver_sdmmc fatfs esp_lcd
    WHOLE_ARCHIVE)
set_source_files_properties(gb_core.c PROPERTIES COMPILE_OPTIONS "-O3;-Wno-error=unused-label")
set_source_files_properties(PaperDisplay.cpp PROPERTIES COMPILE_OPTIONS "-O3")
set_source_files_properties(vendor/minigb_apu/minigb_apu.c PROPERTIES COMPILE_OPTIONS "-O3")
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS)
