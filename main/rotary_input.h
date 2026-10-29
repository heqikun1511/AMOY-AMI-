#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>

typedef enum {
    ROTARY_EVENT_NONE = 0,
    ROTARY_EVENT_LEFT,
    ROTARY_EVENT_RIGHT,
    ROTARY_EVENT_PRESS,
} rotary_event_t;

/* Live GPIO/event data used by the on-screen EC11 wiring diagnostic. */
typedef struct {
    uint8_t a_level;
    uint8_t b_level;
    uint8_t c_level;
    uint32_t rotation_events;
    uint32_t press_events;
} rotary_input_debug_t;

esp_err_t rotary_input_init(void);
bool rotary_input_get_event(rotary_event_t *event, TickType_t wait_ticks);
void rotary_input_get_debug(rotary_input_debug_t *debug);
