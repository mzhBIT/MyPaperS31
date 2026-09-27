#include "usb/usb_modem_probe.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_rndis.h"
#include "modem/ml307_sms.h"
#include "wifi/wifi_setting.h"

static const char *TAG = "USB_MODEM_PROBE";

#define PROBE_HOST_READY_BIT       BIT0
#define PROBE_CLIENT_READY_BIT     BIT1
#define PROBE_DIAL_READY_BIT       BIT2
#define PROBE_RNDIS_LINK_BIT       BIT3
#define PROBE_TASK_STACK           4096
#define ML307A_VID                 0x2ecc
#define ML307A_PID                 0x3012
#define ML307A_AT_INTERFACE        2
#define ML307A_AT_BULK_IN          0x81
#define ML307A_AT_BULK_OUT         0x0a
#define AT_RESPONSE_CAPACITY       1024
#define AT_RESPONSE_TIMEOUT_US     (3000 * 1000LL)
#define MDIALUP_SETTLE_US          (2000 * 1000LL)
#define AT_HANDOFF_SETTLE_MS        250
#define AT_VOICE_FALLBACK_DELAY_US  (8000 * 1000LL)
static esp_netif_t *s_rndis_netif;

// The ML307A manual requires the RNDIS dial state to be established over its
// AT interface before the host starts DHCP.  Keep that transaction strictly
// before the RNDIS driver is created: both paths use USB control endpoint zero.

typedef struct {
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
} modem_interface_t;

typedef struct {
    usb_device_handle_t device;
    usb_transfer_t *control_transfer;
    usb_transfer_t *rx_transfer;
    usb_transfer_t *tx_transfer;
    modem_interface_t interface;
    bool active;
    bool claimed;
    bool gone;
    bool rx_in_flight;
    bool tx_in_flight;
    bool control_in_flight;
    bool control_done;
    bool control_success;
    bool data_ready;
    bool response_done;
    bool handoff_pending;
    bool dial_active;
    unsigned control_stage;
    unsigned command_index;
    unsigned retry_count;
    int64_t response_deadline_us;
    int64_t next_command_not_before_us;
    char response[AT_RESPONSE_CAPACITY];
    size_t response_length;
} ml307a_at_port_t;

static EventGroupHandle_t s_events;
static usb_host_client_handle_t s_client;
static bool s_started;
static volatile uint8_t s_pending_address;
static ml307a_at_port_t s_at;
static iot_eth_driver_t *s_rndis_driver;
static iot_eth_handle_t s_rndis_handle;
static iot_eth_netif_glue_handle_t s_rndis_glue;
static volatile bool s_rndis_online;
static bool s_at_handoff_complete;
static volatile int64_t s_sms_enable_not_before_us;
static volatile uint8_t s_modem_address;

static const usb_device_match_id_t s_ml307a_rndis_match[] = {
    {
        .match_flags = USB_DEVICE_ID_MATCH_VENDOR |
                       USB_DEVICE_ID_MATCH_PRODUCT |
                       USB_DEVICE_ID_MATCH_INT_NUMBER |
                       USB_DEVICE_ID_MATCH_INT_CLASS |
                       USB_DEVICE_ID_MATCH_INT_SUBCLASS |
                       USB_DEVICE_ID_MATCH_INT_PROTOCOL,
        .idVendor = ML307A_VID,
        .idProduct = ML307A_PID,
        .bInterfaceClass = 0xe0,
        .bInterfaceSubClass = 0x01,
        .bInterfaceProtocol = 0x03,
        .bInterfaceNumber = 0,
    },
    {0},
};

static const char *const s_at_commands[] = {
    "ATE0\r\n",
    "AT+MDIALUPCFG=\"mode\"\r\n",
    "AT+MDIALUPCFG=\"auto\"\r\n",
    "AT+MUECONFIG=\"autoconn\"\r\n",
    "AT+CGDCONT?\r\n",
    "AT+CEREG?\r\n",
    "AT+MDIALUP?\r\n",
    "AT+MDIALUP=1,1\r\n",
    "AT+MDIALUP?\r\n",
};

enum {
    AT_COMMAND_ECHO_OFF = 0,
    AT_COMMAND_DIAL_MODE,
    AT_COMMAND_DIAL_AUTO,
    AT_COMMAND_AUTOCONN,
    AT_COMMAND_PDP_CONTEXT,
    AT_COMMAND_REGISTRATION,
    AT_COMMAND_DIAL_QUERY_BEFORE,
    AT_COMMAND_DIAL_START,
    AT_COMMAND_DIAL_QUERY_AFTER,
};

static void rndis_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == IOT_ETH_EVENT) {
        switch (event_id) {
            case IOT_ETH_EVENT_CONNECTED:
                ESP_LOGI(TAG, "ML307A RNDIS link connected; requesting DHCP address");
                // RNDIS has finished its initial control negotiation.  Only
                // now register the independent native Bulk client and claim
                // modem interface 2; it never submits an EP0 request.
                s_sms_enable_not_before_us = esp_timer_get_time() + 250000;
                xEventGroupSetBits(s_events, PROBE_RNDIS_LINK_BIT);
                break;
            case IOT_ETH_EVENT_DISCONNECTED:
                s_rndis_online = false;
                s_sms_enable_not_before_us = 0;
                xEventGroupClearBits(s_events, PROBE_RNDIS_LINK_BIT);
                ESP_LOGW(TAG, "ML307A RNDIS link disconnected");
                break;
            case IOT_ETH_EVENT_START:
                ESP_LOGI(TAG, "ML307A RNDIS network interface started");
                break;
            case IOT_ETH_EVENT_STOP:
                s_rndis_online = false;
                s_sms_enable_not_before_us = 0;
                xEventGroupClearBits(s_events, PROBE_RNDIS_LINK_BIT);
                ESP_LOGI(TAG, "ML307A RNDIS network interface stopped");
                break;
            default:
                break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event == NULL || event->esp_netif != s_rndis_netif) return;
        esp_err_t route_ret = esp_netif_set_default_netif(s_rndis_netif);
        if (route_ret != ESP_OK) {
            ESP_LOGW(TAG, "ML307A default route selection failed: %s",
                     esp_err_to_name(route_ret));
        }
        ESP_LOGI(TAG, "ML307A online: ip=" IPSTR " gateway=" IPSTR " netmask=" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw),
                 IP2STR(&event->ip_info.netmask));
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(s_rndis_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            ESP_LOGI(TAG, "ML307A DNS: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        s_rndis_online = true;
    }
}

static esp_err_t prepare_ml307a_rndis(void)
{
    ESP_RETURN_ON_ERROR(wifi_setting_network_stack_init(), TAG,
                        "shared network stack init failed");

    const usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = 10,
        .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    ESP_RETURN_ON_ERROR(usbh_cdc_driver_install(&cdc_config), TAG,
                        "USB CDC host install failed");
    ESP_RETURN_ON_ERROR(ml307_sms_transport_init(), TAG,
                        "ML307A SMS AT transport init failed");

    const iot_usbh_rndis_config_t rndis_config = {
        .match_id_list = s_ml307a_rndis_match,
        .defer_connect = true,
    };
    ESP_RETURN_ON_ERROR(iot_eth_new_usb_rndis(&rndis_config, &s_rndis_driver), TAG,
                        "RNDIS driver creation failed");

    const iot_eth_config_t eth_config = {
        .driver = s_rndis_driver,
        .stack_input = NULL,
        .stack_input_info = NULL,
    };
    ESP_RETURN_ON_ERROR(iot_eth_install(&eth_config, &s_rndis_handle), TAG,
                        "RNDIS Ethernet install failed");

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_rndis_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_rndis_netif != NULL, ESP_ERR_NO_MEM, TAG,
                        "RNDIS esp-netif creation failed");
    s_rndis_glue = iot_eth_new_netif_glue(s_rndis_handle);
    ESP_RETURN_ON_FALSE(s_rndis_glue != NULL, ESP_ERR_NO_MEM, TAG,
                        "RNDIS netif glue creation failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_rndis_netif, s_rndis_glue), TAG,
                        "RNDIS esp-netif attach failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IOT_ETH_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    rndis_event_handler,
                                                    NULL),
                        TAG, "RNDIS event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                    IP_EVENT_ETH_GOT_IP,
                                                    rndis_event_handler,
                                                    NULL),
                        TAG, "RNDIS IP event registration failed");
    ESP_LOGI(TAG, "ML307A RNDIS interface prepared: waiting for AT dial stage");
    return ESP_OK;
}

static esp_err_t activate_ml307a_rndis(void)
{
    ESP_RETURN_ON_FALSE(s_rndis_handle != NULL && s_rndis_driver != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "RNDIS is not prepared");
    ESP_RETURN_ON_ERROR(iot_eth_start(s_rndis_handle), TAG,
                        "RNDIS Ethernet start failed");
    ESP_RETURN_ON_ERROR(iot_usbh_rndis_set_connection_enabled(s_rndis_driver, true),
                        TAG, "RNDIS control enable failed");
    ESP_LOGI(TAG, "ML307A RNDIS data plane enabled: control=0 data=1");
    return ESP_OK;
}

static const char *speed_name(usb_speed_t speed)
{
    switch (speed) {
        case USB_SPEED_LOW: return "low-speed";
        case USB_SPEED_FULL: return "full-speed";
        case USB_SPEED_HIGH: return "high-speed";
        default: return "unknown";
    }
}

static const char *transfer_name(uint8_t attributes)
{
    switch (attributes & 0x03U) {
        case USB_TRANSFER_TYPE_CTRL: return "control";
        case USB_TRANSFER_TYPE_ISOCHRONOUS: return "isochronous";
        case USB_TRANSFER_TYPE_BULK: return "bulk";
        case USB_TRANSFER_TYPE_INTR: return "interrupt";
        default: return "unknown";
    }
}

static const char *interface_role(const modem_interface_t *interface)
{
    if (interface->interface_number == 0 && interface->interface_class == 0xe0) {
        return "ML307A RNDIS control";
    }
    if (interface->interface_number == 1 && interface->interface_class == 0x0a) {
        return "ML307A RNDIS data";
    }
    if (interface->interface_number == 2 && interface->interface_class == 0xff) {
        return "ML307A primary modem/AT";
    }
    if (interface->interface_number == 3 && interface->interface_class == 0xff) {
        return "ML307A auxiliary AT";
    }
    if (interface->interface_number == 4 && interface->interface_class == 0xff) {
        return "ML307A diagnostic";
    }
    if (interface->interface_class == 0x0a) return "CDC data";
    if (interface->interface_class == 0xff) return "vendor-specific";
    return "other";
}

static void string_descriptor_to_ascii(const usb_str_desc_t *descriptor,
                                       char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return;
    output[0] = '\0';
    if (descriptor == NULL || descriptor->bLength < 2) return;
    size_t chars = (descriptor->bLength - 2U) / 2U;
    if (chars >= output_size) chars = output_size - 1U;
    for (size_t i = 0; i < chars; ++i) {
        uint16_t cp = descriptor->wData[i];
        output[i] = cp >= 0x20 && cp <= 0x7e ? (char)cp : '?';
    }
    output[chars] = '\0';
}

static void log_interface_summary(const modem_interface_t *interface)
{
    ESP_LOGI(TAG,
             "interface=%u alt=%u class=%02x/%02x/%02x role=%s "
             "bulk_in=0x%02x/%u bulk_out=0x%02x/%u",
             interface->interface_number, interface->alternate_setting,
             interface->interface_class, interface->interface_subclass,
             interface->interface_protocol, interface_role(interface),
             interface->bulk_in, interface->bulk_in_mps,
             interface->bulk_out, interface->bulk_out_mps);
}

static void finish_interface(modem_interface_t *current, bool have_interface,
                             modem_interface_t *at_interface, bool *at_found,
                             unsigned *candidate_count)
{
    if (!have_interface) return;
    if (current->bulk_in != 0 && current->bulk_out != 0) ++(*candidate_count);
    log_interface_summary(current);
    if (current->interface_number == ML307A_AT_INTERFACE &&
        current->alternate_setting == 0 && current->bulk_in != 0 &&
        current->bulk_out != 0) {
        *at_interface = *current;
        *at_found = true;
    }
}

static bool inspect_configuration(const usb_config_desc_t *configuration,
                                  modem_interface_t *at_interface)
{
    ESP_LOGI(TAG, "configuration value=%u interfaces=%u total_length=%u",
             configuration->bConfigurationValue, configuration->bNumInterfaces,
             configuration->wTotalLength);
    modem_interface_t current = {0};
    bool have_interface = false;
    bool at_found = false;
    unsigned candidate_count = 0;
    int offset = 0;
    const usb_standard_desc_t *descriptor =
        (const usb_standard_desc_t *)configuration;

    while (descriptor != NULL) {
        if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
            descriptor->bLength >= sizeof(usb_intf_desc_t)) {
            finish_interface(&current, have_interface, at_interface, &at_found,
                             &candidate_count);
            const usb_intf_desc_t *interface = (const usb_intf_desc_t *)descriptor;
            memset(&current, 0, sizeof(current));
            current.interface_number = interface->bInterfaceNumber;
            current.alternate_setting = interface->bAlternateSetting;
            current.interface_class = interface->bInterfaceClass;
            current.interface_subclass = interface->bInterfaceSubClass;
            current.interface_protocol = interface->bInterfaceProtocol;
            have_interface = true;
        } else if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   descriptor->bLength >= sizeof(usb_ep_desc_t)) {
            const usb_ep_desc_t *endpoint = (const usb_ep_desc_t *)descriptor;
            uint16_t mps = endpoint->wMaxPacketSize & USB_W_MAX_PACKET_SIZE_MPS_MASK;
            ESP_LOGI(TAG, "  endpoint=0x%02x direction=%s type=%s mps=%u interval=%u",
                     endpoint->bEndpointAddress,
                     (endpoint->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
                         ? "IN" : "OUT",
                     transfer_name(endpoint->bmAttributes), mps,
                     endpoint->bInterval);
            if (have_interface &&
                (endpoint->bmAttributes & 0x03U) == USB_TRANSFER_TYPE_BULK) {
                if (endpoint->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                    current.bulk_in = endpoint->bEndpointAddress;
                    current.bulk_in_mps = mps;
                } else {
                    current.bulk_out = endpoint->bEndpointAddress;
                    current.bulk_out_mps = mps;
                }
            }
        }
        descriptor = usb_parse_next_descriptor(
            descriptor, configuration->wTotalLength, &offset);
    }
    finish_interface(&current, have_interface, at_interface, &at_found,
                     &candidate_count);
    ESP_LOGI(TAG, "descriptor scan complete: candidates=%u AT-interface=%s",
             candidate_count, at_found ? "found" : "not-found");
    return at_found;
}

static bool response_has_terminal(const char *response)
{
    return strstr(response, "OK\r\n") != NULL ||
           strstr(response, "ERROR\r\n") != NULL ||
           strstr(response, "+CME ERROR:") != NULL ||
           strstr(response, "+CMS ERROR:") != NULL;
}

static bool response_has_active_mdialup(const char *response)
{
    const char *line = strstr(response, "+MDIALUP: 1,1,\"");
    return line != NULL && line[strlen("+MDIALUP: 1,1,\"")] != '\"';
}

static void signal_rndis_handoff(void)
{
    if (s_at_handoff_complete) return;
    s_at_handoff_complete = true;
    // The raw AT client owns the same device/control endpoint as the CDC
    // implementation used by RNDIS.  Releasing only interface 2 is not
    // sufficient: the client must be deregistered before RNDIS starts its
    // first encapsulated-command request.  client_task performs that final
    // teardown and signals PROBE_DIAL_READY_BIT afterwards.
    ESP_LOGI(TAG, "ML307A AT stage released; handing USB control endpoint to RNDIS");
}

static void at_rx_callback(usb_transfer_t *transfer)
{
    ml307a_at_port_t *port = (ml307a_at_port_t *)transfer->context;
    port->rx_in_flight = false;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (transfer->actual_num_bytes > 0) {
            size_t count = (size_t)transfer->actual_num_bytes;
            size_t available = sizeof(port->response) - 1U - port->response_length;
            if (count > available) {
                size_t keep = port->response_length > 256U ? 256U : port->response_length;
                memmove(port->response,
                        port->response + port->response_length - keep, keep);
                port->response_length = keep;
                available = sizeof(port->response) - 1U - keep;
            }
            if (count > available) count = available;
            memcpy(port->response + port->response_length,
                   transfer->data_buffer, count);
            port->response_length += count;
            port->response[port->response_length] = '\0';
            ESP_LOGI(TAG, "AT RX (%d): %.*s", transfer->actual_num_bytes,
                     transfer->actual_num_bytes,
                     (const char *)transfer->data_buffer);
            const unsigned command_count =
                sizeof(s_at_commands) / sizeof(s_at_commands[0]);
            if (port->response_deadline_us != 0 &&
                port->command_index < command_count &&
                response_has_terminal(port->response)) {
                port->response_done = true;
                if (port->command_index == AT_COMMAND_DIAL_QUERY_AFTER ||
                    (port->command_index == AT_COMMAND_DIAL_QUERY_BEFORE &&
                     response_has_active_mdialup(port->response))) {
                    // Do not resubmit the final AT RX transfer.  The next
                    // owner is RNDIS, so interface 2 must become releasable.
                    port->handoff_pending = true;
                    port->active = false;
                }
            }
        }
    } else if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
               transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "AT RX transfer status=%d", transfer->status);
    }

    if (!port->gone && port->active) {
        transfer->num_bytes = transfer->data_buffer_size;
        esp_err_t ret = usb_host_transfer_submit(transfer);
        if (ret == ESP_OK) {
            port->rx_in_flight = true;
        } else {
            ESP_LOGE(TAG, "AT RX resubmit failed: %s", esp_err_to_name(ret));
        }
    }
}

static void at_tx_callback(usb_transfer_t *transfer)
{
    ml307a_at_port_t *port = (ml307a_at_port_t *)transfer->context;
    port->tx_in_flight = false;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED &&
        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "AT TX transfer status=%d", transfer->status);
    }
}

static void at_control_callback(usb_transfer_t *transfer)
{
    ml307a_at_port_t *port = (ml307a_at_port_t *)transfer->context;
    port->control_in_flight = false;
    port->control_success = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
    port->control_done = true;
    if (!port->control_success &&
        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "AT control stage=%u status=%d", port->control_stage,
                 transfer->status);
    }
}

static esp_err_t submit_serial_control(ml307a_at_port_t *port)
{
    usb_setup_packet_t *setup =
        (usb_setup_packet_t *)port->control_transfer->data_buffer;
    memset(port->control_transfer->data_buffer, 0,
           port->control_transfer->data_buffer_size);
    setup->bmRequestType = 0x21;  // Host-to-device, class, interface.
    setup->wIndex = port->interface.interface_number;
    if (port->control_stage == 0) {
        setup->bRequest = 0x20;  // SET_LINE_CODING
        setup->wLength = 7;
        uint8_t *line = port->control_transfer->data_buffer + sizeof(*setup);
        line[0] = 0x00;  // 115200 baud, little endian.
        line[1] = 0xc2;
        line[2] = 0x01;
        line[3] = 0x00;
        line[4] = 0;     // One stop bit.
        line[5] = 0;     // No parity.
        line[6] = 8;     // Eight data bits.
        port->control_transfer->num_bytes = sizeof(*setup) + 7;
        ESP_LOGI(TAG, "AT serial init: SET_LINE_CODING 115200 8N1 interface=%u",
                 port->interface.interface_number);
    } else {
        setup->bRequest = 0x22;  // SET_CONTROL_LINE_STATE
        setup->wValue = 0x0003;  // DTR | RTS
        setup->wLength = 0;
        port->control_transfer->num_bytes = sizeof(*setup);
        ESP_LOGI(TAG, "AT serial init: SET_CONTROL_LINE_STATE DTR|RTS interface=%u",
                 port->interface.interface_number);
    }
    port->control_done = false;
    esp_err_t ret = usb_host_transfer_submit_control(s_client,
                                                     port->control_transfer);
    if (ret == ESP_OK) port->control_in_flight = true;
    return ret;
}

static esp_err_t submit_at_command(ml307a_at_port_t *port)
{
    const unsigned command_count = sizeof(s_at_commands) / sizeof(s_at_commands[0]);
    if (!port->active || port->gone || port->tx_in_flight ||
        port->command_index >= command_count) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *command = s_at_commands[port->command_index];
    const size_t length = strlen(command);
    memcpy(port->tx_transfer->data_buffer, command, length);
    port->tx_transfer->num_bytes = length;
    port->response_length = 0;
    port->response[0] = '\0';
    port->response_done = false;
    esp_err_t ret = usb_host_transfer_submit(port->tx_transfer);
    if (ret == ESP_OK) {
        port->tx_in_flight = true;
        port->response_deadline_us = esp_timer_get_time() + AT_RESPONSE_TIMEOUT_US;
        ESP_LOGI(TAG, "AT TX [%u/%u] attempt=%u: %.*s", port->command_index + 1,
                 command_count, port->retry_count + 1, (int)(length - 2U), command);
    }
    return ret;
}

static void release_at_port(void)
{
    if (s_at.device != NULL) {
        if (s_at.claimed) {
            esp_err_t ret = usb_host_interface_release(
                s_client, s_at.device, s_at.interface.interface_number);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "release AT interface failed: %s", esp_err_to_name(ret));
            }
        }
        esp_err_t ret = usb_host_device_close(s_client, s_at.device);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "close ML307A failed: %s", esp_err_to_name(ret));
        }
    }
    if (s_at.rx_transfer != NULL) usb_host_transfer_free(s_at.rx_transfer);
    if (s_at.tx_transfer != NULL) usb_host_transfer_free(s_at.tx_transfer);
    if (s_at.control_transfer != NULL) {
        usb_host_transfer_free(s_at.control_transfer);
    }
    memset(&s_at, 0, sizeof(s_at));
}

static esp_err_t start_ml307a_at(usb_device_handle_t device,
                                const modem_interface_t *interface)
{
    s_at.device = device;
    s_at.interface = *interface;
    esp_err_t ret = usb_host_interface_claim(s_client, device,
                                             interface->interface_number,
                                             interface->alternate_setting);
    if (ret != ESP_OK) return ret;
    s_at.active = true;
    s_at.claimed = true;

    size_t rx_size = (size_t)interface->bulk_in_mps * 4U;
    if (rx_size < 256U) rx_size = 256U;
    ret = usb_host_transfer_alloc(rx_size, 0, &s_at.rx_transfer);
    if (ret != ESP_OK) return ret;
    ret = usb_host_transfer_alloc(256, 0, &s_at.tx_transfer);
    if (ret != ESP_OK) return ret;
    ret = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 7, 0,
                                  &s_at.control_transfer);
    if (ret != ESP_OK) return ret;

    s_at.rx_transfer->device_handle = device;
    s_at.rx_transfer->bEndpointAddress = interface->bulk_in;
    s_at.rx_transfer->num_bytes = rx_size;
    s_at.rx_transfer->callback = at_rx_callback;
    s_at.rx_transfer->context = &s_at;
    s_at.tx_transfer->device_handle = device;
    s_at.tx_transfer->bEndpointAddress = interface->bulk_out;
    s_at.tx_transfer->callback = at_tx_callback;
    s_at.tx_transfer->context = &s_at;
    s_at.control_transfer->device_handle = device;
    s_at.control_transfer->bEndpointAddress = 0;
    s_at.control_transfer->callback = at_control_callback;
    s_at.control_transfer->context = &s_at;
    ESP_LOGI(TAG,
             "ML307A AT port claimed interface=%u bulk_in=0x%02x/%u "
             "bulk_out=0x%02x/%u",
             interface->interface_number, interface->bulk_in,
             interface->bulk_in_mps, interface->bulk_out,
             interface->bulk_out_mps);
    return submit_serial_control(&s_at);
}

static void inspect_device(uint8_t address)
{
    if (s_at.device != NULL || s_at_handoff_complete) return;
    usb_device_handle_t device = NULL;
    esp_err_t ret = usb_host_device_open(s_client, address, &device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "open device address=%u failed: %s", address,
                 esp_err_to_name(ret));
        return;
    }

    usb_device_info_t info = {0};
    const usb_device_desc_t *device_descriptor = NULL;
    const usb_config_desc_t *configuration = NULL;
    ret = usb_host_device_info(device, &info);
    if (ret != ESP_OK) goto done;
    ret = usb_host_get_device_descriptor(device, &device_descriptor);
    if (ret != ESP_OK) goto done;
    ret = usb_host_get_active_config_descriptor(device, &configuration);
    if (ret != ESP_OK) goto done;

    char manufacturer[64], product[64], serial[64];
    string_descriptor_to_ascii(info.str_desc_manufacturer, manufacturer,
                               sizeof(manufacturer));
    string_descriptor_to_ascii(info.str_desc_product, product, sizeof(product));
    string_descriptor_to_ascii(info.str_desc_serial_num, serial, sizeof(serial));
    ESP_LOGI(TAG,
             "device address=%u speed=%s VID:PID=%04x:%04x class=%02x/%02x/%02x",
             address, speed_name(info.speed), device_descriptor->idVendor,
             device_descriptor->idProduct, device_descriptor->bDeviceClass,
             device_descriptor->bDeviceSubClass, device_descriptor->bDeviceProtocol);
    ESP_LOGI(TAG, "strings manufacturer=\"%s\" product=\"%s\" serial=\"%s\"",
             manufacturer, product, serial);

    modem_interface_t at_interface = {0};
    bool at_found = inspect_configuration(configuration, &at_interface);
    if (device_descriptor->idVendor != ML307A_VID ||
        device_descriptor->idProduct != ML307A_PID) {
        ESP_LOGW(TAG, "device is not supported ML307A 2ECC:3012");
        goto done;
    }
    s_modem_address = address;
    if (!at_found || at_interface.bulk_in != ML307A_AT_BULK_IN ||
        at_interface.bulk_out != ML307A_AT_BULK_OUT) {
        ESP_LOGE(TAG, "ML307A AT descriptor mismatch: in=0x%02x out=0x%02x",
                 at_interface.bulk_in, at_interface.bulk_out);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    ret = start_ml307a_at(device, &at_interface);
    if (ret == ESP_OK) return;
    ESP_LOGE(TAG, "start ML307A dial stage failed: %s", esp_err_to_name(ret));
    release_at_port();
    signal_rndis_handoff();
    return;

done:
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "inspect device address=%u failed: %s", address,
                 esp_err_to_name(ret));
    }
    usb_host_device_close(s_client, device);
}

static void client_event_callback(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_pending_address = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGI(TAG, "USB device disconnected handle=%p", event->dev_gone.dev_hdl);
        if (s_at.device == event->dev_gone.dev_hdl) s_at.gone = true;
    }
}

static void service_at_state(void)
{
    const unsigned command_count = sizeof(s_at_commands) / sizeof(s_at_commands[0]);
    if (s_at.active && !s_at.gone && !s_at.data_ready && s_at.control_done) {
        if (!s_at.control_success) {
            ESP_LOGW(TAG, "AT serial control stage=%u rejected; continuing",
                     s_at.control_stage);
        }
        s_at.control_done = false;
        if (s_at.control_stage == 0) {
            s_at.control_stage = 1;
            esp_err_t ret = submit_serial_control(&s_at);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "submit DTR/RTS control failed: %s",
                         esp_err_to_name(ret));
                s_at.control_done = true;
                s_at.control_success = false;
            }
        } else {
            s_at.data_ready = true;
            s_at.rx_transfer->num_bytes = s_at.rx_transfer->data_buffer_size;
            esp_err_t ret = usb_host_transfer_submit(s_at.rx_transfer);
            if (ret == ESP_OK) {
                s_at.rx_in_flight = true;
                ret = submit_at_command(&s_at);
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "start AT data transfers failed: %s",
                         esp_err_to_name(ret));
                s_at.response_deadline_us = esp_timer_get_time();
            }
        }
    }
    if (s_at.data_ready && !s_at.gone && !s_at.tx_in_flight) {
        if (s_at.response_done) {
            s_at.response_done = false;
            s_at.response_deadline_us = 0;
            s_at.retry_count = 0;
            const unsigned completed = s_at.command_index;
            if (completed == AT_COMMAND_DIAL_QUERY_BEFORE) {
                s_at.dial_active = response_has_active_mdialup(s_at.response);
                ESP_LOGI(TAG, "ML307A RNDIS dial query: %s",
                         s_at.dial_active ? "active" : "inactive; starting dial");
                if (s_at.dial_active) {
                    s_at.handoff_pending = true;
                    s_at.active = false;
                } else {
                    ++s_at.command_index;
                }
            } else if (completed == AT_COMMAND_DIAL_START) {
                // The command's OK precedes the asynchronous PDP result.  Give
                // the modem time to finish, then verify it with a fresh query.
                ++s_at.command_index;
                s_at.next_command_not_before_us = esp_timer_get_time() + MDIALUP_SETTLE_US;
                ESP_LOGI(TAG, "ML307A RNDIS dial requested; waiting 2 seconds before verify");
            } else if (completed == AT_COMMAND_DIAL_QUERY_AFTER) {
                s_at.dial_active = response_has_active_mdialup(s_at.response);
                ESP_LOGI(TAG, "ML307A RNDIS dial result: %s",
                         s_at.dial_active ? "active" : "not confirmed");
                s_at.handoff_pending = true;
                s_at.active = false;
            } else {
                ++s_at.command_index;
            }
            s_at.response_length = 0;
            s_at.response[0] = '\0';
        } else if (s_at.response_deadline_us != 0 &&
                   esp_timer_get_time() >= s_at.response_deadline_us) {
            if (s_at.retry_count < 1) {
                ++s_at.retry_count;
                ESP_LOGW(TAG, "AT command %u timed out; retrying",
                         s_at.command_index + 1);
                submit_at_command(&s_at);
            } else {
                ESP_LOGW(TAG, "AT command %u failed after retry; continuing",
                         s_at.command_index + 1);
                s_at.retry_count = 0;
                ++s_at.command_index;
                if (s_at.command_index < command_count) {
                    submit_at_command(&s_at);
                } else {
                    s_at.response_deadline_us = 0;
                    s_at.handoff_pending = true;
                    s_at.active = false;
                }
            }
        }
    }
    if (s_at.active && s_at.data_ready && !s_at.gone && !s_at.tx_in_flight &&
        s_at.response_deadline_us == 0 && !s_at.response_done &&
        s_at.command_index < command_count &&
        esp_timer_get_time() >= s_at.next_command_not_before_us) {
        esp_err_t ret = submit_at_command(&s_at);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "submit next AT command failed: %s", esp_err_to_name(ret));
            s_at.handoff_pending = true;
            s_at.active = false;
        }
    }
    if (s_at.handoff_pending && !s_at.control_in_flight &&
        !s_at.rx_in_flight && !s_at.tx_in_flight) {
        release_at_port();
        signal_rndis_handoff();
        return;
    }
    if (s_at.gone && !s_at.control_in_flight &&
        !s_at.rx_in_flight && !s_at.tx_in_flight) {
        release_at_port();
        ESP_LOGI(TAG, "ML307A resources released; waiting for reconnect");
    }
}

static void client_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_events, PROBE_HOST_READY_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_callback,
            .callback_arg = NULL,
        },
    };
    esp_err_t ret = usb_host_client_register(&config, &s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "client registration failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    xEventGroupSetBits(s_events, PROBE_CLIENT_READY_BIT);
    ESP_LOGI(TAG, "descriptor/AT client ready; waiting for ML307A");
    while (!s_at_handoff_complete) {
        ret = usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "client event handling failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_pending_address != 0 && s_at.device == NULL) {
            uint8_t address = s_pending_address;
            s_pending_address = 0;
            inspect_device(address);
        }
        service_at_state();
    }

    // No AT transfer is in flight here: service_at_state() releases the
    // interface/device before signalling the handoff.  Drop the whole raw
    // client so its EP0 ownership and event queue cannot collide with the
    // CDC/RNDIS client's encapsulated control requests.
    ESP_LOGI(TAG, "ML307A AT client teardown before RNDIS activation");
    ret = usb_host_client_deregister(s_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT client deregistration failed: %s", esp_err_to_name(ret));
    }
    s_client = NULL;
    vTaskDelay(pdMS_TO_TICKS(AT_HANDOFF_SETTLE_MS));
    xEventGroupSetBits(s_events, PROBE_DIAL_READY_BIT);
    ESP_LOGI(TAG, "ML307A USB handoff complete; starting RNDIS data plane");
    vTaskDelete(NULL);
}

static void host_task(void *arg)
{
    (void)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = NULL,
        .fifo_settings_custom = {0},
        .peripheral_map = 0,
    };
    esp_err_t ret = usb_host_install(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB Host installed on ESP32-P4 default HS controller");
    ret = prepare_ml307a_rndis();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ML307A RNDIS preparation failed: %s", esp_err_to_name(ret));
    }
    xEventGroupSetBits(s_events, PROBE_HOST_READY_BIT);
    xEventGroupWaitBits(s_events, PROBE_CLIENT_READY_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "waiting for ML307A AT dial stage before starting RNDIS");
    // Enumeration is performed by usb_host_lib_handle_events().  Keep
    // servicing it while the separate client task owns interface 2 for AT;
    // blocking here would prevent the NEW_DEV event required to start AT.
    while ((xEventGroupGetBits(s_events) & PROBE_DIAL_READY_BIT) == 0) {
        uint32_t flags = 0;
        ret = usb_host_lib_handle_events(pdMS_TO_TICKS(20), &flags);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "host event handling during AT stage failed: %s",
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    ret = activate_ml307a_rndis();
    const int64_t rndis_start_us = esp_timer_get_time();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ML307A RNDIS startup failed: %s", esp_err_to_name(ret));
    }
    int64_t sms_retry_not_before_us = 0;
    unsigned sms_enable_failures = 0;
    bool sms_enable_issued = false;
    while (true) {
        uint32_t flags = 0;
        ret = usb_host_lib_handle_events(pdMS_TO_TICKS(20), &flags);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "host event handling failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        const int64_t now = esp_timer_get_time();
        const uint8_t current_address = iot_usbh_rndis_get_device_address(s_rndis_driver);
        if (current_address != s_modem_address) {
            s_modem_address = current_address;
            sms_enable_issued = false;
            sms_enable_failures = 0;
            sms_retry_not_before_us = 0;
            if (current_address != 0) {
                ESP_LOGI(TAG, "ML307A enumerated at USB address %u; initialization attempts reset",
                         current_address);
            }
        }
        const EventBits_t bits = xEventGroupGetBits(s_events);
        const bool rndis_ready = current_address != 0 &&
            (bits & PROBE_RNDIS_LINK_BIT) != 0 &&
            s_sms_enable_not_before_us != 0 && now >= s_sms_enable_not_before_us;
        const bool voice_fallback = !rndis_ready && current_address != 0 &&
            now - rndis_start_us >= AT_VOICE_FALLBACK_DELAY_US;
        if ((rndis_ready || voice_fallback) && !sms_enable_issued &&
            sms_enable_failures <= 3 && now >= sms_retry_not_before_us) {
            if (voice_fallback && sms_enable_failures == 0) {
                ESP_LOGW(TAG, "RNDIS not connected after 8 s; enabling independent voice/SMS AT port");
            }
            ret = ml307_sms_transport_enable(current_address);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "ML307A SMS transport enabled after RNDIS negotiation");
                sms_enable_issued = true;
            } else if (ret == ESP_ERR_NOT_FINISHED) {
                ESP_LOGI(TAG, "ML307A SMS transport initialization scheduled");
                sms_enable_issued = true;
            } else {
                ++sms_enable_failures;
                ESP_LOGW(TAG, "ML307A SMS transport request failed (%u/4): %s%s",
                         sms_enable_failures, esp_err_to_name(ret),
                         sms_enable_failures > 3 ? "; stopping until reconnect" : "; retrying");
                sms_retry_not_before_us = esp_timer_get_time() + 2000000;
            }
        }
    }
}

esp_err_t usb_modem_probe_start(void)
{
    if (s_started) return ESP_OK;
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    BaseType_t host_ok = xTaskCreatePinnedToCore(
        host_task, "usb_modem_host", PROBE_TASK_STACK, NULL, 12, NULL, 0);
    BaseType_t client_ok = xTaskCreatePinnedToCore(
        client_task, "usb_modem_probe", PROBE_TASK_STACK, NULL, 11, NULL, 0);
    if (host_ok != pdPASS || client_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create probe tasks host=%ld client=%ld",
                 (long)host_ok, (long)client_ok);
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool usb_modem_probe_is_online(void)
{
    if (!s_rndis_online || s_rndis_netif == NULL ||
        !esp_netif_is_netif_up(s_rndis_netif)) {
        return false;
    }
    esp_netif_ip_info_t ip_info = {0};
    return esp_netif_get_ip_info(s_rndis_netif, &ip_info) == ESP_OK &&
           ip_info.ip.addr != 0;
}

esp_err_t usb_modem_probe_wait_online(int timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!usb_modem_probe_is_online() && esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return usb_modem_probe_is_online() ? ESP_OK : ESP_ERR_TIMEOUT;
}
