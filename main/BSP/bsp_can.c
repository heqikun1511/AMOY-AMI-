#include "bsp_can.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/twai.h"

static const char *TAG = "BSP_CAN";

/* CAN接收任务句柄 */
static TaskHandle_t s_can_rx_task = NULL;
/* 用户回调函数 */
static can_data_callback_t s_user_callback = NULL;

static void can_process_alerts(void)
{
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) != ESP_OK) {
        return;
    }

    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
        ESP_LOGE(TAG, "RX queue full; CAN frames were dropped");
    }
    if (alerts & TWAI_ALERT_ABOVE_ERR_WARN) {
        ESP_LOGW(TAG, "CAN error counter above warning limit");
    }
    if (alerts & TWAI_ALERT_ERR_PASS) {
        ESP_LOGE(TAG, "CAN controller entered error-passive state");
    }
    if (alerts & TWAI_ALERT_BUS_OFF) {
        ESP_LOGE(TAG, "CAN controller BUS-OFF; starting recovery");
        esp_err_t ret = twai_initiate_recovery();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initiate CAN recovery: %s", esp_err_to_name(ret));
        }
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGW(TAG, "CAN bus recovered; restarting controller");
        esp_err_t ret = twai_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restart recovered CAN controller: %s",
                     esp_err_to_name(ret));
        }
    }
}

/**=                                                        
 * @brief       CAN接收任务
 * @param       arg : 未使用
 * @retval      无
 */
static void can_rx_task(void *arg)
{
    twai_message_t rx_msg;
    ESP_LOGI(TAG, "CAN receive task started");

    while (1)
    {
        /* 等待接收CAN报文 (阻塞100ms) */
        if (twai_receive(&rx_msg, pdMS_TO_TICKS(100)) == ESP_OK)
        {
            uint32_t can_id = rx_msg.identifier;
            ESP_LOGD(TAG, "RX %s ID: 0x%08X, len=%d", rx_msg.extd ? "EXT" : "STD",
                     can_id, rx_msg.data_length_code);

            /* Forward both existing extended motor frames and the AMI's new
             * standard vehicle-status frames to the application dispatcher. */
            if (s_user_callback)
            {
                s_user_callback(can_id, rx_msg.extd, rx_msg.data, rx_msg.data_length_code);
            }
        }
        can_process_alerts();
    }
}

/**
 * @brief       初始化CAN(TWAI)接口
 * @param       tx_pin : CAN发送引脚(GPIO号)
 * @param       rx_pin : CAN接收引脚(GPIO号)
 * @param       callback : 接收到报文时的回调函数(可为NULL)
 * @retval      0:成功 -1:失败
 */
int bsp_can_init(int tx_pin, int rx_pin, can_data_callback_t callback)
{
    esp_err_t ret;

    /* 保存用户回调 */
    s_user_callback = callback;

    /* AMI DBC uses standard frames.  Accept all here so the application can
     * count/diagnose other traffic without hard-coding a controller filter. */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)tx_pin, (gpio_num_t)rx_pin, TWAI_MODE_NORMAL
    );
    g_config.rx_queue_len = 32;
    g_config.alerts_enabled = TWAI_ALERT_RX_QUEUE_FULL |
                              TWAI_ALERT_ABOVE_ERR_WARN |
                              TWAI_ALERT_ERR_PASS |
                              TWAI_ALERT_BUS_OFF |
                              TWAI_ALERT_BUS_RECOVERED;

    /* TWAI时序配置: 500kbps */
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();

    /* 滤波配置: 接收全部标准和扩展帧；DBC 分发由应用层完成。 */
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    /* 安装驱动 */
    ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install TWAI driver: %s", esp_err_to_name(ret));
        return -1;
    }
    ESP_LOGI(TAG, "TWAI driver installed (TX:%d, RX:%d, 500kbps)", tx_pin, rx_pin);

    /* 启动TWAI */
    ret = twai_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start TWAI: %s", esp_err_to_name(ret));
        twai_driver_uninstall();
        return -1;
    }
    ESP_LOGI(TAG, "TWAI started");

    /* 创建接收任务 */
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        can_rx_task,
        "can_rx",
        4096,
        NULL,
        5,
        &s_can_rx_task,
        tskNO_AFFINITY
    );
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create CAN RX task");
        twai_stop();
        twai_driver_uninstall();
        return -1;
    }
    ESP_LOGI(TAG, "CAN initialized successfully");

    return 0;
}

int bsp_can_get_status(bsp_can_status_t *status)
{
    if (!status) {
        return -1;
    }

    twai_status_info_t twai_status = {0};
    if (twai_get_status_info(&twai_status) != ESP_OK) {
        return -1;
    }

    status->state = (uint32_t)twai_status.state;
    status->tx_error_counter = twai_status.tx_error_counter;
    status->rx_error_counter = twai_status.rx_error_counter;
    status->tx_failed_count = twai_status.tx_failed_count;
    status->bus_error_count = twai_status.bus_error_count;
    return 0;
}

/**
 * @brief       发送CAN扩展帧
 * @param       can_id : 29位扩展帧ID
 * @param       data   : 数据缓冲区
 * @param       len    : 数据长度(≤8)
 * @retval      0:成功 -1:失败
 */
static int bsp_can_send_frame(uint32_t can_id, bool is_extended,
                              const uint8_t *data, uint8_t len)
{
    if ((!is_extended && can_id > 0x7FFU) ||
        (is_extended && can_id > 0x1FFFFFFFU))
    {
        ESP_LOGE(TAG, "Invalid %s CAN ID: 0x%08lX",
                 is_extended ? "extended" : "standard",
                 (unsigned long)can_id);
        return -1;
    }

    twai_message_t tx_msg = {
        .extd = is_extended,
        .identifier = can_id,
        .data_length_code = (len > 8) ? 8 : len,
    };

    if (data && len > 0)
    {
        for (int i = 0; i < tx_msg.data_length_code && i < 8; i++)
        {
            tx_msg.data[i] = data[i];
        }
    }

    esp_err_t ret = twai_transmit(&tx_msg, pdMS_TO_TICKS(10));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send CAN message: %s", esp_err_to_name(ret));
        return -1;
    }
    return 0;
}

int bsp_can_send(uint32_t can_id, uint8_t *data, uint8_t len)
{
    return bsp_can_send_frame(can_id, true, data, len);
}

int bsp_can_send_standard(uint32_t can_id, const uint8_t *data, uint8_t len)
{
    return bsp_can_send_frame(can_id, false, data, len);
}
