#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    ROTARY_EVENT_NONE = 0,
    ROTARY_EVENT_LEFT,
    ROTARY_EVENT_RIGHT,
    ROTARY_EVENT_PRESS,
} rotary_event_t;

/*
 * The board integration owns these pin assignments.  This driver intentionally
 * provides no defaults so it cannot claim a GPIO before the schematic is fixed.
 */
typedef struct {
    gpio_num_t a_gpio;
    gpio_num_t b_gpio;
    /*
     * Optional encoder common terminal.  Set to GPIO_NUM_NC when the
     * encoder's C terminal is wired directly to GND.  When supplied, this
     * GPIO is driven low by the driver.
     */
    gpio_num_t common_gpio;

    /*
     * Optional, separate active-low push switch.  This must not be the C
     * terminal of a three-pin A/C/B mechanical encoder.
     */
    gpio_num_t switch_gpio;
    bool enable_internal_pullup;
} rotary_input_config_t;

/**
 * @brief Configure and start the rotary-input polling task.
 *
 * A and B must be distinct valid input GPIOs.  common_gpio and switch_gpio
 * are optional and use GPIO_NUM_NC when absent.  No GPIO is configured until
 * this function is called.
 */
esp_err_t rotary_input_init(const rotary_input_config_t *config);

/**
 * @brief Receive the next debounced rotary event.
 */
bool rotary_input_get_event(rotary_event_t *event, TickType_t wait_ticks);
