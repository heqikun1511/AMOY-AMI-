#ifndef __BSP_CAN_H
#define __BSP_CAN_H

#include <stdbool.h>
#include <stdint.h>

/* CAN接收回调函数类型 */
typedef void (*can_data_callback_t)(uint32_t can_id, bool is_extended,
                                    const uint8_t *data, uint8_t len);

typedef struct
{
    uint32_t state;
    uint32_t tx_error_counter;
    uint32_t rx_error_counter;
    uint32_t tx_failed_count;
    uint32_t bus_error_count;
} bsp_can_status_t;

/**
 * @brief       初始化CAN(TWAI)接口
 * @param       tx_pin : CAN发送引脚(GPIO号)
 * @param       rx_pin : CAN接收引脚(GPIO号)
 * @param       callback : 接收到报文时的回调函数(可为NULL)
 * @retval      0:成功 -1:失败
 */
int bsp_can_init(int tx_pin, int rx_pin, can_data_callback_t callback);

/* Obtain controller health counters for a display-only CAN diagnostic. */
int bsp_can_get_status(bsp_can_status_t *status);

/**
 * @brief       发送CAN扩展帧（仅供诊断页测试使用）
 * @param       can_id : 29位扩展帧ID
 * @param       data   : 数据缓冲区
 * @param       len    : 数据长度(≤8)
 * @retval      0:成功 -1:失败
 */
int bsp_can_send(uint32_t can_id, uint8_t *data, uint8_t len);

/**
 * @brief       发送CAN标准帧
 * @param       can_id : 11位标准帧ID
 * @param       data   : 数据缓冲区
 * @param       len    : 数据长度(≤8)
 * @retval      0:成功 -1:失败
 */
int bsp_can_send_standard(uint32_t can_id, const uint8_t *data, uint8_t len);

#endif
