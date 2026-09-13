idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "linux")
    return()
endif()

file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/usb" idf_usb_dir)
set(srcs
    "hcd_dwc.c"
    "${idf_usb_dir}/enum.c"
    "${idf_usb_dir}/hub.c"
    "${idf_usb_dir}/usb_helpers.c"
    "${idf_usb_dir}/usb_host.c"
    "${idf_usb_dir}/usb_private.c"
    "${idf_usb_dir}/usbh.c"
    "${idf_usb_dir}/usb_phy.c")

set(priv_requires esp_driver_gpio esp_mm)
if(${target} STREQUAL "esp32p4")
    list(APPEND priv_requires esp_psram)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "${idf_usb_dir}/include"
    PRIV_INCLUDE_DIRS "${idf_usb_dir}/private_include"
    PRIV_REQUIRES ${priv_requires}
    LDFRAGMENTS "linker.lf")
