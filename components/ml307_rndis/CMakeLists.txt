idf_component_register(SRCS "iot_usbh_rndis.c" "iot_usbh_rndis_descriptor.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES esp_ringbuf esp_event esp_hw_support)
