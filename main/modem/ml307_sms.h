#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ML307_SMS_MAX_MESSAGES 32
#define ML307_SMS_ADDRESS_SIZE 48
#define ML307_SMS_TIME_SIZE 32
#define ML307_SMS_TEXT_SIZE 512

typedef struct {
    int index;
    bool unread;
    bool outgoing;
    int64_t sort_key;
    char address[ML307_SMS_ADDRESS_SIZE];
    char timestamp[ML307_SMS_TIME_SIZE];
    char text[ML307_SMS_TEXT_SIZE];
} ml307_sms_message_t;

/* Register a native USB client for ML307A interface 2 Bulk AT transport. */
esp_err_t ml307_sms_transport_init(void);
esp_err_t ml307_sms_transport_enable(uint8_t device_address);
bool ml307_sms_is_ready(void);
bool ml307_sms_take_new_message_hint(void);

/* These calls serialize access to the modem and may block for a few seconds. */
esp_err_t ml307_sms_list(ml307_sms_message_t *messages, size_t capacity,
                         size_t *count);
esp_err_t ml307_sms_read(int index, ml307_sms_message_t *message);
esp_err_t ml307_sms_send(const char *address_utf8, const char *text_utf8);
/* Query the modem clock as UTC seconds, independent of display time zone. */
esp_err_t ml307_sms_get_time_utc(int64_t *utc_seconds);
/* Query and format the modem clock using the global time basis. */
esp_err_t ml307_sms_get_time(char *timestamp, size_t capacity, int64_t *sort_key);
esp_err_t ml307_sms_delete(int index);

#ifdef __cplusplus
}
#endif
