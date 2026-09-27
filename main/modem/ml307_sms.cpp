#include "modem/ml307_sms.h"
#include "modem/ml307_voice.h"
#include "time/time_basis.h"

#include "esp_check.h"
#include "usb/usb_host.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "ML307_SMS";
constexpr uint16_t ML307A_VID = 0x2ecc;
constexpr uint16_t ML307A_PID = 0x3012;
constexpr uint8_t ML307A_AT_INTERFACE = 2;
constexpr uint8_t ML307A_AT_BULK_IN = 0x81;
constexpr uint8_t ML307A_AT_BULK_OUT = 0x0a;
constexpr EventBits_t RX_EVENT = BIT0;
constexpr EventBits_t PORT_EVENT = BIT1;
constexpr EventBits_t TX_EVENT = BIT2;
constexpr EventBits_t CLIENT_EVENT = BIT3;
constexpr size_t RX_CAPACITY = 16384;
constexpr unsigned AT_PORT_OPEN_RETRIES = 3;

SemaphoreHandle_t s_command_mutex;
SemaphoreHandle_t s_rx_mutex;
EventGroupHandle_t s_events;
usb_host_client_handle_t s_client;
usb_device_handle_t s_device;
usb_transfer_t *s_rx_transfer;
usb_transfer_t *s_tx_transfer;
std::string s_rx;
bool s_registered;
bool s_initialized;
bool s_task_started;
volatile uint8_t s_device_address;
volatile bool s_enable_requested;
std::atomic_uint s_open_attempts{0};
volatile bool s_interface_claimed;
volatile bool s_device_gone;
volatile bool s_rx_in_flight;
volatile bool s_tx_in_flight;
volatile esp_err_t s_tx_result;
bool s_sms_configured;
bool s_at_ready;
volatile bool s_new_message_hint;
std::atomic_bool s_voice_ring_hint{false};
std::string s_voice_urc_tail;

enum class SmsCharset : uint8_t {
    Ucs2,
    Hex,
    Ira,
};

SmsCharset s_sms_charset = SmsCharset::Ira;

void append_utf8(std::string *out, uint32_t cp)
{
    if (cp <= 0x7f) out->push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

bool next_utf8(const char *text, size_t length, size_t *offset, uint32_t *cp)
{
    if (*offset >= length) return false;
    const uint8_t c = static_cast<uint8_t>(text[(*offset)++]);
    if (c < 0x80) { *cp = c; return true; }
    int continuation = 0;
    uint32_t value = 0;
    if ((c & 0xe0) == 0xc0) { continuation = 1; value = c & 0x1f; }
    else if ((c & 0xf0) == 0xe0) { continuation = 2; value = c & 0x0f; }
    else if ((c & 0xf8) == 0xf0) { continuation = 3; value = c & 0x07; }
    else { *cp = 0xfffd; return true; }
    if (*offset + continuation > length) { *cp = 0xfffd; *offset = length; return true; }
    while (continuation--) {
        const uint8_t next = static_cast<uint8_t>(text[(*offset)++]);
        if ((next & 0xc0) != 0x80) { *cp = 0xfffd; return true; }
        value = (value << 6) | (next & 0x3f);
    }
    *cp = value;
    return true;
}

std::string utf8_to_ucs2_hex(const char *text, size_t *units_out = nullptr)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    const size_t length = std::strlen(text);
    size_t offset = 0;
    size_t units = 0;
    while (offset < length) {
        uint32_t cp = 0;
        if (!next_utf8(text, length, &offset, &cp)) break;
        uint16_t words[2] = {};
        int word_count = 1;
        if (cp <= 0xffff && !(cp >= 0xd800 && cp <= 0xdfff)) words[0] = cp;
        else if (cp <= 0x10ffff) {
            cp -= 0x10000;
            words[0] = static_cast<uint16_t>(0xd800 | (cp >> 10));
            words[1] = static_cast<uint16_t>(0xdc00 | (cp & 0x3ff));
            word_count = 2;
        } else words[0] = 0xfffd;
        for (int i = 0; i < word_count; ++i) {
            const uint16_t word = words[i];
            result.push_back(hex[(word >> 12) & 0xf]);
            result.push_back(hex[(word >> 8) & 0xf]);
            result.push_back(hex[(word >> 4) & 0xf]);
            result.push_back(hex[word & 0xf]);
            ++units;
        }
    }
    if (units_out) *units_out = units;
    return result;
}

std::string bytes_to_hex(const char *text)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    const size_t length = std::strlen(text);
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = static_cast<uint8_t>(text[i]);
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

void append_hex_byte(std::string *output, uint8_t value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    output->push_back(hex[value >> 4]);
    output->push_back(hex[value & 0x0f]);
}

bool make_ucs2_submit_pdu(const char *address_utf8, const char *text_utf8,
                          std::string *pdu, size_t *tpdu_bytes,
                          size_t *units_out)
{
    if (!address_utf8 || !text_utf8 || !pdu || !tpdu_bytes) return false;
    const bool international = address_utf8[0] == '+';
    std::string digits;
    for (const char *cursor = address_utf8; *cursor; ++cursor) {
        if (*cursor >= '0' && *cursor <= '9') digits.push_back(*cursor);
        else if (*cursor != '+' && *cursor != ' ' && *cursor != '-') return false;
    }
    if (digits.empty() || digits.size() > 20) return false;

    size_t units = 0;
    const std::string body = utf8_to_ucs2_hex(text_utf8, &units);
    if (units == 0 || units > 70 || body.size() / 2 > 140) return false;

    std::string result;
    result.reserve(32 + digits.size() + body.size());
    result += "00";  // Use the SIM/network default SMSC.
    result += "1100";  // SMS-SUBMIT, relative VP present, message reference 0.
    append_hex_byte(&result, static_cast<uint8_t>(digits.size()));
    append_hex_byte(&result, international ? 0x91 : 0x81);
    for (size_t i = 0; i < digits.size(); i += 2) {
        result.push_back(i + 1 < digits.size() ? digits[i + 1] : 'F');
        result.push_back(digits[i]);
    }
    result += "0008AA";  // PID=0, DCS=UCS2, relative validity period.
    append_hex_byte(&result, static_cast<uint8_t>(body.size() / 2));
    result += body;
    *tpdu_bytes = result.size() / 2 - 1;  // CMGS excludes SMSC length octet.
    if (units_out) *units_out = units;
    *pdu = std::move(result);
    return true;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_bytes(const std::string &value, std::vector<uint8_t> *bytes)
{
    if (value.empty() || value.size() % 2) return false;
    bytes->clear();
    bytes->reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        const int high = hex_value(value[i]);
        const int low = hex_value(value[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes->push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

std::string decode_ucs2(const uint8_t *data, size_t size)
{
    std::string output;
    for (size_t i = 0; i + 1 < size; i += 2) {
        uint16_t word = static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
        uint32_t cp = word;
        if (word >= 0xd800 && word <= 0xdbff && i + 3 < size) {
            uint16_t low = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
            if (low >= 0xdc00 && low <= 0xdfff) {
                cp = 0x10000 + ((word - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            }
        }
        if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
        append_utf8(&output, cp);
    }
    return output;
}

// 3GPP TS 23.038 default alphabet, indexed by septet value.
constexpr uint16_t GSM7[] = {
    '@', 0x00a3, '$', 0x00a5, 0x00e8, 0x00e9, 0x00f9, 0x00ec,
    0x00f2, 0x00c7, '\n', 0x00d8, 0x00f8, '\r', 0x00c5, 0x00e5,
    0x0394, '_', 0x03a6, 0x0393, 0x039b, 0x03a9, 0x03a0, 0x03a8,
    0x03a3, 0x0398, 0x039e, 0, 0x00c6, 0x00e6, 0x00df, 0x00c9,
    ' ', '!', '"', '#', 0x00a4, '%', '&', '\'',
    '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', ':', ';', '<', '=', '>', '?',
    0x00a1, 'A', 'B', 'C', 'D', 'E', 'F', 'G',
    'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',
    'X', 'Y', 'Z', 0x00c4, 0x00d6, 0x00d1, 0x00dc, 0x00a7,
    0x00bf, 'a', 'b', 'c', 'd', 'e', 'f', 'g',
    'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w',
    'x', 'y', 'z', 0x00e4, 0x00f6, 0x00f1, 0x00fc, 0x00e0,
};

uint32_t gsm7_extension(uint8_t code)
{
    switch (code) {
    case 0x0a: return '\f';
    case 0x14: return '^';
    case 0x28: return '{';
    case 0x29: return '}';
    case 0x2f: return '\\';
    case 0x3c: return '[';
    case 0x3d: return '~';
    case 0x3e: return ']';
    case 0x40: return '|';
    case 0x65: return 0x20ac;
    default: return ' ';
    }
}

std::string decode_gsm7(const uint8_t *data, size_t size, size_t septets,
                        size_t skip_septets = 0)
{
    std::string result;
    bool escaped = false;
    for (size_t i = skip_septets; i < septets; ++i) {
        const size_t bit = i * 7;
        if (bit + 7 > size * 8) break;
        const uint8_t code = static_cast<uint8_t>(
            ((static_cast<unsigned>(data[bit / 8]) >> (bit % 8)) |
             (bit / 8 + 1 < size ? static_cast<unsigned>(data[bit / 8 + 1]) <<
                                          (8 - bit % 8) : 0)) & 0x7f);
        if (escaped) {
            append_utf8(&result, gsm7_extension(code));
            escaped = false;
        } else if (code == 0x1b) {
            escaped = true;
        } else {
            append_utf8(&result, GSM7[code]);
        }
    }
    return result;
}

void copy_field(char *target, size_t capacity, const std::string &value)
{
    if (!target || capacity == 0) return;
    std::snprintf(target, capacity, "%s", value.c_str());
}

bool normalize_sms_time(const std::string &value, char *display,
                        size_t capacity, int64_t *sort_key)
{
    int64_t utc_seconds = 0;
    return time_basis_parse_modem(value.c_str(), &utc_seconds) &&
           time_basis_format_utc(utc_seconds, display, capacity, sort_key);
}

int swapped_bcd(uint8_t value)
{
    const int low = value & 0x0f;
    const int high = value >> 4;
    return low <= 9 && high <= 9 ? low * 10 + high : -1;
}

bool pdu_address(const std::vector<uint8_t> &pdu, size_t *cursor,
                 std::string *address)
{
    if (*cursor + 2 > pdu.size()) return false;
    const size_t digits = pdu[(*cursor)++];
    const uint8_t type = pdu[(*cursor)++];
    const bool alpha = (type & 0x70) == 0x50;
    // TP-OA length is in useful semi-octets even for alphanumeric IDs.
    const size_t octets = (digits + 1) / 2;
    if (*cursor + octets > pdu.size()) return false;
    if (alpha) {
        *address = decode_gsm7(pdu.data() + *cursor, octets, digits * 4 / 7);
    } else {
        address->clear();
        if ((type & 0x70) == 0x10) address->push_back('+');
        for (size_t i = 0; i < digits; ++i) {
            const uint8_t nibble = i % 2 ? pdu[*cursor + i / 2] >> 4 :
                                          pdu[*cursor + i / 2] & 0x0f;
            static constexpr char dial[] = "0123456789*#abc";
            if (nibble >= sizeof(dial) - 1) return false;
            address->push_back(dial[nibble]);
        }
    }
    *cursor += octets;
    return true;
}

bool pdu_timestamp(const uint8_t *scts, ml307_sms_message_t *item)
{
    const int yy = swapped_bcd(scts[0]);
    const int month = swapped_bcd(scts[1]);
    const int day = swapped_bcd(scts[2]);
    const int hour = swapped_bcd(scts[3]);
    const int minute = swapped_bcd(scts[4]);
    const int second = swapped_bcd(scts[5]);
    const uint8_t zone = scts[6];
    const int quarters = (zone & 0x07) * 10 + (zone >> 4);
    if (yy < 0 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || quarters > 96) return false;
    char raw[32];
    std::snprintf(raw, sizeof(raw), "%02d/%02d/%02d,%02d:%02d:%02d%c%02d",
                  yy, month, day, hour, minute, second,
                  zone & 0x08 ? '-' : '+', quarters);
    return normalize_sms_time(raw, item->timestamp, sizeof(item->timestamp),
                              &item->sort_key);
}

bool parse_pdu(const std::string &hex, ml307_sms_message_t *item)
{
    std::vector<uint8_t> pdu;
    if (!hex_bytes(hex, &pdu) || pdu.empty()) return false;
    size_t cursor = 0;
    const size_t smsc_octets = pdu[cursor++];
    if (cursor + smsc_octets >= pdu.size()) return false;
    cursor += smsc_octets;
    const uint8_t first = pdu[cursor++];
    const uint8_t mti = first & 0x03;
    if (mti != 0 && mti != 1) return false;
    item->outgoing = mti == 1;
    if (item->outgoing) {
        if (cursor >= pdu.size()) return false;
        ++cursor; // TP-MR
    }
    std::string address;
    if (!pdu_address(pdu, &cursor, &address) || cursor + 2 > pdu.size()) return false;
    copy_field(item->address, sizeof(item->address), address);
    ++cursor; // TP-PID
    const uint8_t dcs = pdu[cursor++];
    if (!item->outgoing) {
        if (cursor + 7 > pdu.size()) return false;
        if (!pdu_timestamp(pdu.data() + cursor, item)) {
            copy_field(item->timestamp, sizeof(item->timestamp), "Time unknown");
        }
        cursor += 7;
    } else {
        const uint8_t vpf = (first >> 3) & 0x03;
        const size_t vp_octets = vpf == 2 ? 1 : (vpf == 1 || vpf == 3 ? 7 : 0);
        if (cursor + vp_octets > pdu.size()) return false;
        cursor += vp_octets;
        copy_field(item->timestamp, sizeof(item->timestamp), "Time unknown");
    }
    if (cursor >= pdu.size()) return false;
    const size_t udl = pdu[cursor++];
    const uint8_t *ud = pdu.data() + cursor;
    const size_t available = pdu.size() - cursor;
    const bool has_header = (first & 0x40) != 0;
    const size_t header_octets = has_header && available ? static_cast<size_t>(ud[0]) + 1 : 0;
    if (header_octets > available) return false;
    std::string body;
    // DCS general coding and message-class groups; 0xF* also uses GSM-7/8-bit.
    const bool general = (dcs & 0xc0) == 0;
    const bool class_group = (dcs & 0xf0) == 0xf0;
    const int alphabet = general && (dcs & 0x20) ? 3 :
                         general ? ((dcs >> 2) & 0x03) :
                         class_group ? ((dcs & 0x04) ? 1 : 0) :
                         (dcs & 0xf0) == 0xe0 ? 2 :
                         (dcs & 0xf0) == 0xc0 || (dcs & 0xf0) == 0xd0 ? 0 : 3;
    if (alphabet == 0) {
        const size_t octets = (udl * 7 + 7) / 8;
        if (octets > available) return false;
        const size_t skip = (header_octets * 8 + 6) / 7;
        body = decode_gsm7(ud, octets, udl, skip);
    } else if (alphabet == 2) {
        if (udl > available) return false;
        body = decode_ucs2(ud + header_octets, udl - header_octets);
    } else if (alphabet == 1) {
        if (udl > available) return false;
        bool printable = true;
        for (size_t i = header_octets; i < udl; ++i) {
            if ((ud[i] < 0x20 && ud[i] != '\n' && ud[i] != '\r' && ud[i] != '\t') ||
                ud[i] > 0x7e) {
                printable = false;
                break;
            }
        }
        body = printable ? std::string(reinterpret_cast<const char *>(ud + header_octets),
                                       udl - header_octets) : "[Binary SMS]";
    } else {
        body = "[Unsupported SMS encoding]";
    }
    copy_field(item->text, sizeof(item->text), body);
    return true;
}

void rx_callback(usb_transfer_t *transfer)
{
    s_rx_in_flight = false;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes > 0) {
        const size_t length = static_cast<size_t>(transfer->actual_num_bytes);
        if (xSemaphoreTake(s_rx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_rx.size() + length > RX_CAPACITY) s_rx.erase(0, s_rx.size() / 2);
            s_rx.append(reinterpret_cast<const char *>(transfer->data_buffer), length);
            if (s_rx.find("+CMTI:") != std::string::npos) s_new_message_hint = true;
            s_voice_urc_tail.append(reinterpret_cast<const char *>(transfer->data_buffer), length);
            if (s_voice_urc_tail.find("RING\r\n") != std::string::npos ||
                s_voice_urc_tail.find("+CRING:") != std::string::npos) {
                s_voice_ring_hint.store(true, std::memory_order_relaxed);
                s_voice_urc_tail.clear();
            } else if (s_voice_urc_tail.size() > 96) {
                s_voice_urc_tail.erase(0, s_voice_urc_tail.size() - 96);
            }
            xSemaphoreGive(s_rx_mutex);
            xEventGroupSetBits(s_events, RX_EVENT);
        } else {
            ESP_LOGW(TAG, "dropping %u AT RX bytes: response mutex busy",
                     static_cast<unsigned>(length));
        }
    } else if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
               transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "native AT RX failed status=%d", transfer->status);
    }

    if (!s_device_gone && s_interface_claimed) {
        transfer->num_bytes = transfer->data_buffer_size;
        const esp_err_t error = usb_host_transfer_submit(transfer);
        if (error == ESP_OK) s_rx_in_flight = true;
        else ESP_LOGE(TAG, "native AT RX resubmit failed: %s",
                      esp_err_to_name(error));
    }
}

void tx_callback(usb_transfer_t *transfer)
{
    s_tx_in_flight = false;
    s_tx_result = transfer->status == USB_TRANSFER_STATUS_COMPLETED
        ? ESP_OK
        : (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE
               ? ESP_ERR_INVALID_STATE : ESP_FAIL);
    xEventGroupSetBits(s_events, TX_EVENT);
}

void client_event(const usb_host_client_event_msg_t *event, void *)
{
    if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               event->dev_gone.dev_hdl == s_device) {
        s_device_gone = true;
        s_enable_requested = false;
        s_device_address = 0;
        s_sms_configured = false;
        s_at_ready = false;
        xEventGroupClearBits(s_events, PORT_EVENT);
        ESP_LOGW(TAG, "ML307A native SMS AT device disconnected");
    }
}

void close_native_port()
{
    if (s_device && s_interface_claimed && !s_device_gone) {
        const esp_err_t error = usb_host_interface_release(
            s_client, s_device, ML307A_AT_INTERFACE);
        if (error != ESP_OK) ESP_LOGW(TAG, "release native AT interface failed: %s",
                                      esp_err_to_name(error));
    }
    s_interface_claimed = false;
    if (s_rx_transfer) usb_host_transfer_free(s_rx_transfer);
    if (s_tx_transfer) usb_host_transfer_free(s_tx_transfer);
    s_rx_transfer = nullptr;
    s_tx_transfer = nullptr;
    if (s_device) {
        const esp_err_t error = usb_host_device_close(s_client, s_device);
        if (error != ESP_OK) ESP_LOGW(TAG, "close native AT device failed: %s",
                                      esp_err_to_name(error));
    }
    s_device = nullptr;
    s_device_gone = false;
    s_sms_configured = false;
    s_at_ready = false;
    xEventGroupClearBits(s_events, PORT_EVENT);
}

esp_err_t open_native_port(uint8_t device_address)
{
    if (s_device && s_interface_claimed) return ESP_OK;
    esp_err_t error = usb_host_device_open(s_client, device_address, &s_device);
    if (error != ESP_OK) return error;

    const usb_device_desc_t *descriptor = nullptr;
    error = usb_host_get_device_descriptor(s_device, &descriptor);
    if (error != ESP_OK || !descriptor || descriptor->idVendor != ML307A_VID ||
        descriptor->idProduct != ML307A_PID) {
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
        return error == ESP_OK ? ESP_ERR_NOT_SUPPORTED : error;
    }
    error = usb_host_interface_claim(s_client, s_device, ML307A_AT_INTERFACE, 0);
    if (error != ESP_OK) goto fail;
    s_interface_claimed = true;

    error = usb_host_transfer_alloc(2048, 0, &s_rx_transfer);
    if (error != ESP_OK) goto fail;
    error = usb_host_transfer_alloc(512, 0, &s_tx_transfer);
    if (error != ESP_OK) goto fail;

    s_rx_transfer->device_handle = s_device;
    s_rx_transfer->bEndpointAddress = ML307A_AT_BULK_IN;
    s_rx_transfer->num_bytes = s_rx_transfer->data_buffer_size;
    s_rx_transfer->callback = rx_callback;
    s_rx_transfer->context = nullptr;
    s_tx_transfer->device_handle = s_device;
    s_tx_transfer->bEndpointAddress = ML307A_AT_BULK_OUT;
    s_tx_transfer->callback = tx_callback;
    s_tx_transfer->context = nullptr;

    s_device_gone = false;
    s_rx_in_flight = true;
    error = usb_host_transfer_submit(s_rx_transfer);
    if (error != ESP_OK) {
        s_rx_in_flight = false;
        goto fail;
    }
    xEventGroupSetBits(s_events, PORT_EVENT);
    ESP_LOGI(TAG, "native SMS AT port ready: addr=%u interface=%u in=0x%02x out=0x%02x",
             device_address, ML307A_AT_INTERFACE, ML307A_AT_BULK_IN,
             ML307A_AT_BULK_OUT);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "open native SMS AT port failed: %s", esp_err_to_name(error));
    close_native_port();
    return error;
}

void transport_task(void *)
{
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 4,
        .async = {
            .client_event_callback = client_event,
            .callback_arg = nullptr,
        },
    };
    const esp_err_t registered = usb_host_client_register(&config, &s_client);
    if (registered != ESP_OK) {
        ESP_LOGE(TAG, "native SMS USB client registration failed: %s",
                 esp_err_to_name(registered));
        xEventGroupSetBits(s_events, CLIENT_EVENT);
        vTaskDelete(nullptr);
        return;
    }
    s_registered = true;
    xEventGroupSetBits(s_events, CLIENT_EVENT);
    ESP_LOGI(TAG, "native SMS USB client registered");

    while (true) {
        const esp_err_t error = usb_host_client_handle_events(
            s_client, pdMS_TO_TICKS(20));
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "native SMS client event error: %s",
                     esp_err_to_name(error));
        }
        if (s_device_gone && !s_rx_in_flight && !s_tx_in_flight) {
            close_native_port();
        }
        if (s_enable_requested && !s_device && s_device_address != 0) {
            const uint8_t address = s_device_address;
            const esp_err_t open_error = open_native_port(address);
            if (open_error == ESP_OK) {
                s_open_attempts.store(0, std::memory_order_relaxed);
            } else if (s_enable_requested && s_device_address == address) {
                const unsigned attempt = s_open_attempts.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                if (attempt > AT_PORT_OPEN_RETRIES) {
                    s_enable_requested = false;
                    ESP_LOGE(TAG, "native AT port failed after %u attempts; waiting for a new USB device",
                             attempt);
                } else {
                    ESP_LOGW(TAG, "native AT port attempt %u/%u failed: %s; retrying",
                             attempt, AT_PORT_OPEN_RETRIES + 1, esp_err_to_name(open_error));
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
        }
    }
}

bool response_complete(const std::string &response, bool prompt)
{
    if (prompt && response.find('>') != std::string::npos) return true;
    return response.find("\r\nOK\r\n") != std::string::npos ||
           response.find("\nOK\r") != std::string::npos ||
           response.find("\r\nERROR") != std::string::npos ||
           response.find("+CMS ERROR:") != std::string::npos ||
           response.find("+CME ERROR:") != std::string::npos;
}

esp_err_t command_locked(const uint8_t *command, size_t command_size,
                         std::string *response, int timeout_ms, bool prompt,
                         bool quiet_timeout = false)
{
    if (!s_device || !s_interface_claimed ||
        (xEventGroupGetBits(s_events) & PORT_EVENT) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (command_size > s_tx_transfer->data_buffer_size) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(s_rx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_rx.clear();
    xSemaphoreGive(s_rx_mutex);
    xEventGroupClearBits(s_events, RX_EVENT | TX_EVENT);
    std::memcpy(s_tx_transfer->data_buffer, command, command_size);
    s_tx_transfer->num_bytes = command_size;
    s_tx_result = ESP_ERR_NOT_FINISHED;
    s_tx_in_flight = true;
    esp_err_t write_error = usb_host_transfer_submit(s_tx_transfer);
    if (write_error != ESP_OK) {
        s_tx_in_flight = false;
        return write_error;
    }
    const EventBits_t tx_bits = xEventGroupWaitBits(
        s_events, TX_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    if ((tx_bits & TX_EVENT) == 0) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(s_tx_result, TAG, "native AT bulk write failed");
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    std::string snapshot;
    while (esp_timer_get_time() < deadline) {
        xEventGroupWaitBits(s_events, RX_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
        if (xSemaphoreTake(s_rx_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            snapshot = s_rx;
            xSemaphoreGive(s_rx_mutex);
        }
        if (response_complete(snapshot, prompt)) break;
        if (!s_device || !s_interface_claimed) return ESP_ERR_INVALID_STATE;
    }
    if (response) *response = snapshot;
    if (!response_complete(snapshot, prompt)) {
        if (!quiet_timeout) {
            ESP_LOGW(TAG, "AT response timeout after %d ms, received=%u bytes: %.*s",
                     timeout_ms, static_cast<unsigned>(snapshot.size()),
                     static_cast<int>(std::min<size_t>(snapshot.size(), 160)),
                     snapshot.c_str());
        }
        return ESP_ERR_TIMEOUT;
    }
    if (snapshot.find("ERROR") != std::string::npos) {
        ESP_LOGW(TAG, "AT command rejected: %.*s",
                 static_cast<int>(std::min<size_t>(snapshot.size(), 160)),
                 snapshot.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t command_locked(const char *command, std::string *response,
                         int timeout_ms = 5000, bool prompt = false,
                         bool quiet_timeout = false)
{
    return command_locked(reinterpret_cast<const uint8_t *>(command),
                          std::strlen(command), response, timeout_ms, prompt,
                          quiet_timeout);
}

esp_err_t ensure_at_locked()
{
    if (s_at_ready) return ESP_OK;
    std::string response;
    // The ML307A boot-time probe uses CRLF successfully.  Keep the persistent
    // bulk AT channel on the same framing; some firmware revisions do not
    // dispatch a command terminated by CR alone on this USB interface.
    // The first command after claiming the bulk interface acts as a wake-up on
    // this firmware and may intentionally receive no reply. Keep it short and
    // quiet, then issue the real handshake.
    esp_err_t handshake = command_locked("AT\r\n", &response, 500, false, true);
    if (handshake != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_RETURN_ON_ERROR(command_locked("AT\r\n", &response, 3000), TAG,
                            "AT handshake failed");
    }
    ESP_RETURN_ON_ERROR(command_locked("ATE0\r\n", &response, 3000), TAG,
                        "ATE0 failed");
    ESP_RETURN_ON_ERROR(command_locked("AT+CMEE=2\r\n", &response), TAG, "CMEE failed");
    s_at_ready = true;
    return ESP_OK;
}

esp_err_t ensure_sms_locked()
{
    if (s_sms_configured) return ESP_OK;
    ESP_RETURN_ON_ERROR(ensure_at_locked(), TAG, "AT handshake failed");
    std::string response;
    // Text mode on this firmware returns a mixture of plain text and hex, and
    // its IRA-only TE charset loses the original DCS. PDU mode preserves the
    // address type, alphabet and user-data header for reliable decoding.
    ESP_RETURN_ON_ERROR(command_locked("AT+CMGF=0\r\n", &response), TAG, "PDU mode failed");
    s_sms_charset = SmsCharset::Ira;
    esp_err_t storage = command_locked("AT+CPMS=\"SM\",\"SM\",\"SM\"\r\n", &response);
    if (storage != ESP_OK) {
        ESP_RETURN_ON_ERROR(command_locked("AT+CPMS=\"ME\",\"ME\",\"ME\"\r\n", &response),
                            TAG, "SMS storage selection failed");
    }
    ESP_RETURN_ON_ERROR(command_locked("AT+CNMI=2,1,0,0,0\r\n", &response), TAG,
                        "new-message notification setup failed");
    s_sms_configured = true;
    const char *charset_name = s_sms_charset == SmsCharset::Ucs2 ? "UCS2" :
        (s_sms_charset == SmsCharset::Hex ? "HEX" : "IRA");
    ESP_LOGI(TAG, "SMS receive mode ready: PDU charset=%s send=%s", charset_name,
             s_sms_charset == SmsCharset::Ira ? "PDU/UCS2" : "text/UCS2");
    return ESP_OK;
}

void parse_list(const std::string &response, ml307_sms_message_t *messages,
                size_t capacity, size_t *count, const char *prefix)
{
    *count = 0;
    size_t cursor = 0;
    int pending_index = -1;
    int pending_status = -1;
    while (cursor <= response.size()) {
        size_t end = response.find('\n', cursor);
        if (end == std::string::npos) end = response.size();
        std::string line = response.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        cursor = end + 1;
        if (line.rfind(prefix, 0) == 0) {
            if (std::strcmp(prefix, "+CMGL:") == 0) {
                if (std::sscanf(line.c_str(), "+CMGL: %d,%d", &pending_index,
                                &pending_status) != 2) pending_index = -1;
            } else {
                pending_index = 0;
                if (std::sscanf(line.c_str(), "+CMGR: %d", &pending_status) != 1) {
                    pending_index = -1;
                }
            }
        } else if (pending_index >= 0 && *count < capacity) {
            ml307_sms_message_t item = {};
            item.index = pending_index;
            item.unread = pending_status == 0;
            if (parse_pdu(line, &item)) {
                if (!item.sort_key) item.sort_key = pending_index;
                messages[(*count)++] = item;
                pending_index = -1;
            }
        }
    }
}

struct CommandGuard {
    bool locked = false;
    CommandGuard() { locked = s_command_mutex &&
        xSemaphoreTake(s_command_mutex, pdMS_TO_TICKS(15000)) == pdTRUE; }
    ~CommandGuard() { if (locked) xSemaphoreGive(s_command_mutex); }
};

}  // namespace

extern "C" esp_err_t ml307_sms_transport_init(void)
{
    if (s_initialized) return ESP_OK;
    s_command_mutex = xSemaphoreCreateMutex();
    s_rx_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_command_mutex || !s_rx_mutex || !s_events) return ESP_ERR_NO_MEM;
    s_initialized = true;
    ESP_LOGI(TAG, "native SMS transport prepared; USB client deferred until RNDIS link-up");
    return ESP_OK;
}

extern "C" bool ml307_sms_is_ready(void)
{
    return s_device != nullptr && s_interface_claimed &&
           (xEventGroupGetBits(s_events) & PORT_EVENT) != 0;
}

extern "C" esp_err_t ml307_sms_transport_enable(uint8_t device_address)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (ml307_sms_is_ready()) return ESP_OK;
    if (device_address == 0) return ESP_ERR_INVALID_ARG;
    if (!s_enable_requested || s_device_address != device_address) {
        s_open_attempts.store(0, std::memory_order_relaxed);
    }
    s_device_address = device_address;
    s_enable_requested = true;
    if (!s_task_started) {
        s_task_started = true;
        const BaseType_t created = xTaskCreatePinnedToCore(
            transport_task, "ml307_sms_usb", 4096, nullptr, 9, nullptr, 0);
        if (created != pdPASS) {
            s_task_started = false;
            s_enable_requested = false;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_ERR_NOT_FINISHED;
}

extern "C" bool ml307_sms_take_new_message_hint(void)
{
    const bool value = s_new_message_hint;
    s_new_message_hint = false;
    return value;
}

extern "C" bool ml307_voice_take_ring_hint(void)
{
    return s_voice_ring_hint.exchange(false, std::memory_order_relaxed);
}

extern "C" esp_err_t ml307_voice_query(ml307_call_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    *info = {};
    info->state = ML307_CALL_IDLE;
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_at_locked(), TAG, "voice AT handshake failed");
    std::string response;
    ESP_RETURN_ON_ERROR(command_locked("AT+CLCC\r\n", &response, 3000), TAG,
                        "voice status failed");
    const size_t marker = response.find("+CLCC:");
    if (marker == std::string::npos) return ESP_OK;
    int index = 0, direction = 0, status = 0, mode = 0, multiparty = 0;
    if (std::sscanf(response.c_str() + marker, "+CLCC: %d,%d,%d,%d,%d",
                    &index, &direction, &status, &mode, &multiparty) != 5) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (mode != 0) return ESP_OK; // Ignore non-voice calls.
    switch (status) {
        case 0: info->state = ML307_CALL_ACTIVE; break;
        case 1: info->state = ML307_CALL_HELD; break;
        case 2: info->state = ML307_CALL_DIALING; break;
        case 3: info->state = ML307_CALL_ALERTING; break;
        case 4: info->state = ML307_CALL_INCOMING; break;
        case 5: info->state = ML307_CALL_WAITING; break;
        default: return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t quote = response.find('"', marker);
    if (quote != std::string::npos) {
        const size_t end = response.find('"', quote + 1);
        if (end != std::string::npos) {
            const size_t count = std::min(end - quote - 1, sizeof(info->number) - 1);
            std::memcpy(info->number, response.data() + quote + 1, count);
            info->number[count] = '\0';
        }
    }
    return ESP_OK;
}

extern "C" esp_err_t ml307_voice_dial(const char *number)
{
    if (!number) return ESP_ERR_INVALID_ARG;
    const size_t length = std::strlen(number);
    if (length == 0 || length > 32) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < length; ++i) {
        const char c = number[i];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '*' || c == '#' ||
              (i == 0 && c == '+'))) return ESP_ERR_INVALID_ARG;
    }
    char command[48];
    std::snprintf(command, sizeof(command), "ATD%s;\r\n", number);
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_at_locked(), TAG, "voice AT handshake failed");
    return command_locked(command, nullptr, 10000);
}

extern "C" esp_err_t ml307_voice_answer(void)
{
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_at_locked(), TAG, "voice AT handshake failed");
    return command_locked("ATA\r\n", nullptr, 10000);
}

extern "C" esp_err_t ml307_voice_hangup(void)
{
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_at_locked(), TAG, "voice AT handshake failed");
    return command_locked("AT+CHUP\r\n", nullptr, 5000);
}

extern "C" esp_err_t ml307_voice_send_dtmf(char digit)
{
    if (!((digit >= '0' && digit <= '9') || digit == '*' || digit == '#')) {
        return ESP_ERR_INVALID_ARG;
    }
    char command[20];
    std::snprintf(command, sizeof(command), "AT+VTS=%c\r\n", digit);
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_at_locked(), TAG, "voice AT handshake failed");
    const esp_err_t ret = command_locked(command, nullptr, 3000);
    if (ret == ESP_OK) ESP_LOGI(TAG, "DTMF sent: %c", digit);
    return ret;
}

extern "C" esp_err_t ml307_sms_list(ml307_sms_message_t *messages,
                                      size_t capacity, size_t *count)
{
    if (!messages || !count || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_sms_locked(), TAG, "SMS init failed");
    std::string response;
    ESP_RETURN_ON_ERROR(command_locked("AT+CMGL=4\r\n", &response, 12000),
                        TAG, "SMS list failed");
    parse_list(response, messages, capacity, count, "+CMGL:");
    ESP_LOGI(TAG, "SMS list loaded: %u messages", static_cast<unsigned>(*count));
    return ESP_OK;
}

extern "C" esp_err_t ml307_sms_read(int index, ml307_sms_message_t *message)
{
    if (!message || index < 0) return ESP_ERR_INVALID_ARG;
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_sms_locked(), TAG, "SMS init failed");
    char command[40];
    std::snprintf(command, sizeof(command), "AT+CMGR=%d\r\n", index);
    std::string response;
    ESP_RETURN_ON_ERROR(command_locked(command, &response), TAG, "SMS read failed");
    size_t count = 0;
    parse_list(response, message, 1, &count, "+CMGR:");
    if (count == 0) return ESP_ERR_NOT_FOUND;
    message->index = index;
    return ESP_OK;
}

extern "C" esp_err_t ml307_sms_send(const char *address_utf8,
                                      const char *text_utf8)
{
    if (!address_utf8 || !address_utf8[0] || !text_utf8 || !text_utf8[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_sms_locked(), TAG, "SMS init failed");
    size_t units = 0;
    const std::string body = utf8_to_ucs2_hex(text_utf8, &units);
    if (units == 0 || units > 70) return ESP_ERR_INVALID_SIZE;

    if (s_sms_charset == SmsCharset::Ira) {
        std::string pdu;
        size_t tpdu_bytes = 0;
        if (!make_ucs2_submit_pdu(address_utf8, text_utf8, &pdu, &tpdu_bytes,
                                  &units)) {
            return ESP_ERR_INVALID_ARG;
        }
        std::string response;
        ESP_RETURN_ON_ERROR(command_locked("AT+CMGF=0\r\n", &response), TAG,
                            "PDU mode failed");
        char command[40];
        std::snprintf(command, sizeof(command), "AT+CMGS=%u\r\n",
                      static_cast<unsigned>(tpdu_bytes));
        esp_err_t result = command_locked(command, &response, 5000, true);
        if (result == ESP_OK) {
            pdu.push_back(static_cast<char>(0x1a));
            result = command_locked(reinterpret_cast<const uint8_t *>(pdu.data()),
                                    pdu.size(), &response, 30000, false);
        }
        const esp_err_t restore = command_locked("AT+CMGF=0\r\n", &response);
        if (result != ESP_OK) return result;
        ESP_RETURN_ON_ERROR(restore, TAG, "restore SMS PDU mode failed");
        ESP_LOGI(TAG, "SMS sent via PDU: destination=%s ucs2_units=%u",
                 address_utf8, static_cast<unsigned>(units));
        return ESP_OK;
    }

    const std::string address = s_sms_charset == SmsCharset::Ucs2
        ? utf8_to_ucs2_hex(address_utf8)
        : bytes_to_hex(address_utf8);
    std::string response;
    const std::string command = "AT+CMGS=\"" + address + "\"\r\n";
    ESP_RETURN_ON_ERROR(command_locked(command.c_str(), &response, 5000, true),
                        TAG, "CMGS prompt failed");
    std::string payload = body;
    payload.push_back(static_cast<char>(0x1a));
    ESP_RETURN_ON_ERROR(command_locked(reinterpret_cast<const uint8_t *>(payload.data()),
                                       payload.size(), &response, 30000, false),
                        TAG, "SMS submit failed");
    ESP_LOGI(TAG, "SMS sent: destination=%s ucs2_units=%u", address_utf8,
             static_cast<unsigned>(units));
    return ESP_OK;
}

extern "C" esp_err_t ml307_sms_get_time_utc(int64_t *utc_seconds)
{
    if (!utc_seconds) return ESP_ERR_INVALID_ARG;
    *utc_seconds = 0;
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_sms_locked(), TAG, "SMS init failed");
    std::string response;
    ESP_RETURN_ON_ERROR(command_locked("AT+CCLK?\r\n", &response), TAG,
                        "modem clock query failed");
    const size_t label = response.find("+CCLK:");
    if (label == std::string::npos) return ESP_ERR_INVALID_RESPONSE;
    const size_t quote = response.find('"', label);
    if (quote == std::string::npos) return ESP_ERR_INVALID_RESPONSE;
    const size_t end = response.find('"', quote + 1);
    if (end == std::string::npos) return ESP_ERR_INVALID_RESPONSE;
    const std::string raw = response.substr(quote + 1, end - quote - 1);
    if (!time_basis_parse_modem_clock(raw.c_str(), utc_seconds) ||
        *utc_seconds < 1577836800LL) {
        *utc_seconds = 0;
        ESP_LOGW(TAG, "modem clock is not valid: %s", raw.c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "modem clock: raw=%s basis=%s utc=%lld", raw.c_str(),
             time_basis_modem_clock_is_utc() ? "UTC digits" : "reported zone",
             static_cast<long long>(*utc_seconds));
    return ESP_OK;
}

extern "C" esp_err_t ml307_sms_get_time(char *timestamp, size_t capacity,
                                         int64_t *sort_key)
{
    if (!timestamp || capacity == 0 || !sort_key) return ESP_ERR_INVALID_ARG;
    timestamp[0] = '\0';
    *sort_key = 0;
    int64_t utc_seconds = 0;
    ESP_RETURN_ON_ERROR(ml307_sms_get_time_utc(&utc_seconds), TAG,
                        "modem clock query failed");
    return time_basis_format_utc(utc_seconds, timestamp, capacity, sort_key)
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

extern "C" esp_err_t ml307_sms_delete(int index)
{
    if (index < 0) return ESP_ERR_INVALID_ARG;
    CommandGuard guard;
    if (!guard.locked) return ESP_ERR_TIMEOUT;
    ESP_RETURN_ON_ERROR(ensure_sms_locked(), TAG, "SMS init failed");
    char command[40];
    std::snprintf(command, sizeof(command), "AT+CMGD=%d\r\n", index);
    std::string response;
    return command_locked(command, &response);
}
