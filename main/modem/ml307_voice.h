#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ML307_CALL_IDLE,
    ML307_CALL_DIALING,
    ML307_CALL_ALERTING,
    ML307_CALL_INCOMING,
    ML307_CALL_ACTIVE,
    ML307_CALL_HELD,
    ML307_CALL_WAITING,
} ml307_call_state_t;

typedef struct {
    ml307_call_state_t state;
    char number[48];
} ml307_call_info_t;

/* Uses the shared native USB AT port; calls may block and must run off the UI task. */
esp_err_t ml307_voice_query(ml307_call_info_t *info);
esp_err_t ml307_voice_dial(const char *number);
esp_err_t ml307_voice_answer(void);
esp_err_t ml307_voice_hangup(void);
/* Send one in-call DTMF digit (0-9, * or #) for automated voice menus. */
esp_err_t ml307_voice_send_dtmf(char digit);
bool ml307_voice_take_ring_hint(void);

#ifdef __cplusplus
}
#endif
