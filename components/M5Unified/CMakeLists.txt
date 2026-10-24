if(IDF_TARGET STREQUAL "esp32s31")
    # The shared apps use Display, software timestamps and Button_Class.
    # Do not compile other boards' PMIC/I2S register implementations on S31.
    idf_component_register(SRCS "src/M5UnifiedS31.cpp" "src/utility/Button_Class.cpp"
                           "src/utility/I2C_Class.cpp" "src/utility/power/BQ27220_Class.cpp"
                           "src/utility/rtc/RTC_Base.cpp" "src/utility/rtc/PCF8563_Class.cpp"
                           INCLUDE_DIRS src REQUIRES M5GFX esp_timer)
    return()
endif()

set(COMPONENT_ADD_INCLUDEDIRS
    src
    )
file(GLOB SRCS
     src/*.cpp
     src/utility/*.cpp
     src/utility/imu/*.cpp
     src/utility/led/*.cpp
     src/utility/power/*.cpp
     src/utility/rtc/*.cpp
     )
set(COMPONENT_SRCS ${SRCS})

if (IDF_VERSION_MAJOR GREATER_EQUAL 5)
    set(COMPONENT_REQUIRES M5GFX esp_adc driver)
else()
    set(COMPONENT_REQUIRES M5GFX esp_adc_cal)
endif()

### If you use arduino-esp32 components, please activate next comment line.
# list(APPEND COMPONENT_REQUIRES arduino-esp32)


message(STATUS "M5Unified use component = ${COMPONENT_REQUIRES}" )


register_component()

