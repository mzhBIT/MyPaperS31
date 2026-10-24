# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(MYPAPER_BOARD "MyPaperS31" CACHE STRING "MyPaper hardware profile")
set_property(CACHE MYPAPER_BOARD PROPERTY STRINGS MyPaperP4 MyPaperS31)
include($ENV{IDF_PATH}/tools/cmake/version.cmake)
if(MYPAPER_BOARD STREQUAL "MyPaperS31")
    if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
        message(FATAL_ERROR "MyPaperS31 requires ESP-IDF 6.1; activate its environment first")
    endif()
    if(DEFINED IDF_TARGET AND NOT IDF_TARGET STREQUAL "esp32s31")
        message(FATAL_ERROR "MyPaperS31 requires IDF_TARGET=esp32s31 and a separate build directory")
    endif()
    set(IDF_TARGET esp32s31 CACHE STRING "IDF target")
    if(CMAKE_BINARY_DIR STREQUAL CMAKE_CURRENT_LIST_DIR OR
       CMAKE_BINARY_DIR STREQUAL "${CMAKE_CURRENT_LIST_DIR}/build")
        message(FATAL_ERROR "Use a separate S31 build directory (recommended: build-s31)")
    endif()
    if(NOT DEFINED SDKCONFIG)
        set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    endif()
    get_filename_component(_board_sdkconfig "${SDKCONFIG}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    if(_board_sdkconfig STREQUAL "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
        message(FATAL_ERROR "S31 must not overwrite the existing P4 root sdkconfig")
    endif()
    if(NOT DEFINED SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/configs/mypapers31/sdkconfig.defaults")
    endif()
    # Share application sources, but retain a board-local component entry so
    # IDF 6.1 never resolves the P4 main component's managed dependency manifest.
    set(ENV{IDF_COMPONENT_MANAGER} "0")
    set(COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main/boards/MyPaperS31/application"
                       "${CMAKE_CURRENT_LIST_DIR}/components/M5GFX"
                       "${CMAKE_CURRENT_LIST_DIR}/components/M5Unified"
                       "${CMAKE_CURRENT_LIST_DIR}/components/freetype"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__libjpeg-turbo"
                       "$ENV{IDF_PATH}/components")
    set(COMPONENTS application)
elseif(MYPAPER_BOARD STREQUAL "MyPaperP4")
    if(NOT IDF_VERSION_MAJOR EQUAL 5 OR NOT IDF_VERSION_MINOR EQUAL 3 OR NOT IDF_VERSION_PATCH EQUAL 5)
        message(FATAL_ERROR "MyPaperP4 baseline requires ESP-IDF 5.3.5")
    endif()
    if(DEFINED IDF_TARGET AND NOT IDF_TARGET STREQUAL "esp32p4")
        message(FATAL_ERROR "MyPaperP4 requires IDF_TARGET=esp32p4")
    endif()
else()
    message(FATAL_ERROR "Unknown MYPAPER_BOARD: ${MYPAPER_BOARD}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(MYPAPER_BOARD STREQUAL "MyPaperP4")
    set(EXTRA_COMPONENT_DIRS
        "managed_components/espressif__eppp_link"
        "managed_components/espressif__esp_audio_simple_player"
        "managed_components/espressif__esp_hosted"
        "managed_components/espressif__esp_wifi_remote")

    set(COMPONENTS main)
endif()

project(${MYPAPER_BOARD})

# The registry package refers to its IDF target by the unnamespaced package
# name, while Component Manager registers it with the vendor prefix.
if(TARGET idf::espressif__libjpeg-turbo AND NOT TARGET idf::libjpeg-turbo)
    get_target_property(libjpeg_turbo_component idf::espressif__libjpeg-turbo ALIASED_TARGET)
    add_library(idf::libjpeg-turbo ALIAS ${libjpeg_turbo_component})
endif()
