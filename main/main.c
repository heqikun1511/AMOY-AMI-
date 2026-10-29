

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "lvgl_demo.h"
#include "bsp_can.h"
#include "ami_display_can.h"
#include "myiic.h"
#include "xl9555.h"
#include "ui.h"
#include "ui/vars.h"
#include "ui/fonts.h"
#include "lcd.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "rotary_input.h"

static const char *TAG = "MAIN";

/* The board Type-C connector is wired to the P4 native USB DP/DM pair.
 * Use the TinyUSB CDC device on that connector for the application console. */
static void usb_cdc_console_init(void)
{
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_cfg));
    ESP_ERROR_CHECK(tinyusb_console_init(cdc_cfg.cdc_port));
}

/* Prepare the panel backlight pin at boot, but keep the panel dark until the
 * first complete UI frame has reached the DSI framebuffer. */
static void lcd_backlight_boot_prepare(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << LCD_BL_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));
    LCD_BACKLIGHT_OFF();
}

/*
 * 仅用于硬件定位：设为 1 时，app_main() 在 USB CDC 就绪后直接初始化 MIPI
 * 屏，并循环显示纯色；LVGL、触摸、I2C 扩展器、CAN 和 UI 任务均不会启动。
 * 确认屏幕正常后改回 0，即可恢复原应用。
 */
#define LCD_MINIMAL_TEST 0
/*
 * 正点原子开发板对照测试的第一阶段：只验证 GPIO53 背光。
 * 保持为 0 时绝不初始化 DSI PHY、绝不读屏 ID、绝不发送任何 MIPI 命令。
 * 确认背光后才改为 1 进入下一阶段。
 */
#define LCD_MINIMAL_TEST_ENABLE_MIPI 1

/* Stand-alone diagnostic: only the complete display driver and CAN-B run.
 * The old dashboard, touch, rotary, LEDs, buttons and I2C expander are not
 * initialised. */
#define SCREEN_CAN_TEST_ONLY 0
/* Normal AMI operation is receive-only.  TWAI still acknowledges valid CAN
 * frames at the CAN physical/protocol layer. */
#define AMI_RECEIVE_ONLY 1

/* Keep the optional rotary input disabled while diagnosing the CAN bus. */
#define ROTARY_INPUT_ENABLE 0

/* This custom AMI PCB does not populate the development-board XL9555.  Keep
 * its source available for the reference build, but do not issue I2C traffic
 * to a device which is not fitted. */
#define BOARD_HAS_XL9555 0

/* CAN parsing and key debounce retain their 20 ms period.  Rendering changing
 * dashboard values at 20 Hz is visually smooth at the panel's frame rate and
 * avoids invalidating text faster than it can be displayed. */
#define UI_DASHBOARD_UPDATE_MS     50
#define AMI_DBC_FRAME_TIMEOUT_MS    250
static bool s_driving_demo_started = false;

/* 独立BOOT按键，直接连接ESP32-P4 GPIO35，低电平有效。 */
#define BOOT_GPIO_PIN GPIO_NUM_35

/* AMI task selector: external switch from GPIO5 to GND, active low. */
#define AMI_BUTTON_GPIO              GPIO_NUM_5
#define AMI_BUTTON_ACTIVE_LEVEL      0
#define AMI_BUTTON_DEBOUNCE_MS       40
#define AMI_BUTTON_LONG_PRESS_MS     800

/* EXIO8/9/10通过XL9555 IO扩展器连接，均按低电平有效处理。 */

/* CAN transceiver connections requested for this board revision. */
#define CAN_TX_PIN GPIO_NUM_15
#define CAN_RX_PIN GPIO_NUM_14

#if SCREEN_CAN_TEST_ONLY
#define CAN_SCREEN_TEST_TX_ID 0x18FF50E5U

typedef struct
{
    uint32_t rx_count;
    uint32_t last_id;
    uint8_t last_len;
    uint8_t last_data[8];
    TickType_t last_rx_tick;
    uint32_t tx_ok_count;
    uint32_t tx_fail_count;
} can_screen_test_data_t;

static can_screen_test_data_t s_can_screen_test_data;
static portMUX_TYPE s_can_screen_test_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t *s_can_screen_test_status;
static lv_obj_t *s_can_screen_test_bus;
static lv_obj_t *s_can_screen_test_rx;
static lv_obj_t *s_can_screen_test_last;
static lv_obj_t *s_can_screen_test_tx;

static void can_screen_test_callback(uint32_t can_id, bool is_extended,
                                     const uint8_t *data, uint8_t len)
{
    (void)ami_display_can_receive(can_id, is_extended, data, len);
    taskENTER_CRITICAL(&s_can_screen_test_lock);
    s_can_screen_test_data.rx_count++;
    s_can_screen_test_data.last_id = can_id;
    s_can_screen_test_data.last_len = len > 8 ? 8 : len;
    if (data && len) {
        memcpy(s_can_screen_test_data.last_data, data,
               s_can_screen_test_data.last_len);
    }
    s_can_screen_test_data.last_rx_tick = xTaskGetTickCount();
    taskEXIT_CRITICAL(&s_can_screen_test_lock);
}

static const char *can_screen_test_state_name(uint32_t state)
{
    switch (state) {
    case 0: return "STOPPED";
    case 1: return "RUNNING";
    case 2: return "BUS OFF";
    case 3: return "RECOVERING";
    default: return "UNKNOWN";
    }
}

static lv_obj_t *can_screen_test_label(lv_obj_t *parent, int y, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, 90, y);
    lv_obj_set_width(label, 1740);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
    return label;
}

static void can_screen_test_ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        can_screen_test_data_t data;
        bsp_can_status_t status = {0};
        taskENTER_CRITICAL(&s_can_screen_test_lock);
        data = s_can_screen_test_data;
        taskEXIT_CRITICAL(&s_can_screen_test_lock);
        int status_ok = bsp_can_get_status(&status);
        TickType_t now = xTaskGetTickCount();
        bool rx_recent = data.rx_count &&
                         (now - data.last_rx_tick <= pdMS_TO_TICKS(2000));
        char text[192];

        if (lvgl_port_lock(portMAX_DELAY)) {
            lv_label_set_text(s_can_screen_test_status,
                              rx_recent ? "CAN-B RECEIVING" : "CAN-B WAITING FOR PC FRAME");
            lv_obj_set_style_text_color(s_can_screen_test_status,
                                        lv_color_hex(rx_recent ? 0x39FF65 : 0xFFD400),
                                        LV_PART_MAIN);

            if (status_ok == 0) {
                snprintf(text, sizeof(text),
                         "Controller: %s    TEC: %lu    REC: %lu    Bus errors: %lu    TX failures: %lu",
                         can_screen_test_state_name(status.state),
                         (unsigned long)status.tx_error_counter,
                         (unsigned long)status.rx_error_counter,
                         (unsigned long)status.bus_error_count,
                         (unsigned long)status.tx_failed_count);
            } else {
                snprintf(text, sizeof(text), "Controller: NOT INITIALISED");
            }
            lv_label_set_text(s_can_screen_test_bus, text);

            snprintf(text, sizeof(text), "RX frames: %lu", (unsigned long)data.rx_count);
            lv_label_set_text(s_can_screen_test_rx, text);

            snprintf(text, sizeof(text),
                     "Last RX: ID 0x%08lX   DLC %u   DATA %02X %02X %02X %02X %02X %02X %02X %02X",
                     (unsigned long)data.last_id, data.last_len,
                     data.last_data[0], data.last_data[1], data.last_data[2], data.last_data[3],
                     data.last_data[4], data.last_data[5], data.last_data[6], data.last_data[7]);
            lv_label_set_text(s_can_screen_test_last, text);

#if AMI_RECEIVE_ONLY
            snprintf(text, sizeof(text), "AMI application TX: DISABLED (receive-only mode)");
#else
            snprintf(text, sizeof(text),
                     "ESP test TX: ID 0x%08X every 1 s    success: %lu    failed: %lu",
                     CAN_SCREEN_TEST_TX_ID, (unsigned long)data.tx_ok_count,
                     (unsigned long)data.tx_fail_count);
#endif

            lv_label_set_text(s_can_screen_test_tx, text);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void can_screen_test_tx_task(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    for (;;) {
        uint8_t frame[8] = { 0xCA, 0x4E, 0xB0, 0x01,
                             (uint8_t)(sequence >> 24), (uint8_t)(sequence >> 16),
                             (uint8_t)(sequence >> 8), (uint8_t)sequence };
        int ret = bsp_can_send(CAN_SCREEN_TEST_TX_ID, frame, sizeof(frame));
        taskENTER_CRITICAL(&s_can_screen_test_lock);
        if (ret == 0) {
            s_can_screen_test_data.tx_ok_count++;
        } else {
            s_can_screen_test_data.tx_fail_count++;
        }
        taskEXIT_CRITICAL(&s_can_screen_test_lock);
        sequence++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void can_screen_test_start(void)
{
    /* Complete MIPI/LVGL display path; this intentionally does not initialise
     * the touch controller or the application-generated screens. */
    lvgl_display_only_init();

    if (lvgl_port_lock(portMAX_DELAY)) {
        lv_obj_t *screen = lv_scr_act();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x071018), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *title = can_screen_test_label(screen, 80, &lv_font_montserrat_48);
        lv_label_set_text(title, "CAN-B / DISPLAY DIAGNOSTIC");
        s_can_screen_test_status = can_screen_test_label(screen, 200, &lv_font_montserrat_48);
        s_can_screen_test_bus = can_screen_test_label(screen, 330, &ui_font_orbitron_bold_25);
        s_can_screen_test_rx = can_screen_test_label(screen, 440, &lv_font_montserrat_48);
        s_can_screen_test_last = can_screen_test_label(screen, 560, &ui_font_orbitron_bold_25);
        s_can_screen_test_tx = can_screen_test_label(screen, 690, &ui_font_orbitron_bold_25);

        lv_obj_t *pins = can_screen_test_label(screen, 820, &ui_font_orbitron_bold_25);
        lv_label_set_text(pins, "CAN-B pins: GPIO15 = TX, GPIO14 = RX   |   Bit rate: 250 kbit/s   |   Standard + extended frames");
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }
    if (!lcd_wait_for_next_refresh(500)) {
        ESP_LOGW(TAG, "CAN diagnostic first-frame VSYNC timeout");
    }
    LCD_BACKLIGHT_ON();

    ami_display_can_init();
    int can_ret = bsp_can_init(CAN_TX_PIN, CAN_RX_PIN, can_screen_test_callback);
    ESP_LOGI(TAG, "CAN-B diagnostic init: TX=%d RX=%d result=%d", CAN_TX_PIN, CAN_RX_PIN, can_ret);
    if (can_ret == 0 && !AMI_RECEIVE_ONLY) {
        xTaskCreatePinnedToCore(can_screen_test_tx_task, "can_test_tx", 3072,
                                NULL, 4, NULL, tskNO_AFFINITY);
    }
    xTaskCreatePinnedToCore(can_screen_test_ui_task, "can_test_ui", 4096,
                            NULL, 3, NULL, tskNO_AFFINITY);
}
#endif

#if LCD_MINIMAL_TEST
static void lcd_minimal_test(void)
{
    const gpio_config_t lcd_ctrl_cfg = {
#if LCD_MINIMAL_TEST_ENABLE_MIPI
        .pin_bit_mask = (1ULL << LCD_BL_PIN) | (1ULL << LCD_RST_PIN),
#else
        /* 背光单项测试：GPIO52/MIPI_RST 与全部 DSI 专用引脚保持完全未配置。 */
        .pin_bit_mask = (1ULL << LCD_BL_PIN),
#endif
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&lcd_ctrl_cfg));
    LCD_BACKLIGHT_OFF();
#if LCD_MINIMAL_TEST_ENABLE_MIPI
    LCD_RESET_ASSERT();
    vTaskDelay(pdMS_TO_TICKS(20));
    LCD_RESET_RELEASE();
#else
    vTaskDelay(pdMS_TO_TICKS(20));
#endif
    LCD_BACKLIGHT_ON();
#if LCD_MINIMAL_TEST_ENABLE_MIPI
    ESP_LOGI(TAG, "LCD signals: GPIO53(BL)=1, GPIO52(RST)=1; hold for 2 seconds");
#else
    ESP_LOGI(TAG, "Backlight-only test: GPIO53(LCD_BL)=1; GPIO52 and MIPI DSI are untouched");
#endif
    vTaskDelay(pdMS_TO_TICKS(2000));

#if LCD_MINIMAL_TEST_ENABLE_MIPI
    /* 放在 DSI 前，避免 USB 枚举初期的日志被 monitor 漏掉。 */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGW(TAG, "ESP32-P4 silicon revision: %u (DSI build target: v3.x)", chip_info.revision);
    ESP_LOGW(TAG, "Reset reason immediately before DSI: %d", esp_reset_reason());
    ESP_LOGI(TAG, "LCD minimal test: initialising MIPI LCD only");
    lcd_init();
    if (lcddev.lcd_panel_handle == NULL) {
        ESP_LOGE(TAG, "MIPI initialisation failed; GPIO53 remains HIGH for hardware checks");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    /* The normal LCD driver deliberately leaves BL off until its UI first
     * frame.  This standalone hardware-pattern test owns its own first frame. */
    LCD_BACKLIGHT_ON();
    ESP_LOGI(TAG, "LCD ready: %lux%lu; backlight is ON", lcddev.width, lcddev.height);

    /* Generate vertical colour bars inside the P4 DSI host.  This bypasses
     * the LCD frame buffers and draw_bitmap(), leaving only panel reset,
     * controller initialisation and the DSI physical/video link under test. */
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(lcddev.lcd_panel_handle,
                                                  MIPI_DSI_PATTERN_BAR_VERTICAL));
    ESP_LOGI(TAG, "MIPI DSI hardware colour-bar test is active");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    ESP_LOGI(TAG, "GPIO-only test active: MIPI DSI is deliberately disabled");
    for (;;) {
        /* Keep LCD_BL asserted continuously; GPIO52 and all DSI pins stay untouched. */
        LCD_BACKLIGHT_ON();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}
#endif

/* CAN callback executes in the TWAI RX task.  The DBC decoder owns all
 * accepted AMI frames; no legacy K2 data is mixed into this snapshot. */
static void can_data_callback(uint32_t can_id, bool is_extended,
                              const uint8_t *data, uint8_t len)
{
    (void)ami_display_can_receive(can_id, is_extended, data, len);
}

/* The display is driven only by the AMI DBC stream once a task is entered. */
static void driving_demo_start(void)
{
    if (s_driving_demo_started) {
        return;
    }
    s_driving_demo_started = true;
    ESP_LOGI(TAG, "Driving data source: AMI DBC (0x620-0x625)");
}

#define AMI_SELECTION_COUNT 6
#define INSPECT_STAGE_COUNT 5

static const char *const s_ami_task_names[AMI_SELECTION_COUNT] = {
    "MANUAL DRIVING",
    "ACCELERATION",
    "SKIDPAD",
    "TRACKDRIVE",
    "EBS TEST",
    "INSPECTION",
};

/* Compact labels for the Autonomous dashboard's limited left-side space. */
static const char *const s_autonomous_task_labels[AMI_SELECTION_COUNT] = {
    "TASK: MANUAL",
    "TASK: ACCEL",
    "TASK: SKIDPAD",
    "TASK: TRACK",
    "TASK: EBS",
    "TASK: INSPECT",
};

static lv_obj_t *s_can_dashboard;

typedef struct
{
    lv_obj_t *online;
    lv_obj_t *command_rpm;
    lv_obj_t *actual_rpm;
    lv_obj_t *command_torque;
    lv_obj_t *actual_torque;
    lv_obj_t *controller_temp;
    lv_obj_t *motor_temp;
    lv_obj_t *dc_bus_voltage;
    lv_obj_t *dc_bus_current;
    lv_obj_t *ac_rms_current;
    lv_obj_t *dc_power;
    lv_obj_t *command_state;
    lv_obj_t *mcu_state;
    lv_obj_t *readiness;
    lv_obj_t *faults;
    lv_obj_t *alarms;
} can_dashboard_widgets_t;

static can_dashboard_widgets_t s_can_widgets;

static lv_obj_t *can_dashboard_card(lv_obj_t *parent, int x, int y, int w, int h,
                                    const char *title, bool large_value)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x68717d), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);

    lv_obj_t *caption = lv_label_create(card);
    lv_obj_set_pos(caption, 0, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(0x9aa7b4), LV_PART_MAIN);
    lv_obj_set_style_text_font(caption, &ui_font_orbitron_bold_20, LV_PART_MAIN);
    lv_label_set_text(caption, title);

    lv_obj_t *value = lv_label_create(card);
    lv_obj_set_pos(value, 0, large_value ? 43 : 38);
    lv_obj_set_size(value, w - 28, h - 55);
    lv_obj_set_style_text_color(value, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(value, large_value ? &ui_font_orbitron_bold_50
                                                  : &ui_font_orbitron_bold_20,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_label_set_text(value, "--");
    return value;
}

static void autonomous_can_dashboard_init(void)
{
    if (!lvgl_port_lock(portMAX_DELAY))
        return;

    if (!s_can_dashboard)
    {
        /* CAN接收调试面板使用独立LVGL screen，不侵入原AUTONOMOUS界面。 */
        s_can_dashboard = lv_obj_create(NULL);
        lv_obj_set_size(s_can_dashboard, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_can_dashboard, lv_color_hex(0x070b10), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_can_dashboard, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_pos(s_can_dashboard, 0, 0);

        lv_obj_t *title = lv_label_create(s_can_dashboard);
        lv_obj_set_pos(title, 55, 24);
        lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(title, &ui_font_orbitron_bold_30, LV_PART_MAIN);
        lv_label_set_text(title, "AMI DISPLAY CAN DIAGNOSTICS");

        s_can_widgets.online = lv_label_create(s_can_dashboard);
        lv_obj_set_pos(s_can_widgets.online, 1600, 25);
        lv_obj_set_size(s_can_widgets.online, 260, 48);
        lv_obj_set_style_bg_opa(s_can_widgets.online, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_can_widgets.online, lv_color_hex(0x68717d), LV_PART_MAIN);
        lv_obj_set_style_radius(s_can_widgets.online, 8, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_can_widgets.online, lv_color_hex(0x050607), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_can_widgets.online, &ui_font_orbitron_bold_20, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_can_widgets.online, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(s_can_widgets.online, "CAN OFFLINE");

        s_can_widgets.command_rpm = can_dashboard_card(s_can_dashboard, 50, 95, 430, 165,
                                                        "MOTOR SPEED (RPM)", true);
        s_can_widgets.actual_rpm = can_dashboard_card(s_can_dashboard, 515, 95, 430, 165,
                                                       "MOTOR POWER (kW)", true);
        s_can_widgets.command_torque = can_dashboard_card(s_can_dashboard, 980, 95, 430, 165,
                                                           "THROTTLE (%)", true);
        s_can_widgets.actual_torque = can_dashboard_card(s_can_dashboard, 1445, 95, 430, 165,
                                                          "BRAKE (%)", true);

        s_can_widgets.controller_temp = can_dashboard_card(s_can_dashboard, 50, 290, 280, 165,
                                                            "CTRL TEMP (C)", true);
        s_can_widgets.motor_temp = can_dashboard_card(s_can_dashboard, 360, 290, 280, 165,
                                                       "MOTOR TEMP (C)", true);
        s_can_widgets.dc_bus_voltage = can_dashboard_card(s_can_dashboard, 670, 290, 280, 165,
                                                          "DC BUS (V)", true);
        s_can_widgets.dc_bus_current = can_dashboard_card(s_can_dashboard, 980, 290, 280, 165,
                                                          "DRIVE BUS (A)", true);
        s_can_widgets.ac_rms_current = can_dashboard_card(s_can_dashboard, 1290, 290, 280, 165,
                                                          "LV BATTERY (V)", true);
        s_can_widgets.dc_power = can_dashboard_card(s_can_dashboard, 1600, 290, 275, 165,
                                                    "STEER ACTUAL (deg)", true);

        s_can_widgets.command_state = can_dashboard_card(s_can_dashboard, 50, 485, 575, 225,
                                                          "CONTROL OWNERS", false);
        s_can_widgets.mcu_state = can_dashboard_card(s_can_dashboard, 672, 485, 575, 225,
                                                      "GEAR / LINKS", false);
        s_can_widgets.readiness = can_dashboard_card(s_can_dashboard, 1295, 485, 580, 225,
                                                      "SAFETY", false);
        s_can_widgets.faults = can_dashboard_card(s_can_dashboard, 50, 740, 897, 275,
                                                   "AS STATUS / PRESSURES", false);
        s_can_widgets.alarms = can_dashboard_card(s_can_dashboard, 977, 740, 898, 275,
                                                   "SOURCE HEALTH", false);
    }

    lvgl_port_unlock();
}

static const char *ami_owner_name(uint8_t owner)
{
    switch (owner) {
    case 1: return "VCU";
    case 2: return "RC";
    case 3: return "TEST";
    default: return "NONE";
    }
}

static const char *ami_gear_name(uint8_t gear)
{
    switch (gear) {
    case 1: return "PARK";
    case 2: return "REVERSE";
    case 3: return "NEUTRAL";
    case 4: return "DRIVE";
    case 255: return "UNAVAILABLE";
    default: return "INVALID";
    }
}

static const char *ami_source_name(uint8_t state)
{
    switch (state) {
    case 1: return "FRESH";
    case 2: return "TIMEOUT";
    default: return "NEVER";
    }
}

static const char *ami_as_state_name(uint8_t state)
{
    switch (state) {
    case 1: return "NORMAL";
    case 2: return "FAULT";
    case 3: return "GO";
    default: return "OFFLINE";
    }
}

static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

static void dashboard_set_text(lv_obj_t *label, const char *text, uint32_t color)
{
    if (!label)
        return;
    label_set_if_changed(label, text);
    const lv_color_t wanted = lv_color_hex(color);
    if (lv_color_to32(lv_obj_get_style_text_color(label, LV_PART_MAIN)) !=
        lv_color_to32(wanted))
    {
        lv_obj_set_style_text_color(label, wanted, LV_PART_MAIN);
    }
}

static void dbc_value_or_dash(char *out, size_t size, bool valid,
                              const char *format, float value)
{
    if (valid) {
        snprintf(out, size, format, value);
    } else {
        strlcpy(out, "--", size);
    }
}

/* AUTONOMOUS status cards are deliberately driven only by decoded DBC data.
 * No received frame, a stale frame, or an explicit non-normal state is red. */
static void autonomous_status_card(lv_obj_t *card, lv_obj_t *title, bool normal)
{
    const lv_color_t color = lv_color_hex(normal ? 0x39ff65 : 0xff3030);
    if (card) {
        lv_obj_set_style_border_color(card, color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(card, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(card, color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(card, normal ? LV_OPA_20 : LV_OPA_30,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (title) {
        lv_obj_set_style_text_color(title, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void autonomous_status_indicators(const ami_display_can_snapshot_t *data,
                                         bool pedals_fresh, bool as_fresh,
                                         bool control_fresh)
{
    const bool ebs_ok = as_fresh && data->as_status.ebs_state == 1;
    const bool sbw_ok = as_fresh && data->as_status.sbw_state == 1;
    const bool ehb_ok = as_fresh && data->as_status.ehb_state == 1;
    const bool as_ok = pedals_fresh && data->pedals.as_positive;
    /* BMSDischargeAvailable is an upstream estimate.  It is used here only
     * for the visual AMS/R2D indication, never as a control interlock. */
    const bool ams_ok = control_fresh &&
                        data->control_status.bms_discharge_available == 1;
    const bool ts_ok = control_fresh && pedals_fresh &&
                       data->control_status.sdc_closed && data->pedals.air_positive;
    const bool r2d_ok = ts_ok && ams_ok && data->control_status.actual_gear == 4;

    autonomous_status_card(objects.ebs, objects.obj3, ebs_ok);
    autonomous_status_card(objects.as, objects.obj7, as_ok);
    autonomous_status_card(objects.ams, objects.obj6, ams_ok);
    autonomous_status_card(objects.ts_2, objects.obj10, ts_ok);
    autonomous_status_card(objects.r2d, objects.r2d_1, r2d_ok);
    autonomous_status_card(objects.sbw, objects.obj8, sbw_ok);
    autonomous_status_card(objects.ebh, objects.obj9, ehb_ok);
}

static void autonomous_can_dashboard_update(const ami_display_can_snapshot_t *data,
                                            bool online)
{
    if (!data || !objects.autonomous) {
        return;
    }

    const bool motor_fresh = ami_display_can_frame_is_fresh(&data->motor.meta,
                                                              AMI_DBC_FRAME_TIMEOUT_MS);
    const bool pedals_fresh = ami_display_can_frame_is_fresh(&data->pedals.meta,
                                                               AMI_DBC_FRAME_TIMEOUT_MS);
    const bool as_fresh = ami_display_can_frame_is_fresh(&data->as_status.meta,
                                                           AMI_DBC_FRAME_TIMEOUT_MS);
    const bool electrical_fresh = ami_display_can_frame_is_fresh(&data->electrical.meta,
                                                                   AMI_DBC_FRAME_TIMEOUT_MS);
    const bool control_fresh = ami_display_can_frame_is_fresh(&data->control_status.meta,
                                                                AMI_DBC_FRAME_TIMEOUT_MS);
    const bool angles_fresh = ami_display_can_frame_is_fresh(&data->angles_and_sources.meta,
                                                               AMI_DBC_FRAME_TIMEOUT_MS);
    /* The gateway transmits zero for an upstream timeout.  When its source
     * status is available, prefer an explicit dash to a believable zero. */
    const bool motor_speed_valid = motor_fresh &&
        (!angles_fresh || data->angles_and_sources.source_state[3] == 1);
    const bool electrical_values_valid = electrical_fresh &&
        (!angles_fresh || data->angles_and_sources.source_state[4] == 1);

    autonomous_status_indicators(data, pedals_fresh, as_fresh, control_fresh);

    /* The DBC has no SOC or LV current; do not fabricate either value. */
    label_set_if_changed(objects.batt, "--");
    char lv_batt[20];
    dbc_value_or_dash(lv_batt, sizeof(lv_batt), pedals_fresh, "%.0f",
                      (float)data->pedals.low_voltage_battery_v);
    label_set_if_changed(objects.lv_volt, lv_batt);
    label_set_if_changed(objects.lv_a, "--");

    char speed[20], power[20], throttle[20], brake[20];
    char controller_temp[20], motor_temp[20], dc_bus[20], dc_current[20], steering[20];
    dbc_value_or_dash(speed, sizeof(speed), motor_speed_valid, "%.0f",
                      data->motor.motor_speed_rpm);
    dbc_value_or_dash(power, sizeof(power), motor_fresh, "%.1f", data->motor.motor_power_kw);
    dbc_value_or_dash(throttle, sizeof(throttle), pedals_fresh, "%.1f", data->pedals.throttle_percent);
    dbc_value_or_dash(brake, sizeof(brake), pedals_fresh, "%.1f", data->pedals.brake_percent);
    dbc_value_or_dash(controller_temp, sizeof(controller_temp), motor_fresh, "%.1f",
                      data->motor.controller_temperature_c);
    dbc_value_or_dash(motor_temp, sizeof(motor_temp), motor_fresh, "%.1f",
                      data->motor.motor_temperature_c);
    dbc_value_or_dash(dc_bus, sizeof(dc_bus), electrical_values_valid, "%.1f",
                      data->electrical.drive_bus_voltage_v);
    dbc_value_or_dash(dc_current, sizeof(dc_current), electrical_values_valid, "%.1f",
                      data->electrical.drive_bus_current_a);
    const bool steering_valid = angles_fresh &&
                               data->angles_and_sources.steering_actual_angle_deg > -3276.8f;
    dbc_value_or_dash(steering, sizeof(steering), steering_valid, "%.1f",
                      data->angles_and_sources.steering_actual_angle_deg);

    if (s_can_widgets.online)
    {
        label_set_if_changed(s_can_widgets.online, online ? "CAN ONLINE" : "CAN OFFLINE");
        const lv_color_t wanted = lv_color_hex(online ? 0x39ff65 : 0xff2c2c);
        if (lv_color_to32(lv_obj_get_style_bg_color(s_can_widgets.online, LV_PART_MAIN)) !=
            lv_color_to32(wanted))
        {
            lv_obj_set_style_bg_color(s_can_widgets.online, wanted, LV_PART_MAIN);
        }
    }

    dashboard_set_text(s_can_widgets.command_rpm, speed, 0xffffff);
    dashboard_set_text(s_can_widgets.actual_rpm, power, 0xf7ff00);
    dashboard_set_text(s_can_widgets.command_torque, throttle, 0x39ff65);
    dashboard_set_text(s_can_widgets.actual_torque, brake, 0xff5050);
    dashboard_set_text(s_can_widgets.controller_temp, controller_temp, 0xffffff);
    dashboard_set_text(s_can_widgets.motor_temp, motor_temp, 0xffffff);
    dashboard_set_text(s_can_widgets.dc_bus_voltage, dc_bus, 0xffffff);
    dashboard_set_text(s_can_widgets.dc_bus_current, dc_current, 0xffffff);
    dashboard_set_text(s_can_widgets.ac_rms_current, lv_batt, 0xffffff);
    dashboard_set_text(s_can_widgets.dc_power, steering, 0xffffff);

    char detail[192];
    if (control_fresh) {
        snprintf(detail, sizeof(detail), "DRIVE  %s\nSTEER  %s\nBRAKE  %s",
                 ami_owner_name(data->control_status.drive_control_owner),
                 ami_owner_name(data->control_status.steering_control_owner),
                 ami_owner_name(data->control_status.brake_control_owner));
        dashboard_set_text(s_can_widgets.command_state, detail, 0xffffff);
        snprintf(detail, sizeof(detail), "ACTUAL  %s\nCOMMAND  %s\nRC LINK  %s",
                 ami_gear_name(data->control_status.actual_gear),
                 ami_gear_name(data->control_status.commanded_gear),
                 data->control_status.rc_link_connected ? "CONNECTED" : "OFFLINE");
        dashboard_set_text(s_can_widgets.mcu_state, detail, 0xffffff);
        snprintf(detail, sizeof(detail), "SDC  %s\nBMS DISCHARGE  %s",
                 data->control_status.sdc_closed ? "CLOSED" : "OPEN",
                 data->control_status.bms_discharge_available == 1 ? "AVAILABLE" :
                 data->control_status.bms_discharge_available == 2 ? "UNAVAILABLE" : "UNKNOWN");
        dashboard_set_text(s_can_widgets.readiness, detail,
                           data->control_status.sdc_closed ? 0x39ff65 : 0xffd400);
    } else {
        dashboard_set_text(s_can_widgets.command_state, "NO CONTROL STATUS", 0x68717d);
        dashboard_set_text(s_can_widgets.mcu_state, "--", 0x68717d);
        dashboard_set_text(s_can_widgets.readiness, "--", 0x68717d);
    }

    if (as_fresh) {
        snprintf(detail, sizeof(detail),
                 "EBS  %s    RES  %s\nSBW  %s    EHB  %s\n"
                 "BRK P1 %.2f MPa    P2 %.2f MPa\nAIR P1 %.2f MPa    P2 %.2f MPa",
                 ami_as_state_name(data->as_status.ebs_state),
                 ami_as_state_name(data->as_status.res_state),
                 ami_as_state_name(data->as_status.sbw_state),
                 ami_as_state_name(data->as_status.ehb_state),
                 data->as_status.brake_pressure_1_mpa, data->as_status.brake_pressure_2_mpa,
                 data->as_status.air_pressure_1_mpa, data->as_status.air_pressure_2_mpa);
        dashboard_set_text(s_can_widgets.faults, detail,
                           (data->as_status.ebs_state == 2 || data->as_status.sbw_state == 2 ||
                            data->as_status.ehb_state == 2) ? 0xff2c2c : 0x39ff65);
    } else {
        dashboard_set_text(s_can_widgets.faults, "NO AS STATUS", 0x68717d);
    }

    if (angles_fresh) {
        snprintf(detail, sizeof(detail), "ECTL SPEED  %s    ECTL ELEC  %s\n"
                 "BMS INFO    %s    BMS POWER %s\n"
                 "SBW SOURCE  %s    VCU 0x503 %s",
                 ami_source_name(data->angles_and_sources.source_state[3]),
                 ami_source_name(data->angles_and_sources.source_state[4]),
                 ami_source_name(data->angles_and_sources.source_state[5]),
                 ami_source_name(data->angles_and_sources.source_state[6]),
                 ami_source_name(data->angles_and_sources.source_state[1]),
                 ami_source_name(data->angles_and_sources.source_state[14]));
        dashboard_set_text(s_can_widgets.alarms, detail,
                           (data->angles_and_sources.source_state[3] == 1 &&
                            data->angles_and_sources.source_state[4] == 1) ?
                           0x39ff65 : 0xffd400);
    } else {
        dashboard_set_text(s_can_widgets.alarms, "NO SOURCE STATUS", 0x68717d);
    }

    label_set_if_changed(objects.speed_label_3, speed);
    label_set_if_changed(objects.motor_tem, motor_temp);
    label_set_if_changed(objects.rotating_speed, controller_temp);
    label_set_if_changed(objects.ts_volt, dc_bus);
    label_set_if_changed(objects.ts_a, dc_current);
    label_set_if_changed(objects.__, power);
}

static void driving_dashboard_update(const ami_display_can_snapshot_t *data)
{
    if (!data || !objects.driver_view) {
        return;
    }
    const bool motor_fresh = ami_display_can_frame_is_fresh(&data->motor.meta,
                                                              AMI_DBC_FRAME_TIMEOUT_MS);
    const bool pedals_fresh = ami_display_can_frame_is_fresh(&data->pedals.meta,
                                                               AMI_DBC_FRAME_TIMEOUT_MS);
    const bool angles_fresh = ami_display_can_frame_is_fresh(&data->angles_and_sources.meta,
                                                               AMI_DBC_FRAME_TIMEOUT_MS);
    const bool motor_speed_valid = motor_fresh &&
        (!angles_fresh || data->angles_and_sources.source_state[3] == 1);
    char text[20];
    dbc_value_or_dash(text, sizeof(text), motor_speed_valid, "%.0f",
                      data->motor.motor_speed_rpm);
    label_set_if_changed(objects.speed_label_2, text);
    dbc_value_or_dash(text, sizeof(text), motor_fresh, "%.1f", data->motor.motor_temperature_c);
    label_set_if_changed(objects.tempmotor_label, text);
    dbc_value_or_dash(text, sizeof(text), motor_fresh, "%.1f", data->motor.controller_temperature_c);
    label_set_if_changed(objects.temp_inv_label, text);
    if (pedals_fresh) {
        lv_bar_set_value(objects.brake_presure_bar, (int32_t)data->pedals.brake_percent,
                         LV_ANIM_OFF);
        lv_bar_set_value(objects.accellerator_presure_bar,
                         (int32_t)data->pedals.throttle_percent, LV_ANIM_OFF);
    }
}

/* INSPECT left-side READY CONDITIONS.  These six rows were generated with
 * green demonstration defaults; drive both their dot and value from the DBC
 * so an absent/stale bus is visibly unsafe instead of deceptively ready. */
static void inspect_condition_set(lv_obj_t *dot, lv_obj_t *value, bool normal,
                                  const char *text)
{
    const lv_color_t color = lv_color_hex(normal ? 0x39ff65 : 0xff3030);
    if (dot && lv_color_to32(lv_obj_get_style_bg_color(dot, LV_PART_MAIN)) !=
                   lv_color_to32(color)) {
        lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(dot, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (value) {
        label_set_if_changed(value, text);
        if (lv_color_to32(lv_obj_get_style_text_color(value, LV_PART_MAIN)) !=
            lv_color_to32(color)) {
            lv_obj_set_style_text_color(value, color, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void inspect_conditions_update(const ami_display_can_snapshot_t *data)
{
    if (!data || !objects.inspect) {
        return;
    }

    const bool control_fresh = ami_display_can_frame_is_fresh(&data->control_status.meta,
                                                                AMI_DBC_FRAME_TIMEOUT_MS);
    const bool as_fresh = ami_display_can_frame_is_fresh(&data->as_status.meta,
                                                           AMI_DBC_FRAME_TIMEOUT_MS);

    const bool asms_ok = control_fresh &&
                         data->control_status.bms_discharge_available == 1;
    const bool tsms_ok = control_fresh && data->control_status.sdc_closed;
    const bool res_ok = as_fresh && (data->as_status.res_state == 1 ||
                                     data->as_status.res_state == 3);
    const bool ebs_ok = as_fresh && data->as_status.ebs_state == 1;
    const bool steering_ok = as_fresh && data->as_status.sbw_state == 1;
    const bool brake_ok = as_fresh && data->as_status.ehb_state == 1;

    inspect_condition_set(objects.obj21, objects.obj23, asms_ok,
                          !control_fresh ? "NO CAN" :
                          asms_ok ? "AVAILABLE" : "UNAVAILABLE");
    inspect_condition_set(objects.obj24, objects.obj26, tsms_ok,
                          !control_fresh ? "NO CAN" : tsms_ok ? "CLOSED" : "OPEN");
    inspect_condition_set(objects.obj27, objects.obj29, res_ok,
                          !as_fresh ? "NO CAN" : data->as_status.res_state == 3 ? "GO" :
                          res_ok ? "NORMAL" : "FAULT");
    inspect_condition_set(objects.obj30, objects.obj32, ebs_ok,
                          !as_fresh ? "NO CAN" : ebs_ok ? "ENABLED" : "FAULT");
    inspect_condition_set(objects.obj33, objects.obj35, steering_ok,
                          !as_fresh ? "NO CAN" : steering_ok ? "READY" : "FAULT");
    inspect_condition_set(objects.obj36, objects.obj38, brake_ok,
                          !as_fresh ? "NO CAN" : brake_ok ? "ENABLED" : "FAULT");
}

#define ASSI_OFF_COLOR 0x68717d
#define ASSI_YELLOW_COLOR 0xffd400
#define ASSI_BLUE_COLOR 0x168bff

/* 主页面六个任务按屏幕上的顺时针顺序排列。 */
static lv_obj_t *ami_selection_led(uint8_t index)
{
    lv_obj_t *leds[AMI_SELECTION_COUNT] = {
        objects.driving, objects.line_acc, objects.eight,
        objects.high_foll, objects.ebs_1, objects.check};
    return index < AMI_SELECTION_COUNT ? leds[index] : NULL;
}

/* AMI selected-task overlay: kept outside the generated UI so future EEZ
 * exports do not overwrite the interactive task-selection presentation. */
static lv_obj_t *s_ami_selection_card;
static lv_obj_t *s_ami_selection_label;
static lv_obj_t *s_ami_selection_caption;
static lv_obj_t *s_ami_selection_hint;
static lv_obj_t *s_ami_selection_accent;
#if ROTARY_INPUT_ENABLE
static lv_obj_t *s_ami_rotary_debug_label;
#endif

static void inspect_show_stage(uint8_t stage);

/* Generated EEZ pages have fixed 1920x1080 geometry.  Dynamic labels need
 * explicit overflow behaviour so an unexpected CAN/status string never
 * shifts, overlaps, or escapes its panel. */
static void label_layout_guard(lv_obj_t *label, lv_label_long_mode_t mode,
                               lv_text_align_t align)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, mode);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void ui_apply_text_layout_guards(void)
{
    /* AUTONOMOUS: numeric fields are always one line; clipping prevents an
     * unexpected string from colliding with surrounding instruments. */
    label_layout_guard(objects.obj2, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.speed_label_3, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.motor_tem, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.rotating_speed, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.ts_volt, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.ts_a, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.__, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);

    /* DRIVER_VIEW: protect the live CAN numerics and generated text fields. */
    label_layout_guard(objects.speed_label, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.speed_label_2, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.tempmotor_label, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.temp_inv_label, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.ready_label, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);

    /* INSPECTION has deliberately wide panels for multi-line status text. */
    label_layout_guard(objects.inspect_as_state, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_test_timer, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_main_action, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_main_detail, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_main_note, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_ebs_value, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_ts_value, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_brake_value, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_steering_value, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_CENTER);
    label_layout_guard(objects.inspect_last_command, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_LEFT);
    label_layout_guard(objects.inspect_trigger_source, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_LEFT);
    label_layout_guard(objects.inspect_reset_required, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_LEFT);
    label_layout_guard(objects.inspect_reset_instruction, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_LEFT);
    label_layout_guard(objects.inspect_footer_status, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_LEFT);
    label_layout_guard(objects.inspect_data_age, LV_LABEL_LONG_CLIP, LV_TEXT_ALIGN_RIGHT);
}

static void ami_show_selection(uint8_t selected)
{
    if (!objects.ami)
        return;

    if (!s_ami_selection_card)
    {
        /* Top-right task card: it is clear of the six circular task LEDs and
         * replaces the old label that sat over the centre of the dashboard. */
        s_ami_selection_card = lv_obj_create(objects.ami);
        /* Wide, shallow card keeps every task name on one instrument-style
         * line.  It occupies unused top-right space, clear of all task LEDs. */
        lv_obj_set_pos(s_ami_selection_card, 1240, 42);
        lv_obj_set_size(s_ami_selection_card, 620, 160);
        lv_obj_clear_flag(s_ami_selection_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_ami_selection_card, 16,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_ami_selection_card, lv_color_hex(0x07131f),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_ami_selection_card, 220,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_ami_selection_card, lv_color_hex(0x2ed8ff),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(s_ami_selection_card, 210,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_ami_selection_card, 2,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_ami_selection_card, 0,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(s_ami_selection_card, lv_color_hex(0x000000),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(s_ami_selection_card, 120,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(s_ami_selection_card, 12,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_ofs_y(s_ami_selection_card, 5,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);

        s_ami_selection_accent = lv_obj_create(s_ami_selection_card);
        lv_obj_set_pos(s_ami_selection_accent, 0, 18);
        lv_obj_set_size(s_ami_selection_accent, 6, 124);
        lv_obj_clear_flag(s_ami_selection_accent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_ami_selection_accent, 3,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_ami_selection_accent, lv_color_hex(0xff3030),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_ami_selection_accent, LV_OPA_COVER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_ami_selection_accent, 0,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);

        s_ami_selection_caption = lv_label_create(s_ami_selection_card);
        lv_obj_set_pos(s_ami_selection_caption, 26, 13);
        lv_obj_set_size(s_ami_selection_caption, 568, 24);
        lv_obj_set_style_text_align(s_ami_selection_caption, LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_ami_selection_caption, lv_color_hex(0x82bfd0),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(s_ami_selection_caption, &ui_font_orbitron_bold_20,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_letter_space(s_ami_selection_caption, 2,
                                           LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text_static(s_ami_selection_caption, "SELECTED TASK");

        s_ami_selection_label = lv_label_create(s_ami_selection_card);
        lv_obj_set_pos(s_ami_selection_label, 24, 38);
        lv_obj_set_size(s_ami_selection_label, 572, 52);
        lv_obj_set_style_text_align(s_ami_selection_label, LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_ami_selection_label, lv_color_hex(0xf3fbff),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        /* Orbitron 40 retains the dashboard style while fitting the longest
         * name (MANUAL DRIVING) with generous side margins. */
        lv_obj_set_style_text_font(s_ami_selection_label, &ui_font_orbitron_bold_40,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_letter_space(s_ami_selection_label, 0,
                                           LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_long_mode(s_ami_selection_label, LV_LABEL_LONG_CLIP);

        s_ami_selection_hint = lv_label_create(s_ami_selection_card);
        lv_obj_set_pos(s_ami_selection_hint, 26, 122);
        lv_obj_set_size(s_ami_selection_hint, 568, 24);
        lv_obj_set_style_text_align(s_ami_selection_hint, LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_ami_selection_hint, lv_color_hex(0x6e98a8),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(s_ami_selection_hint, &ui_font_orbitron_bold_20,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text_static(s_ami_selection_hint, "SHORT: NEXT   HOLD: ENTER");
    }

    for (uint8_t i = 0; i < AMI_SELECTION_COUNT; ++i)
    {
        lv_obj_t *led = ami_selection_led(i);
        if (led)
        {
            bool is_selected = (i == selected);
            /* The selected AMI task is red; all other task LEDs remain green. */
            lv_led_set_color(led, lv_color_hex(is_selected ? 0xff2020 : 0x00ff73));
            lv_led_set_brightness(led, 255);
        }
    }

    if (selected < AMI_SELECTION_COUNT)
    {
        lv_label_set_text(s_ami_selection_label, s_ami_task_names[selected]);
        lv_obj_clear_flag(s_ami_selection_card, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        /* 未选择时不显示任务卡，避免遮挡主界面。 */
        lv_obj_add_flag(s_ami_selection_card, LV_OBJ_FLAG_HIDDEN);
    }

    /* Each changed LED/label invalidates itself.  Invalidating the parent
     * would turn a single GPIO5 selection step into a full-screen redraw. */
}

static void ami_show_rotary_debug(void)
{
#if ROTARY_INPUT_ENABLE
    if (!objects.ami) {
        return;
    }
    if (!s_ami_rotary_debug_label) {
        s_ami_rotary_debug_label = lv_label_create(objects.ami);
        lv_obj_set_pos(s_ami_rotary_debug_label, 710, 585);
        lv_obj_set_size(s_ami_rotary_debug_label, 500, 34);
        lv_obj_set_style_text_align(s_ami_rotary_debug_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_ami_rotary_debug_label, lv_color_hex(0x9aa7b4),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(s_ami_rotary_debug_label, &ui_font_orbitron_bold_20,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    rotary_input_debug_t debug = {0};
    rotary_input_get_debug(&debug);
    char text[96];
    snprintf(text, sizeof(text), "EC11 A4:%u B3:%u C2:%u  ROT:%lu  KEY:%lu",
             debug.a_level, debug.b_level, debug.c_level,
             (unsigned long)debug.rotation_events,
             (unsigned long)debug.press_events);
    lv_label_set_text(s_ami_rotary_debug_label, text);
#endif
}

/* INSPECTION has its own procedure page.  Every other AMI task enters the
 * Autonomous dashboard and shows the selected task's standard English name. */
static void ami_open_selection(uint8_t selected, uint8_t *inspect_stage,
                               bool *inspect_led_on, TickType_t *inspect_blink_tick)
{
    if (selected == AMI_SELECTION_COUNT - 1) {
        *inspect_stage = 0;
        *inspect_led_on = false;
        *inspect_blink_tick = xTaskGetTickCount();
        inspect_show_stage(*inspect_stage);
        loadScreen(SCREEN_ID_INSPECT);
    } else {
        driving_demo_start();
        if (objects.obj2) {
            lv_label_set_text(objects.obj2, s_autonomous_task_labels[selected]);
        }
        loadScreen(SCREEN_ID_AUTONOMOUS);
    }
}

typedef struct
{
    uint8_t raw_level;
    uint8_t stable_level;
    TickType_t raw_change_tick;
    TickType_t press_start_tick;
    bool press_started_on_ami;
    bool long_press_handled;
} ami_button_state_t;

static bool ami_button_is_pressed(uint8_t level)
{
    return level == AMI_BUTTON_ACTIVE_LEVEL;
}

static void ami_button_state_init(ami_button_state_t *button, lv_obj_t *active_scr)
{
    const TickType_t now = xTaskGetTickCount();
    const uint8_t level = gpio_get_level(AMI_BUTTON_GPIO) ? 1 : 0;

    *button = (ami_button_state_t) {
        .raw_level = level,
        .stable_level = level,
        .raw_change_tick = now,
        .press_start_tick = now,
        .press_started_on_ami = ami_button_is_pressed(level) && active_scr == objects.ami,
        .long_press_handled = false,
    };
}

/* GPIO5 is sampled by the existing 20 ms UI task.  A long press is executed
 * once while held; its later release cannot also become a short press.
 * On AMI it opens the selected task; on every task page it returns to AMI. */
static void ami_button_process(ami_button_state_t *button, lv_obj_t *active_scr,
                               uint8_t *ami_selection, uint8_t *inspect_stage,
                               bool *inspect_led_on, TickType_t *inspect_blink_tick)
{
    const TickType_t now = xTaskGetTickCount();
    const uint8_t raw_level = gpio_get_level(AMI_BUTTON_GPIO) ? 1 : 0;

    if (raw_level != button->raw_level)
    {
        button->raw_level = raw_level;
        button->raw_change_tick = now;
    }

    if (button->stable_level != button->raw_level &&
        now - button->raw_change_tick >= pdMS_TO_TICKS(AMI_BUTTON_DEBOUNCE_MS))
    {
        button->stable_level = button->raw_level;

        if (ami_button_is_pressed(button->stable_level))
        {
            button->press_start_tick = now;
            button->press_started_on_ami = active_scr == objects.ami;
            button->long_press_handled = false;
        }
        else
        {
            /* Releasing a non-long press on AMI advances clockwise once. */
            if (button->press_started_on_ami && !button->long_press_handled &&
                active_scr == objects.ami && lvgl_port_lock(portMAX_DELAY))
            {
                *ami_selection = (uint8_t)((*ami_selection + 1U) % AMI_SELECTION_COUNT);
                ami_show_selection(*ami_selection);
                lvgl_port_unlock();
                ESP_LOGI(TAG, "GPIO5 short press: AMI selection=%u", *ami_selection);
            }
            button->press_started_on_ami = false;
            button->long_press_handled = false;
        }
    }

    if (ami_button_is_pressed(button->stable_level) &&
        !button->long_press_handled &&
        now - button->press_start_tick >= pdMS_TO_TICKS(AMI_BUTTON_LONG_PRESS_MS))
    {
        if (lvgl_port_lock(portMAX_DELAY))
        {
            if (button->press_started_on_ami && active_scr == objects.ami)
            {
                ami_open_selection(*ami_selection, inspect_stage,
                                   inspect_led_on, inspect_blink_tick);
                ESP_LOGI(TAG, "GPIO5 long press: open AMI selection=%u", *ami_selection);
            }
            else if (!button->press_started_on_ami && active_scr != objects.ami)
            {
                /* Keep the task highlight the user came from, without
                 * allowing the later release to advance the selection. */
                ami_show_selection(*ami_selection);
                loadScreen(SCREEN_ID_AMI);
                ESP_LOGI(TAG, "GPIO5 long press: return to AMI menu");
            }
            lvgl_port_unlock();
        }
        button->long_press_handled = true;
    }
}

static void inspect_show_stage(uint8_t stage)
{
    lv_obj_t *circles[INSPECT_STAGE_COUNT] = {
        objects.obj40, objects.obj44, objects.obj48, objects.obj52, objects.obj56};
    lv_obj_t *numbers[INSPECT_STAGE_COUNT] = {
        objects.obj41, objects.obj45, objects.obj49, objects.obj53, objects.obj57};
    lv_obj_t *names[INSPECT_STAGE_COUNT] = {
        objects.obj42, objects.obj46, objects.obj50, objects.obj54, objects.obj58};
    const char *actions[INSPECT_STAGE_COUNT] = {
        "LV ON", "SELF TEST", "READY", "DRIVE", "FINISH"};
    const char *details[INSPECT_STAGE_COUNT] = {
        "SWITCH LV ON", "RUN SYSTEM SELF TEST", "CHECK READY CONDITIONS",
        "BEGIN INSPECTION DRIVE", "INSPECTION COMPLETE"};
    const char *as_states[INSPECT_STAGE_COUNT] = {
        "AS OFF", "SELF TEST", "AS READY", "AS DRIVING", "AS FINISHED"};
    const uint32_t assi_colors[INSPECT_STAGE_COUNT] = {
        ASSI_OFF_COLOR, ASSI_YELLOW_COLOR, ASSI_YELLOW_COLOR,
        ASSI_YELLOW_COLOR, ASSI_BLUE_COLOR};

    uint32_t current_assi_color = assi_colors[stage];

    for (uint8_t i = 0; i < INSPECT_STAGE_COUNT; ++i)
    {
        uint32_t color = i < stage ? 0x39ff65 : (i == stage ? current_assi_color : 0x68717d);
        uint32_t bg = i <= stage ? color : 0x11151a;
        lv_obj_set_style_bg_color(circles[i], lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(circles[i], lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(numbers[i], lv_color_hex(i <= stage ? 0x050607 : color), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(names[i], lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    set_var_ami_main_action(actions[stage]);
    set_var_ami_main_detail(details[stage]);
    set_var_ami_as_state_text(as_states[stage]);
    set_var_ami_as_state_code(stage);
    tick_screen_inspect();

    /* AUTONOMOUS SYSTEM大字和AMOY左侧ASSI指示灯使用同一状态颜色。 */
    lv_color_t assi_color = lv_color_hex(current_assi_color);
    lv_obj_set_style_text_color(objects.inspect_as_state, assi_color,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(objects.inspect_main_action, assi_color,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(objects.inspect_footer_status, assi_color,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(objects.inspect_state_led, assi_color,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(objects.inspect_state_led, assi_color,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    /* AS OFF灭灯，其余动作阶段由定时扫描控制1秒闪烁。 */
    lv_obj_set_style_bg_opa(objects.inspect_state_led,
                            stage == 0 ? LV_OPA_TRANSP : LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    if (stage == 0)
    {
        lv_obj_add_flag(objects.inspect_state_led, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(objects.inspect_state_led, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief       LVGL UI更新任务
 * @param       arg : 未使用
 * @retval      无
 */
static void ui_update_task(void *arg)
{
    uint8_t boot_last = 1;
#if BOARD_HAS_XL9555
    uint8_t extio9_last = 1;
    uint8_t extio10_last = 1;
#endif
    uint8_t ami_selection = 0; /* 默认选择有人驾驶/DRIVING */
    uint8_t inspect_stage = 0;
    TickType_t inspect_blink_tick = xTaskGetTickCount();
    bool inspect_led_on = true;
    bool last_can_online = true; /* 强制首次循环将界面更新为CAN OFF */
    TickType_t rotary_debug_tick = 0;
    TickType_t dashboard_update_tick = 0;
    lv_obj_t *last_dashboard_screen = NULL;
    ami_button_state_t ami_button;

    ESP_LOGI(TAG, "UI update task started");
    if (lvgl_port_lock(portMAX_DELAY))
    {
        ui_apply_text_layout_guards();
        ami_show_selection(ami_selection);
        lvgl_port_unlock();
    }
    ami_button_state_init(&ami_button, lv_scr_act());

    while (1)
    {
        ami_display_can_snapshot_t can_data = {0};
        ami_display_can_get_snapshot(&can_data);

        /* === 检测当前显示的屏幕 === */
        lv_obj_t *active_scr = lv_scr_act();

        /* Update immediately after a page switch, then at a bounded visual
         * rate.  CAN reception and its smoothing calculation remain 20 ms. */
        if (active_scr != last_dashboard_screen)
        {
            last_dashboard_screen = active_scr;
            dashboard_update_tick = 0;
        }
        const TickType_t dashboard_now = xTaskGetTickCount();
        const bool dashboard_due = dashboard_update_tick == 0 ||
                                   dashboard_now - dashboard_update_tick >=
                                       pdMS_TO_TICKS(UI_DASHBOARD_UPDATE_MS);

        if (active_scr == objects.ami &&
            xTaskGetTickCount() - rotary_debug_tick >= pdMS_TO_TICKS(100) &&
            lvgl_port_lock(portMAX_DELAY))
        {
            rotary_debug_tick = xTaskGetTickCount();
            ami_show_rotary_debug();
            lvgl_port_unlock();
        }

        /* DBC sends every recognised AMI frame at 20 Hz.  A 250 ms window
         * tolerates a few dropped frames without hiding a live dashboard. */
        bool can_online = ami_display_can_any_frame_is_fresh(&can_data,
                                                               AMI_DBC_FRAME_TIMEOUT_MS);

        /* 两页都接收实时数据：AUTONOMOUS更新原有数值，调试页更新CAN明细。 */
        if (dashboard_due &&
            (active_scr == objects.autonomous || active_scr == s_can_dashboard) &&
            lvgl_port_lock(portMAX_DELAY))
        {
            autonomous_can_dashboard_update(&can_data, can_online);
            dashboard_update_tick = xTaskGetTickCount();
            lvgl_port_unlock();
        }
        else if (dashboard_due && active_scr == objects.driver_view &&
                 lvgl_port_lock(portMAX_DELAY))
        {
            driving_dashboard_update(&can_data);
            dashboard_update_tick = xTaskGetTickCount();
            lvgl_port_unlock();
        }
        else if (dashboard_due && active_scr == objects.inspect &&
                 lvgl_port_lock(portMAX_DELAY))
        {
            inspect_conditions_update(&can_data);
            dashboard_update_tick = xTaskGetTickCount();
            lvgl_port_unlock();
        }

        if (can_online != last_can_online && objects.inspect_can_status)
        {
            if (lvgl_port_lock(portMAX_DELAY))
            {
                lv_label_set_text(objects.inspect_can_status,
                                  can_online ? "CAN OK" : "CAN OFF");
                lv_obj_set_style_text_color(objects.inspect_can_status,
                                            lv_color_hex(can_online ? 0x39ff65 : 0xff2c2c),
                                            LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_invalidate(objects.inspect_can_status);
                lvgl_port_unlock();
            }
            last_can_online = can_online;
        }

        /* === BOOT / GPIO35：在AUTONOMOUS原界面与CAN调试页之间切换 === */
        {
            uint8_t val = gpio_get_level(BOOT_GPIO_PIN);
            static uint8_t debounce_cnt = 0;
            if (val == 0)
            {
                if (debounce_cnt < 5)
                    debounce_cnt++;
            }
            else
            {
                if (debounce_cnt > 0)
                    debounce_cnt--;
            }
            uint8_t curr = (debounce_cnt >= 4) ? 0 : 1;

            if (boot_last == 1 && curr == 0)
            {
                ESP_LOGI(TAG, "BOOT pressed (GPIO35)");
                if (active_scr == objects.autonomous && s_can_dashboard)
                {
                    lv_scr_load_anim(s_can_dashboard, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
                }
                else
                {
                    loadScreen(SCREEN_ID_AUTONOMOUS);
                }
            }
            boot_last = curr;
        }

        /* === GPIO5：AMI短按顺时针选择；AMI长按进入，页面长按返回AMI === */
        ami_button_process(&ami_button, lv_scr_act(), &ami_selection,
                           &inspect_stage, &inspect_led_on, &inspect_blink_tick);

        /* === 旋钮：主界面左/右选择，按下确认 === */
#if ROTARY_INPUT_ENABLE
        {
            rotary_event_t event;
            while (rotary_input_get_event(&event, 0))
            {
                if (active_scr != objects.ami)
                {
                    /* The EC11 button is also the back key outside the AMI
                     * menu.  Rotation is deliberately ignored on pages. */
                    if (event == ROTARY_EVENT_PRESS)
                    {
                        ami_selection = 0;
                        ami_show_selection(ami_selection);
                        loadScreen(SCREEN_ID_AMI);
                        ESP_LOGI(TAG, "Rotary press: return to AMI menu");
                    }
                    continue;
                }

                if (event == ROTARY_EVENT_LEFT)
                {
                    ami_selection = (ami_selection == UINT8_MAX || ami_selection == 0)
                                        ? (AMI_SELECTION_COUNT - 1)
                                        : (uint8_t)(ami_selection - 1);
                    ami_show_selection(ami_selection);
                    ESP_LOGI(TAG, "Rotary left, selection=%u", ami_selection);
                }
                else if (event == ROTARY_EVENT_RIGHT)
                {
                    ami_selection = (ami_selection == UINT8_MAX)
                                        ? 0
                                        : (uint8_t)((ami_selection + 1) % AMI_SELECTION_COUNT);
                    ami_show_selection(ami_selection);
                    ESP_LOGI(TAG, "Rotary right, selection=%u", ami_selection);
                }
                else if (event == ROTARY_EVENT_PRESS && ami_selection != UINT8_MAX)
                {
                    ami_open_selection(ami_selection, &inspect_stage,
                                       &inspect_led_on, &inspect_blink_tick);
                    ESP_LOGI(TAG, "Rotary press, selection=%u", ami_selection);
                }
            }
        }
#endif

#if BOARD_HAS_XL9555
        /* === EXIO9：确认主页面任务 === */
        {
            uint8_t val = xl9555_extio9_read();
            static uint8_t debounce_cnt = 0;
            if (val == 0)
            {
                if (debounce_cnt < 5)
                    debounce_cnt++;
            }
            else
            {
                if (debounce_cnt > 0)
                    debounce_cnt--;
            }
            uint8_t curr = (debounce_cnt >= 4) ? 0 : 1;

            if (extio9_last == 1 && curr == 0)
            {
                if (active_scr == objects.ami && ami_selection != UINT8_MAX)
                {
                    ami_open_selection(ami_selection, &inspect_stage,
                                       &inspect_led_on, &inspect_blink_tick);
                }
            }
            extio9_last = curr;
        }

        /* === EXIO10：车检页面进入下一阶段，到FINISH后保持 === */
        {
            uint8_t val = xl9555_extio10_read();
            static uint8_t debounce_cnt = 0;
            if (val == 0)
            {
                if (debounce_cnt < 5)
                    debounce_cnt++;
            }
            else
            {
                if (debounce_cnt > 0)
                    debounce_cnt--;
            }
            uint8_t curr = (debounce_cnt >= 4) ? 0 : 1;

            if (extio10_last == 1 && curr == 0 && active_scr == objects.inspect)
            {
                if (inspect_stage + 1 < INSPECT_STAGE_COUNT)
                {
                    ++inspect_stage;
                    inspect_led_on = true;
                    inspect_blink_tick = xTaskGetTickCount();
                    inspect_show_stage(inspect_stage);
                    ESP_LOGI(TAG, "Inspection stage=%u", inspect_stage);
                }
            }
            extio10_last = curr;
        }
#endif

        /* 车检页除AS OFF外：ASSI灯按当前颜色每1秒切换一次亮/灭。 */
        if (active_scr == objects.inspect && inspect_stage > 0)
        {
            TickType_t now = xTaskGetTickCount();
            if ((now - inspect_blink_tick) >= pdMS_TO_TICKS(1000))
            {
                /* 全屏刷新可能持锁较久，必须成功取得锁后才更新节拍。 */
                if (lvgl_port_lock(portMAX_DELAY))
                {
                    inspect_blink_tick = xTaskGetTickCount();
                    inspect_led_on = !inspect_led_on;
                    if (inspect_led_on)
                    {
                        lv_obj_clear_flag(objects.inspect_state_led, LV_OBJ_FLAG_HIDDEN);
                    }
                    else
                    {
                        lv_obj_add_flag(objects.inspect_state_led, LV_OBJ_FLAG_HIDDEN);
                    }
                    lv_obj_invalidate(objects.inspect_state_led);
                    lvgl_port_unlock();
                    ESP_LOGI(TAG, "ASSI LED %s", inspect_led_on ? "ON" : "OFF");
                }
            }
        }
        /* 每20ms检查一次 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    /* Stand-alone 12 V boot must not depend on USB CDC initialisation, but
     * BL remains off until the first normal UI frame is ready. */
    lcd_backlight_boot_prepare();

#if LCD_MINIMAL_TEST
    /*
     * 硬件第一阶段必须先于 USB：若 USB CDC 配置或枚举失败，仍可判断芯片
     * 是否已经运行到应用程序。此时 X2-7 应立即为 3.3 V，背光应点亮。
     */
    const gpio_config_t lcd_probe_cfg = {
        .pin_bit_mask = (1ULL << LCD_BL_PIN) | (1ULL << LCD_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&lcd_probe_cfg));
    LCD_RESET_ASSERT();
    LCD_BACKLIGHT_OFF();
    vTaskDelay(pdMS_TO_TICKS(20));
    LCD_RESET_RELEASE();
    LCD_BACKLIGHT_ON();
    vTaskDelay(pdMS_TO_TICKS(2000));
#endif

    usb_cdc_console_init();
    /* CDC enumeration continues in the background; do not hold display
     * startup for a fixed one-second serial-port delay. */
    ESP_LOGI(TAG, "TinyUSB CDC console ready");
    ESP_LOGW(TAG, "Reset reason: %d", esp_reset_reason());
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "ESP32-P4 silicon revision: %u (DSI build target: v3.x)",
             chip_info.revision);

#if LCD_MINIMAL_TEST
    /* 留出 CDC 枚举时间，之后只执行 LCD 驱动和纯色刷新。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    lcd_minimal_test();
    return;
#endif

#if SCREEN_CAN_TEST_ONLY
    /* Deliberately return after display + CAN-B. No XL9555/I2C, rotary,
     * touch, BOOT key, dashboard or other application task runs. */
    can_screen_test_start();
    return;
#endif

    esp_err_t ret;
    ESP_LOGI(TAG, "System starting...");

    ret = nvs_flash_init(); /* 初始化NVS */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* GPIO15/14 connect to the external CAN transceiver.  The MIPI-only LCD
     * path does not probe or claim the optional RGB data pins. */
    ESP_LOGI(TAG, "Initializing CAN (TX:%d, RX:%d)...", CAN_TX_PIN, CAN_RX_PIN);
    ami_display_can_init();
    if (bsp_can_init(CAN_TX_PIN, CAN_RX_PIN, can_data_callback) == 0)
    {
        ESP_LOGI(TAG, "CAN initialized successfully");
    }
    else
    {
        ESP_LOGW(TAG, "CAN initialization failed");
    }

    /* 初始化 I2C 总线（触摸/正点原子参考外设共用）。 */
    ret = myiic_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C init failed!");
    }
    else
    {
        ESP_LOGI(TAG, "I2C initialized (SCL=32, SDA=33)");
    }

    /* 开发板参考设计的 XL9555 在自制板上未装配，不能轮询一个不存在的
     * I2C 地址；GPIO5 已承担本板的主界面按键。 */
#if BOARD_HAS_XL9555
    ret = xl9555_init(bus_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "XL9555 init failed! EXIO buttons will not work!");
    }
    else
    {
        ESP_LOGI(TAG, "XL9555 initialized (EXIO8/9/10 inputs)");
    }
#else
    ESP_LOGI(TAG, "XL9555 not populated; EXIO buttons disabled");
#endif

    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_cfg));
    ESP_LOGI(TAG, "BOOT initialized (GPIO%d)", BOOT_GPIO_PIN);

    const gpio_config_t ami_button_cfg = {
        .pin_bit_mask = 1ULL << AMI_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ami_button_cfg));
    ESP_LOGI(TAG, "AMI task button initialized (GPIO%d, active-low; short=next, long=open)",
             AMI_BUTTON_GPIO);

    /* 初始化LVGL显示 */
    lvgl_demo(); /* 运行LVGL例程 */

    /* 创建独立CAN接收调试页；启动页保持AUTONOMOUS原界面。 */
    autonomous_can_dashboard_init();

    /* The custom-board MIPI driver does not consume GPIO2/3/4 for panel
     * identification, so initialise the EC11 after the display is ready. */
#if ROTARY_INPUT_ENABLE
    ESP_ERROR_CHECK(rotary_input_init());
#endif

    /* 创建UI更新任务 */
    xTaskCreatePinnedToCore(
        ui_update_task,
        "ui_update",
        4096,
        NULL,
        3,
        NULL,
        tskNO_AFFINITY);

    ESP_LOGI(TAG, "System started");
}
