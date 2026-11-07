/* 面向 WS281x 兼容灯珠(含 WS2815B)的极简 RMT 编码器。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/rmt_encoder.h"

/*
 * WS2815B 编码器配置
 *
 * resolution_hz : RMT 通道的时钟分辨率(Hz)。
 *                 编码器用它把“微秒”换算成 RMT 的 tick 数(1 tick = 1/resolution_hz 秒)。
 *                 例如 10MHz 时 1 tick = 0.1us, 可以精确表达 0.3us / 0.9us 的码型。
 * reset_us      : 一帧数据发完后需要保持低电平(复位)的时间, 单位微秒。
 *                 WS2815B 至少需要 280us, 否则灯珠不会锁存新数据。
 */
typedef struct {
    uint32_t resolution_hz;
    uint32_t reset_us;
} ws2815_rmt_encoder_config_t;

/**
 * @brief Check that a configuration can be represented by an RMT symbol.
 */
bool ws2815_rmt_encoder_config_is_valid(const ws2815_rmt_encoder_config_t *config);

/**
 * @brief  创建一个 WS2815B 专用的 RMT 编码器
 * @param  config      编码器配置(时钟分辨率 + 复位时间)
 * @param  ret_encoder 输出参数, 返回新建的编码器句柄
 * @retval ESP_OK 成功, 其它为错误码
 */
esp_err_t ws2815_new_rmt_encoder(const ws2815_rmt_encoder_config_t *config,
                                  rmt_encoder_handle_t *ret_encoder);
