#include "rotary_input.h"

#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "ROTARY"

#define ROTARY_QUEUE_LENGTH       16
/*
 * The EC11 circuit already has a 10 kOhm / 100 nF hardware filter.  A 10 ms
 * software polling period can therefore skip complete quadrature phases when
 * the knob is turned normally, leaving the decoder unable to collect its four
 * transitions.  Sample faster than the RC time constant instead.
 */
#define ROTARY_TASK_PERIOD_MS     1
#define ROTARY_PRESS_FILTER_COUNT 10
/* Keep this enabled while commissioning the EC11.  A normal detent generates
 * four AB transitions; the log exposes a missing channel or wrong wiring
 * before the decoder has enough transitions to emit an event. */
#define ROTARY_LOG_TRANSITIONS     1

static QueueHandle_t s_event_queue;
static rotary_input_config_t s_config;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started_or_starting;

static bool rotary_gpio_is_optional(gpio_num_t gpio)
{
    return gpio == GPIO_NUM_NC || GPIO_IS_VALID_GPIO(gpio);
}

static bool rotary_config_is_valid(const rotary_input_config_t *config)
{
    if (config == NULL ||
        !GPIO_IS_VALID_GPIO(config->a_gpio) ||
        !GPIO_IS_VALID_GPIO(config->b_gpio) ||
        config->a_gpio == config->b_gpio ||
        !rotary_gpio_is_optional(config->common_gpio) ||
        !rotary_gpio_is_optional(config->switch_gpio)) {
        return false;
    }

    if (config->common_gpio != GPIO_NUM_NC &&
        (!GPIO_IS_VALID_OUTPUT_GPIO(config->common_gpio) ||
         config->common_gpio == config->a_gpio ||
         config->common_gpio == config->b_gpio)) {
        return false;
    }

    if (config->switch_gpio != GPIO_NUM_NC &&
        (config->switch_gpio == config->a_gpio ||
         config->switch_gpio == config->b_gpio ||
         config->switch_gpio == config->common_gpio)) {
        return false;
    }

    return true;
}

static void rotary_task(void *arg)
{
    (void)arg;

    uint8_t previous_state =
        ((gpio_get_level(s_config.a_gpio) ? 1 : 0) << 1) |
        (gpio_get_level(s_config.b_gpio) ? 1 : 0);
    int8_t step_accumulator = 0;
    uint8_t last_switch = 1;
    uint8_t stable_switch = 1;
    uint8_t switch_count = 0;
    TickType_t poll_delay = pdMS_TO_TICKS(ROTARY_TASK_PERIOD_MS);
    if (poll_delay == 0) {
        poll_delay = 1;
    }

    /* Four-phase quadrature transition table. */
    static const int8_t transition_table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0,
    };

    while (true) {
        uint8_t a = gpio_get_level(s_config.a_gpio) ? 1 : 0;
        uint8_t b = gpio_get_level(s_config.b_gpio) ? 1 : 0;
        uint8_t current_state = (a << 1) | b;
        uint8_t table_index = (previous_state << 2) | current_state;
        int8_t transition = transition_table[table_index];

        step_accumulator += transition;
#if ROTARY_LOG_TRANSITIONS
        if (current_state != previous_state) {
            ESP_LOGI(TAG, "AB: %d%d -> %d%d, delta=%d, sum=%d",
                     (previous_state >> 1) & 1, previous_state & 1,
                     a, b, transition, step_accumulator);
        }
#endif
        previous_state = current_state;

        /* A detent normally produces four valid state transitions. */
        if (step_accumulator >= 4) {
            rotary_event_t event = ROTARY_EVENT_RIGHT;
            (void)xQueueSend(s_event_queue, &event, 0);
            ESP_LOGI(TAG, "detent: right");
            step_accumulator = 0;
        } else if (step_accumulator <= -4) {
            rotary_event_t event = ROTARY_EVENT_LEFT;
            (void)xQueueSend(s_event_queue, &event, 0);
            ESP_LOGI(TAG, "detent: left");
            step_accumulator = 0;
        }

        /* The optional, separate push switch is active-low. */
        if (s_config.switch_gpio != GPIO_NUM_NC) {
            uint8_t raw_switch =
                gpio_get_level(s_config.switch_gpio) ? 1 : 0;
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
                }
            }
        }

        /* Never allow a sub-tick polling interval to become a busy loop. */
        vTaskDelay(poll_delay);
    }
}

esp_err_t rotary_input_init(const rotary_input_config_t *config)
{
    if (!rotary_config_is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_init_lock);
    if (s_started_or_starting) {
        portEXIT_CRITICAL(&s_init_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_started_or_starting = true;
    portEXIT_CRITICAL(&s_init_lock);

    s_config = *config;
    uint64_t input_pin_mask = (1ULL << s_config.a_gpio) |
                              (1ULL << s_config.b_gpio);
    if (s_config.switch_gpio != GPIO_NUM_NC) {
        input_pin_mask |= 1ULL << s_config.switch_gpio;
    }

    const gpio_config_t input_gpio_config = {
        .pin_bit_mask = input_pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = s_config.enable_internal_pullup ? GPIO_PULLUP_ENABLE
                                                       : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&input_gpio_config);
    if (err != ESP_OK) {
        goto init_failed;
    }

    if (s_config.common_gpio != GPIO_NUM_NC) {
        const gpio_config_t common_gpio_config = {
            .pin_bit_mask = 1ULL << s_config.common_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&common_gpio_config);
        if (err != ESP_OK) {
            goto init_failed;
        }

        err = gpio_set_level(s_config.common_gpio, 0);
        if (err != ESP_OK) {
            goto init_failed;
        }
    }

    s_event_queue = xQueueCreate(ROTARY_QUEUE_LENGTH, sizeof(rotary_event_t));
    if (s_event_queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto init_failed;
    }

    if (xTaskCreate(rotary_task, "rotary_input", 3072, NULL, 10, NULL) != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        err = ESP_ERR_NO_MEM;
        goto init_failed;
    }

    if (s_config.common_gpio == GPIO_NUM_NC) {
        ESP_LOGI(TAG,
                 "Rotary input initialized: A=GPIO%d B=GPIO%d C=GND, initial AB=%d%d",
                 s_config.a_gpio, s_config.b_gpio,
                 gpio_get_level(s_config.a_gpio), gpio_get_level(s_config.b_gpio));
    } else {
        ESP_LOGI(TAG,
                 "Rotary input initialized: A=GPIO%d B=GPIO%d C=GPIO%d (low)",
                 s_config.a_gpio, s_config.b_gpio, s_config.common_gpio);
    }
    return ESP_OK;

init_failed:
    portENTER_CRITICAL(&s_init_lock);
    s_started_or_starting = false;
    portEXIT_CRITICAL(&s_init_lock);
    return err;
}

bool rotary_input_get_event(rotary_event_t *event, TickType_t wait_ticks)
{
    if (event == NULL || s_event_queue == NULL) {
        return false;
    }

    return xQueueReceive(s_event_queue, event, wait_ticks) == pdTRUE;
}
