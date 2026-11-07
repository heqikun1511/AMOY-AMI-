

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "lvgl_demo.h"
#include "bsp_can.h"
#include "myiic.h"
#include "xl9555.h"
#include "ui.h"
#include "lcd.h"

static const char *TAG = "MAIN";

/* 任意 CAN 帧到达后置位；蜂鸣器由 UI 任务锁存开启。 */
static volatile bool g_can_message_received = false;

/* Single UI button, active low: short press selects next on AMI; long press
 * enters the selected task or returns from a task page to AMI. */
#define TASK_BUTTON_GPIO          GPIO_NUM_36
#define TASK_BUTTON_DEBOUNCE_MS   40
#define TASK_BUTTON_LONG_PRESS_MS 800

/* KEY0和KEY1通过XL9555 IO扩展器连接 (见原理图) */
/* KEY0 = EXIO8 (IO1_0), KEY1 = EXIO7 (IO0_7) */

/* CAN TX引脚 (根据实际硬件修改) */
#define CAN_TX_PIN  GPIO_NUM_23
/* CAN RX引脚 (根据实际硬件修改) */
#define CAN_RX_PIN  GPIO_NUM_51

/*
 * CAN 联调模式：跳过 LCD、触摸、LVGL、旋钮和 UI 任务，使 TWAI 在启动后
 * 立即初始化。收到任意有效 CAN 帧时，板载蜂鸣器会持续鸣响。
 */
#define CAN_DIAGNOSTIC_MODE  0

#if !CAN_DIAGNOSTIC_MODE
typedef enum {
    AMI_TASK_DRIVING = 0,
    AMI_TASK_LINE_ACC,
    AMI_TASK_EIGHT,
    AMI_TASK_HIGH_FOLL,
    AMI_TASK_EBS,
    AMI_TASK_CHECK,
    AMI_TASK_COUNT,
} ami_task_t;

/* The order follows the clockwise LED layout on the AMI background. */
static ami_task_t s_ami_selected_task = AMI_TASK_DRIVING;

static void ami_update_task_leds(void)
{
    lv_obj_t *const leds[AMI_TASK_COUNT] = {
        objects.driving,
        objects.line_acc,
        objects.eight,
        objects.high_foll,
        objects.ebs_1,
        objects.check,
    };

    for (int i = 0; i < AMI_TASK_COUNT; ++i) {
        if (leds[i] == NULL) {
            continue;
        }

        lv_led_set_color(leds[i],
                         lv_color_hex(i == s_ami_selected_task ? 0xff0000
                                                                : 0x00ff73));
        lv_led_set_brightness(leds[i], 255);
    }
}

static void ami_select_next_task(int direction)
{
    int next = (int)s_ami_selected_task + direction;
    if (next < 0) {
        next = AMI_TASK_COUNT - 1;
    } else if (next >= AMI_TASK_COUNT) {
        next = 0;
    }

    s_ami_selected_task = (ami_task_t)next;
    ami_update_task_leds();
}

static void ami_confirm_selected_task(void)
{
    static const char *const task_names[AMI_TASK_COUNT] = {
        [AMI_TASK_DRIVING] = "MANUAL DRIVING",
        [AMI_TASK_LINE_ACC] = "ACCELERATION",
        [AMI_TASK_EIGHT] = "SKIDPAD",
        [AMI_TASK_HIGH_FOLL] = "TRACKDRIVE",
        [AMI_TASK_EBS] = "EBS TEST",
        [AMI_TASK_CHECK] = "INSPECTION",
    };
    static const enum ScreensEnum task_screens[AMI_TASK_COUNT] = {
        [AMI_TASK_DRIVING] = SCREEN_ID_AUTONOMOUS,
        [AMI_TASK_LINE_ACC] = SCREEN_ID_AUTONOMOUS,
        [AMI_TASK_EIGHT] = SCREEN_ID_AUTONOMOUS,
        [AMI_TASK_HIGH_FOLL] = SCREEN_ID_AUTONOMOUS,
        [AMI_TASK_EBS] = SCREEN_ID_AUTONOMOUS,
        [AMI_TASK_CHECK] = SCREEN_ID_INSPECT,
    };

    if (objects.autonomous_task_label != NULL) {
        lv_label_set_text(objects.autonomous_task_label,
                          task_names[s_ami_selected_task]);
    }

    ESP_LOGI(TAG, "AMI task confirmed: %s", task_names[s_ami_selected_task]);
    loadScreen(task_screens[s_ami_selected_task]);
}
#endif

/**
 * @brief       CAN数据接收回调(在CAN任务上下文中调用)
 * @param       can_id : CAN扩展帧ID
 * @param       data   : 数据缓冲区
 * @param       len    : 数据长度
 * @retval      无
 */
static void can_data_callback(uint32_t can_id, uint8_t *data, uint8_t len)
{
    /* 任意硬件接收的 CAN 帧都作为总线通信成功的提示。 */
    g_can_message_received = true;

    /* 当前工程不再解析或显示转速，保留参数以兼容 BSP 回调接口。 */
    (void)can_id;
    (void)data;
    (void)len;
}

#if CAN_DIAGNOSTIC_MODE
/**
 * @brief       CAN 直连诊断时的蜂鸣器任务
 *
 * 不依赖 LVGL，避免显示初始化失败时无法确认 CAN 接收状态。
 */
static void can_buzzer_task(void *arg)
{
    (void)arg;
    bool beep_latched = false;

    while (true) {
        if (g_can_message_received && !beep_latched) {
            esp_err_t ret = xl9555_beep_set(true);
            if (ret == ESP_OK) {
                beep_latched = true;
                ESP_LOGW(TAG, "CAN message received: buzzer latched ON");
            } else {
                ESP_LOGE(TAG, "Failed to enable buzzer: %s", esp_err_to_name(ret));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

/**
 * @brief       LVGL UI更新任务
 * @param       arg : 未使用
 * @retval      无
 */
#if !CAN_DIAGNOSTIC_MODE
static void ui_update_task(void *arg)
{
    bool beep_latched = false;
    uint8_t task_button_last = 1;
    bool task_button_long_handled = false;
    TickType_t task_button_pressed_at = 0;

    ESP_LOGI(TAG, "UI update task started");
    ami_update_task_leds(); /* DRIVING starts selected, shown in red. */

    while (1)
    {
        /*
         * BEEP 接在 XL9555 的 EXIO0（低电平有效）。收到第一帧 CAN 后仅写
         * 一次并保持低电平，蜂鸣器会持续鸣响直到开发板复位/断电。
         * 放在本任务中执行可与按键的 XL9555 I2C 访问串行化。
         */
        if (g_can_message_received && !beep_latched) {
            esp_err_t ret = xl9555_beep_set(true);
            if (ret == ESP_OK) {
                beep_latched = true;
                ESP_LOGW(TAG, "CAN message received: buzzer latched ON");
            } else {
                ESP_LOGE(TAG, "Failed to enable buzzer: %s", esp_err_to_name(ret));
            }
        }

        /* === 检测当前显示的屏幕 === */
        lv_obj_t *active_scr = lv_scr_act();

        /* GPIO36 task selector: a short press advances clockwise; holding it
         * for 800 ms confirms the highlighted task.  Release bounce is
         * rejected by the minimum duration check. */
        {
            uint8_t task_button = gpio_get_level(TASK_BUTTON_GPIO) ? 1 : 0;
            TickType_t now = xTaskGetTickCount();

            if (task_button_last == 1 && task_button == 0) {
                task_button_pressed_at = now;
                task_button_long_handled = false;
            } else if (task_button_last == 0 && task_button == 0 &&
                       !task_button_long_handled &&
                       (now - task_button_pressed_at) >=
                           pdMS_TO_TICKS(TASK_BUTTON_LONG_PRESS_MS)) {
                task_button_long_handled = true;
                if (active_scr == objects.ami) {
                    ESP_LOGI(TAG, "GPIO36 long press: confirm task %d",
                             s_ami_selected_task);
                    ami_confirm_selected_task();
                } else {
                    ESP_LOGI(TAG, "GPIO36 long press: return to AMI");
                    loadScreen(SCREEN_ID_AMI);
                    ami_update_task_leds();
                }
            } else if (task_button_last == 0 && task_button == 1) {
                TickType_t held_ticks = now - task_button_pressed_at;
                if (!task_button_long_handled &&
                    held_ticks >= pdMS_TO_TICKS(TASK_BUTTON_DEBOUNCE_MS) &&
                    active_scr == objects.ami) {
                    ESP_LOGI(TAG, "GPIO36 short press: next task");
                    ami_select_next_task(1);
                }
            }

            task_button_last = task_button;
        }

        /* 每20ms检查一次 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    esp_err_t ret;
    ESP_LOGI(TAG, "System starting...");

    ret = nvs_flash_init();     /* 初始化NVS */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 初始化I2C总线 (XL9555 IO扩展器使用) */
    ret = myiic_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed!");
    } else {
        ESP_LOGI(TAG, "I2C initialized (SCL=32, SDA=33)");
    }

    /* 初始化XL9555 IO扩展器 (KEY0=EXIO8, KEY1=EXIO7) */
    ret = xl9555_init(bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XL9555 init failed! KEY1 will not work!");
    } else {
        ESP_LOGI(TAG, "XL9555 initialized (KEY1=EXIO7, KEY0=EXIO8)");
    }

    gpio_config_t task_button_cfg = {
        .pin_bit_mask = 1ULL << TASK_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&task_button_cfg));
    ESP_LOGI(TAG,
             "Task button initialized on GPIO%d (short=next, long=enter/back)",
             TASK_BUTTON_GPIO);

    /* 初始化LVGL显示 */
#if !CAN_DIAGNOSTIC_MODE
    lvgl_demo();                /* 运行LVGL例程 */
    /* Driver View no longer presents vehicle speed or RPM. */
    if (objects.speed_container) {
        lv_obj_add_flag(objects.speed_container, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    /* 初始化CAN (TWAI) */
    ESP_LOGI(TAG, "Initializing CAN (TX:%d, RX:%d)...", CAN_TX_PIN, CAN_RX_PIN);
    if (bsp_can_init(CAN_TX_PIN, CAN_RX_PIN, can_data_callback) == 0)
    {
        ESP_LOGI(TAG, "CAN initialized successfully");
    }
    else
    {
        ESP_LOGW(TAG, "CAN initialization failed, UI will still work");
    }

    /* CAN 联调时不启动任何 LVGL 相关任务。 */
#if CAN_DIAGNOSTIC_MODE
    if (xTaskCreate(can_buzzer_task, "can_buzzer", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create CAN buzzer task");
    }
#else
    /* 创建UI更新任务 */
    xTaskCreatePinnedToCore(
        ui_update_task,
        "ui_update",
        4096,
        NULL,
        3,
        NULL,
        tskNO_AFFINITY
    );
#endif
    
    ESP_LOGI(TAG, "System started");
    
}
