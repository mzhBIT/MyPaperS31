# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    "managed_components/espressif__eppp_link"
    "managed_components/espressif__esp_audio_simple_player"
    "managed_components/espressif__esp_hosted"
    "managed_components/espressif__esp_wifi_remote")

set(COMPONENTS main)

project(MyPaperP4)

# The registry package refers to its IDF target by the unnamespaced package
# name, while Component Manager registers it with the vendor prefix.
if(TARGET idf::espressif__libjpeg-turbo AND NOT TARGET idf::libjpeg-turbo)
    get_target_property(libjpeg_turbo_component idf::espressif__libjpeg-turbo ALIASED_TARGET)
    add_library(idf::libjpeg-turbo ALIAS ${libjpeg_turbo_component})
endif()
