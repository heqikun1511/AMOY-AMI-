#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "ws2815_rmt_encoder.h"
#include "ws2815_test.h"

/* Board wiring: GPIO13 -> WS2815B DI. */
#define WS2815_TEST_GPIO          GPIO_NUM_13
#define WS2815_TEST_LED_COUNT     8
#define WS2815_RMT_RESOLUTION_HZ  (10 * 1000 * 1000)

/* Eight-LED tachometer layout: 3 green, 3 yellow, 2 red, then shift flash. */
#define RPM_GREEN_LEDS             3
#define RPM_YELLOW_LEDS            3
#define RPM_RED_LEDS               2
#define RPM_SHIFT                  4500
#define RPM_LIMIT                  5000
#define RPM_BAR_REFRESH_MS         20
#define RPM_IDLE_FLASH_HALF_MS     450
#define RPM_SHIFT_FLASH_HALF_MS    70   /* about 7 Hz */
#define RPM_LIMIT_FLASH_HALF_MS    45   /* about 11 Hz */
#define WS2815_STARTUP_SELF_TEST    0
#define LED_SELF_TEST_COLOR_MS     350
#define LED_SELF_TEST_CHASE_MS     100

static const char *TAG = "WS2815_TEST";
/* The task is started at boot so the AMI home screen has a deterministic
 * standby indication.  It is only written from the UI task. */
static volatile bool s_home_standby = true;

static void ws2815_send_frame(rmt_channel_handle_t channel,
                              rmt_encoder_handle_t encoder,
                              const uint8_t *pixels)
{
    const rmt_transmit_config_t tx_config = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(channel, encoder, pixels,
                                 WS2815_TEST_LED_COUNT * 3, &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, portMAX_DELAY));
}

static void ws2815_set_rgb(uint8_t *pixels, size_t led,
                            uint8_t red, uint8_t green, uint8_t blue)
{
    /* Hardware verification showed this LED chain accepts bytes in RGB order. */
    pixels[led * 3 + 0] = red;
    pixels[led * 3 + 1] = green;
    pixels[led * 3 + 2] = blue;
}

static void ws2815_rpm_bar_frame(uint8_t *pixels, int rpm, TickType_t now)
{
    memset(pixels, 0, WS2815_TEST_LED_COUNT * 3);

    /* AMI 首页：所有转速灯常亮暗绿色，不闪烁、不随后台 RPM 演示变化。 */
    if (s_home_standby) {
        for (size_t i = 0; i < WS2815_TEST_LED_COUNT; ++i) {
            ws2815_set_rgb(pixels, i, 0, 36, 6);
        }
        return;
    }

    if (rpm < 0) {
        rpm = 0;
    }

    if (rpm == 0) {
        /* At zero RPM the entire strip gives a clearly visible but subdued
         * light-blue heartbeat.  It is intentionally brighter than the old
         * (0, 0, 8) value, which looked like a failed first LED. */
        bool on = ((now / pdMS_TO_TICKS(RPM_IDLE_FLASH_HALF_MS)) & 1U) == 0;
        if (on) {
            for (size_t i = 0; i < WS2815_TEST_LED_COUNT; ++i) {
                ws2815_set_rgb(pixels, i, 0, 28, 72);
            }
        }
        return;
    }

    if (rpm < RPM_SHIFT) {
        /* Once motion starts, LED 1 is bright green and every following LED
         * adds a fixed colour zone: 3 green, 3 yellow, 2 red. */
        const int span = RPM_SHIFT;
        int lit_count = (rpm * WS2815_TEST_LED_COUNT + span - 1) / span;
        if (lit_count < 1) {
            lit_count = 1;
        }
        if (lit_count > WS2815_TEST_LED_COUNT) {
            lit_count = WS2815_TEST_LED_COUNT;
        }

        for (int i = 0; i < lit_count; ++i) {
            if (i < RPM_GREEN_LEDS) {
                ws2815_set_rgb(pixels, i, 0, 255, 0);
            } else if (i < RPM_GREEN_LEDS + RPM_YELLOW_LEDS) {
                /* Hardware-verified yellow on this RGB-order chain: FF FF 00. */
                ws2815_set_rgb(pixels, i, 255, 255, 0);
            } else {
                ws2815_set_rgb(pixels, i, 255, 0, 0);
            }
        }
        return;
    }

    if (rpm >= RPM_SHIFT) {
        const TickType_t half_period = pdMS_TO_TICKS(
            rpm >= RPM_LIMIT ? RPM_LIMIT_FLASH_HALF_MS : RPM_SHIFT_FLASH_HALF_MS);
        bool on = ((now / half_period) & 1U) == 0;
        if (on) {
            uint8_t red = rpm >= RPM_LIMIT ? 255 : 128;
            for (size_t i = 0; i < WS2815_TEST_LED_COUNT; ++i) {
                ws2815_set_rgb(pixels, i, red, 0, 0);
            }
        }
        return;
    }

}

static void ws2815_rpm_bar_task(void *arg)
{
    volatile int *rpm_source = arg;
    rmt_channel_handle_t channel = NULL;
    rmt_encoder_handle_t encoder = NULL;
    const rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2815_TEST_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2815_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &channel));

    const ws2815_rmt_encoder_config_t encoder_config = {
        .resolution_hz = WS2815_RMT_RESOLUTION_HZ,
        .reset_us = 300,
    };
    ESP_ERROR_CHECK(ws2815_new_rmt_encoder(&encoder_config, &encoder));
    ESP_ERROR_CHECK(rmt_enable(channel));

    ESP_LOGI(TAG, "RPM bar active: %d LEDs, %d rpm limit", WS2815_TEST_LED_COUNT,
             RPM_LIMIT);
    uint8_t pixels[WS2815_TEST_LED_COUNT * 3];

#if WS2815_STARTUP_SELF_TEST
    /* 上电全色和逐颗自检；正常工程保持关闭。 */
    const uint8_t test_colors[][3] = {
        {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}
    };
    for (size_t color = 0; color < sizeof(test_colors) / sizeof(test_colors[0]); ++color) {
        for (size_t led = 0; led < WS2815_TEST_LED_COUNT; ++led) {
            ws2815_set_rgb(pixels, led,
                           test_colors[color][0],
                           test_colors[color][1],
                           test_colors[color][2]);
        }
        ws2815_send_frame(channel, encoder, pixels);
        vTaskDelay(pdMS_TO_TICKS(LED_SELF_TEST_COLOR_MS));
    }
    for (size_t led = 0; led < WS2815_TEST_LED_COUNT; ++led) {
        memset(pixels, 0, sizeof(pixels));
        ws2815_set_rgb(pixels, led, 255, 255, 255);
        ws2815_send_frame(channel, encoder, pixels);
        vTaskDelay(pdMS_TO_TICKS(LED_SELF_TEST_CHASE_MS));
    }
#endif

    for (;;) {
        ws2815_rpm_bar_frame(pixels, *rpm_source, xTaskGetTickCount());
        ws2815_send_frame(channel, encoder, pixels);
        vTaskDelay(pdMS_TO_TICKS(RPM_BAR_REFRESH_MS));
    }
}

void ws2815_rpm_bar_start(volatile int *rpm_source)
{
    configASSERT(rpm_source != NULL);
    BaseType_t created = xTaskCreate(ws2815_rpm_bar_task, "ws2815_rpm", 4096,
                                     (void *)rpm_source, 4, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void ws2815_rpm_bar_set_home_standby(bool enabled)
{
    s_home_standby = enabled;
}

void gpio13_high_level_test(void)
{
    const gpio_config_t gpio_output_config = {
        .pin_bit_mask = 1ULL << WS2815_TEST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_output_config));
    ESP_ERROR_CHECK(gpio_set_level(WS2815_TEST_GPIO, 1));
    ESP_LOGW(TAG, "GPIO%d is held at 3.3 V", WS2815_TEST_GPIO);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ws2815_full_yellow_test(void)
{
    rmt_channel_handle_t channel = NULL;
    rmt_encoder_handle_t encoder = NULL;

    const rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2815_TEST_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2815_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &channel));

    const ws2815_rmt_encoder_config_t encoder_config = {
        .resolution_hz = WS2815_RMT_RESOLUTION_HZ,
        .reset_us = 300,
    };
    ESP_ERROR_CHECK(ws2815_new_rmt_encoder(&encoder_config, &encoder));
    ESP_ERROR_CHECK(rmt_enable(channel));

    /* This board's LED chain uses RGB byte order; FF FF 00 is full yellow. */
    uint8_t pixels[WS2815_TEST_LED_COUNT * 3];
    for (size_t i = 0; i < WS2815_TEST_LED_COUNT; ++i) {
        pixels[i * 3 + 0] = 255;
        pixels[i * 3 + 1] = 255;
        pixels[i * 3 + 2] = 0;
    }

    const rmt_transmit_config_t tx_config = { .loop_count = 0 };
    ESP_LOGW(TAG, "GPIO%d: %u LEDs steady full yellow", WS2815_TEST_GPIO,
             WS2815_TEST_LED_COUNT);
    for (;;) {
        /* Periodically resend the steady full-yellow frame. */
        ESP_ERROR_CHECK(rmt_transmit(channel, encoder, pixels, sizeof(pixels), &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
