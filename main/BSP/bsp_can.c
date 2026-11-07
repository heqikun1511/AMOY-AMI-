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
            /*
             * 将所有硬件已接收的 CAN 帧通知应用层。应用层可自行决定哪些
             * ID 用于业务解析；这样标准帧也能作为总线连通性证明。
             */
            uint32_t can_id = rx_msg.identifier;

            ESP_LOGD(TAG, "RX %s ID: 0x%08X, len=%d",
                     rx_msg.extd ? "extended" : "standard", can_id,
                     rx_msg.data_length_code);

            /* 调用用户回调 */
            if (s_user_callback)
            {
                s_user_callback(can_id, rx_msg.data, rx_msg.data_length_code);
            }
        }
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

    /* TWAI通用配置: 250kbps, 扩展帧 */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)tx_pin, (gpio_num_t)rx_pin, TWAI_MODE_NORMAL
    );
    g_config.rx_queue_len = 32;

    /* TWAI时序配置: 250kbps */
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();

    /* 滤波配置: 接收所有扩展帧(根据协议, 使用扩展29位ID) */
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    /* 安装驱动 */
    ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install TWAI driver: %s", esp_err_to_name(ret));
        return -1;
    }
    ESP_LOGI(TAG, "TWAI driver installed (TX:%d, RX:%d, 250kbps)", tx_pin, rx_pin);

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

/**
 * @brief       发送CAN扩展帧
 * @param       can_id : 29位扩展帧ID
 * @param       data   : 数据缓冲区
 * @param       len    : 数据长度(≤8)
 * @retval      0:成功 -1:失败
 */
int bsp_can_send(uint32_t can_id, uint8_t *data, uint8_t len)
{
    twai_message_t tx_msg = {
        .extd = 1,                          /* 扩展帧 */
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
