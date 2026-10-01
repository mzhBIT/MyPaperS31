idf_component_register(
    SRCS "app_main.cpp" "../../Board.cpp" "../BoardS31.cpp"
    INCLUDE_DIRS "../../.."
    REQUIRES esp_common esp_system log freertos)
