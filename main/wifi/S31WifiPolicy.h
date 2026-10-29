#pragma once

// esp_wifi_set_max_tx_power uses quarter-dBm units and requires Wi-Fi started.
// Shared by ordinary STA connections and the STA/AP file transfer service.
#define S31_WIFI_DEFAULT_TX_POWER_QDBM 20
#define S31_WIFI_DEFAULT_PROTOCOL (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX)
