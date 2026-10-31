# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(MYPAPER_BOARD "MyPaperS31" CACHE STRING "MyPaper hardware profile")
set(MYPAPER_FIRMWARE "System" CACHE STRING "System or independent Emulator firmware")
set_property(CACHE MYPAPER_FIRMWARE PROPERTY STRINGS System Emulator)
if(NOT MYPAPER_FIRMWARE MATCHES "^(System|Emulator)$")
    message(FATAL_ERROR "Unknown MYPAPER_FIRMWARE: ${MYPAPER_FIRMWARE}")
endif()
if(MYPAPER_FIRMWARE STREQUAL "Emulator" AND NOT MYPAPER_BOARD STREQUAL "MyPaperS31")
    message(FATAL_ERROR "Independent emulator firmware supports MyPaperS31 only")
endif()
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
        if(MYPAPER_FIRMWARE STREQUAL "Emulator")
            set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/firmware/emulator/sdkconfig.defaults")
        else()
            set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/configs/mypapers31/sdkconfig.defaults")
        endif()
    endif()
    # Share application sources, but retain a board-local component entry so
    # IDF 6.1 never resolves the P4 main component's managed dependency manifest.
    set(ENV{IDF_COMPONENT_MANAGER} "0")
    set(COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/main/boards/MyPaperS31/application"
                       "${CMAKE_CURRENT_LIST_DIR}/components/M5GFX"
                       "${CMAKE_CURRENT_LIST_DIR}/components/M5Unified"
                       "${CMAKE_CURRENT_LIST_DIR}/components/freetype"
                       "${CMAKE_CURRENT_LIST_DIR}/third_party/s31/esp_tinyusb"
                       "${CMAKE_CURRENT_LIST_DIR}/third_party/s31/tinyusb"
                       "${CMAKE_CURRENT_LIST_DIR}/third_party/s31/usb"
                       "${CMAKE_CURRENT_LIST_DIR}/components/uac2_host"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__usb_host_uac"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__cmake_utilities"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__libjpeg-turbo"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_audio_codec"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_audio_effects"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__gmf_fft"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__gmf_core"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__gmf_audio"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__gmf_io"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_audio_simple_player"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_hls_stream"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_extractor"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__media_lib_sal"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__gmf_core"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__gmf_audio"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__gmf_io"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_audio_simple_player"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_hls_stream"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_extractor"
                       "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__media_lib_sal"
                       "$ENV{IDF_PATH}/components")
    # UAC1 uses the package_manager module even with dependency resolution off.
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__cmake_utilities")
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

if(MYPAPER_FIRMWARE STREQUAL "Emulator")
    set(COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/firmware/emulator"
                       "${CMAKE_CURRENT_LIST_DIR}/components/M5GFX"
                       "$ENV{IDF_PATH}/components")
    set(COMPONENTS emulator)
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

if(MYPAPER_FIRMWARE STREQUAL "Emulator")
    project(MyPaperEmulatorS31)
else()
    project(${MYPAPER_BOARD})
endif()

if(MYPAPER_BOARD STREQUAL "MyPaperS31" AND MYPAPER_FIRMWARE STREQUAL "System")
    # S31 deliberately disables Component Manager. Supply the dependencies
    # normally exported by these packages' idf_component.yml manifests.
    target_link_libraries(__idf_espressif__gmf_audio PUBLIC
        idf::espressif__gmf_core idf::espressif__esp_audio_codec idf::espressif__esp_audio_effects)
    target_link_libraries(__idf_espressif__gmf_io PUBLIC idf::espressif__gmf_core)
    target_link_libraries(__idf_espressif__esp_audio_simple_player PUBLIC
        idf::espressif__gmf_audio idf::espressif__gmf_io)
    target_link_libraries(__idf_espressif__esp_hls_stream PUBLIC
        idf::espressif__esp_extractor idf::espressif__gmf_io idf::espressif__media_lib_sal)
endif()

if(MYPAPER_BOARD STREQUAL "MyPaperS31" AND MYPAPER_FIRMWARE STREQUAL "System")
    # S31 deliberately disables Component Manager. Supply the dependencies
    # normally exported by these packages' idf_component.yml manifests.
    target_link_libraries(__idf_espressif__gmf_audio PUBLIC
        idf::espressif__gmf_core idf::espressif__esp_audio_codec idf::espressif__esp_audio_effects)
    target_link_libraries(__idf_espressif__gmf_io PUBLIC idf::espressif__gmf_core)
    target_link_libraries(__idf_espressif__esp_audio_simple_player PUBLIC
        idf::espressif__gmf_audio idf::espressif__gmf_io)
    target_link_libraries(__idf_espressif__esp_hls_stream PUBLIC
        idf::espressif__esp_extractor idf::espressif__gmf_io idf::espressif__media_lib_sal)
endif()

# The registry package refers to its IDF target by the unnamespaced package
# name, while Component Manager registers it with the vendor prefix.
if(TARGET idf::espressif__libjpeg-turbo AND NOT TARGET idf::libjpeg-turbo)
    get_target_property(libjpeg_turbo_component idf::espressif__libjpeg-turbo ALIASED_TARGET)
    add_library(idf::libjpeg-turbo ALIAS ${libjpeg_turbo_component})
endif()

if(MYPAPER_FIRMWARE STREQUAL "Emulator")
    # IDF normally flashes the first (factory) app. Never let an emulator build
    # overwrite the system, bootloader, partition table or OTA selection.
    foreach(flash_target flash app-flash)
        foreach(flash_property FLASH_FILE FLASH_ENTRY IMAGES)
            set_property(TARGET ${flash_target} PROPERTY ${flash_property} "")
        endforeach()
        esptool_py_flash_target_image(${flash_target} emulator "0x810000"
            "${CMAKE_BINARY_DIR}/MyPaperEmulatorS31.bin")
    endforeach()
    add_custom_target(emulator_isolation_check ALL
        COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/tools/check_emulator_isolation.py"
            --elf "${CMAKE_BINARY_DIR}/MyPaperEmulatorS31.elf" --nm "${CMAKE_NM}"
        DEPENDS gen_project_binary VERBATIM)
endif()

if(MYPAPER_BOARD STREQUAL "MyPaperS31")
    # IDF's generic check compares one image to every app slot. Our two apps
    # have different budgets; additionally enforce the selected firmware slot.
    if(MYPAPER_FIRMWARE STREQUAL "Emulator")
        set(_firmware_subtype ota_0)
        set(_firmware_bin MyPaperEmulatorS31.bin)
    else()
        set(_firmware_subtype factory)
        set(_firmware_bin MyPaperS31.bin)
    endif()
    partition_table_add_check_size_target(firmware_slot_check
        DEPENDS gen_project_binary BINARY_PATH "${CMAKE_BINARY_DIR}/${_firmware_bin}"
        PARTITION_TYPE app PARTITION_SUBTYPE ${_firmware_subtype})
    add_dependencies(firmware_slot_check app_check_size)
    add_dependencies(app firmware_slot_check)
endif()
