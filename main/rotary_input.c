#include "rotary_input.h"

#include "driver/gpio.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

#define TAG "ROTARY"

/* EC11 wiring used on this AMI:
 *   A -> GPIO4, B -> GPIO3, C/push switch -> GPIO2.
 * Each signal is active-low and uses the ESP32 internal pull-up. */
#define ROTARY_A_GPIO  GPIO_NUM_4
#define ROTARY_B_GPIO  GPIO_NUM_3
#define ROTARY_SW_GPIO GPIO_NUM_2

#define ROTARY_QUEUE_LENGTH       16
#define ROTARY_TASK_PERIOD_MS     2
#define ROTARY_PRESS_FILTER_COUNT 10

static QueueHandle_t s_event_queue;
static volatile uint8_t s_a_level = 1;
static volatile uint8_t s_b_level = 1;
static volatile uint8_t s_c_level = 1;
static volatile uint32_t s_rotation_events;
static volatile uint32_t s_press_events;

static void rotary_task(void *arg)
{
    (void)arg;

    uint8_t previous_state =
        ((gpio_get_level(ROTARY_A_GPIO) ? 1 : 0) << 1) |
        (gpio_get_level(ROTARY_B_GPIO) ? 1 : 0);
    int8_t step_accumulator = 0;
    uint8_t last_switch = 1;
    uint8_t stable_switch = 1;
    uint8_t switch_count = 0;

    /* A/B follows the requested A=GPIO4 / B=GPIO3 wiring.  A positive
     * quadrature cycle is the clockwise/menu-next direction. */
    static const int8_t transition_table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    while (true) {
        uint8_t a = gpio_get_level(ROTARY_A_GPIO) ? 1 : 0;
        uint8_t b = gpio_get_level(ROTARY_B_GPIO) ? 1 : 0;
        s_a_level = a;
        s_b_level = b;
        uint8_t current_state = (a << 1) | b;
        uint8_t table_index = (previous_state << 2) | current_state;

        step_accumulator += transition_table[table_index];
        previous_state = current_state;

        if (step_accumulator >= 4) {
            rotary_event_t event = ROTARY_EVENT_RIGHT;
            (void)xQueueSend(s_event_queue, &event, 0);
            ++s_rotation_events;
            ESP_LOGI(TAG, "RIGHT event (A=%d B=%d)", a, b);
            step_accumulator = 0;
        } else if (step_accumulator <= -4) {
            rotary_event_t event = ROTARY_EVENT_LEFT;
            (void)xQueueSend(s_event_queue, &event, 0);
            ++s_rotation_events;
            ESP_LOGI(TAG, "LEFT event (A=%d B=%d)", a, b);
            step_accumulator = 0;
        }

        /* C脚低电平有效，按下时只上报一次。 */
        uint8_t raw_switch = gpio_get_level(ROTARY_SW_GPIO) ? 1 : 0;
        s_c_level = raw_switch;
        if (raw_switch == last_switch) {
            if (switch_count < ROTARY_PRESS_FILTER_COUNT) {
                ++switch_count;
            }
        } else {
            switch_count = 0;
            last_switch = raw_switch;
        }

        if (switch_count >= ROTARY_PRESS_FILTER_COUNT &&
            stable_switch != raw_switch) {
            stable_switch = raw_switch;
            if (stable_switch == 0) {
                rotary_event_t event = ROTARY_EVENT_PRESS;
                (void)xQueueSend(s_event_queue, &event, 0);
                ++s_press_events;
                ESP_LOGI(TAG, "PRESS event");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ROTARY_TASK_PERIOD_MS));
    }
}

esp_err_t rotary_input_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << ROTARY_A_GPIO) |
                        (1ULL << ROTARY_B_GPIO) |
                        (1ULL << ROTARY_SW_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "rotary GPIO config failed");

    s_event_queue = xQueueCreate(ROTARY_QUEUE_LENGTH, sizeof(rotary_event_t));
    if (s_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(rotary_task, "rotary_input", 3072, NULL, 10, NULL) != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialized: A=GPIO4 B=GPIO3 SW=GPIO2");
    return ESP_OK;
}

bool rotary_input_get_event(rotary_event_t *event, TickType_t wait_ticks)
{
    if (event == NULL || s_event_queue == NULL) {
        return false;
    }
    return xQueueReceive(s_event_queue, event, wait_ticks) == pdTRUE;
}

void rotary_input_get_debug(rotary_input_debug_t *debug)
{
    if (!debug) {
        return;
    }

    /* All members are native 32-bit-or-smaller values.  A diagnostic snapshot
     * may be one scan old, which is acceptable and avoids blocking the input
     * sampler. */
    debug->a_level = s_a_level;
    debug->b_level = s_b_level;
    debug->c_level = s_c_level;
    debug->rotation_events = s_rotation_events;
    debug->press_events = s_press_events;
}
