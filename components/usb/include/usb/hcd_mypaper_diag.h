#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sequence;
    uint32_t irq_enter_cycles;
    uint32_t mark_done_cycles;
    uint32_t exec_cycles;
    uint32_t parse_cycles;
    uint32_t fill_cycles;
    uint16_t irq_microframe;
    uint16_t irq_remaining_ticks;
    uint16_t current_microframe;
    uint16_t expected_next_microframe;
    uint16_t handoff_remaining_ticks;
    int16_t irq_late_microframes;
    uint8_t had_next_buffer;
    uint8_t pending_before;
    uint8_t executable_before;
    uint8_t pending_after;
} usb_hcd_mypaper_diag_t;

// Returns the most recent ISR timing record for this usb_transfer_t pointer.
// The argument is void* so this diagnostic header does not expose HCD-private
// types to the class driver.
bool usb_hcd_mypaper_diag_get(const void *transfer,
                              usb_hcd_mypaper_diag_t *out);

#ifdef __cplusplus
}
#endif
