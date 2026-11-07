#include <stdlib.h>

#include "esp_check.h"
#include "ws2815_rmt_encoder.h"

static const char *TAG = "ws2815_encoder";

#define WS2815_MIN_RESET_US 280U
#define RMT_DURATION_MAX    0x7fffU

/*
 * WS2815B 的 RMT 编码器实例
 *
 * base          : RMT 框架要求的“基类”, 必须放在结构体第一个成员。
 *                 外部只拿到 &base, 内部用 __containerof 反推出整个结构体指针,
 *                 这是 C 语言里模拟“继承”的常见写法。
 * bytes_encoder : 复用的字节编码器, 负责把 0/1 比特按 WS2815B 的码型展开成 RMT symbol。
 * copy_encoder  : 复用的复制编码器, 把数据原样变成 symbol(不做比特展开),
 *                 这里用来输出帧尾的复位低电平。
 * state         : 编码状态机。0 = 正在编码/发送像素数据; 1 = 正在编码/发送复位码。
 * reset_code    : 帧尾复位低电平对应的 RMT symbol(WS2815B 要求 > 280us)。
 */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes_encoder;
    rmt_encoder_handle_t copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2815_rmt_encoder_t;

/*
 * 编码器核心回调: 由 RMT 驱动在发送数据时反复调用, 直到返回 RMT_ENCODING_COMPLETE 为止。
 *
 * 为什么会被反复调用?
 *   RMT 通道的硬件内存有限(大小由通道的 mem_block_symbols 决定)。当一帧数据
 *   装不下时, 驱动会先取走一部分 symbol 去发送, 然后再次调用本函数继续填充剩余部分。
 *   因此本函数用 ws2815->state 记录进度, 必须支持“从中间继续”。
 *
 * 参数:
 *   encoder    : 编码器句柄(实际指向 base 成员)
 *   channel    : 目标 RMT 发送通道
 *   data       : 待发送的原始数据(这里是每颗灯珠 G/R/B 三个字节)
 *   data_size  : 原始数据字节数
 *   ret_state  : 输出, 本次编码的状态位组合(COMPLETE / MEM_FULL)
 * 返回:
 *   本次一共填充了多少个 RMT symbol
 */
RMT_ENCODER_FUNC_ATTR   /* 放到 IRAM 中执行: 这些编码函数可能在中断/临界区被调用 */
static size_t ws2815_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    /* base 是结构体首成员, 通过它反推回外层结构体, 才能访问 bytes_encoder 等成员。
     * __containerof 相当于 Linux 内核里的 container_of。 */
    ws2815_rmt_encoder_t *ws2815 =
        __containerof(encoder, ws2815_rmt_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;  /* 子编码器返回的状态 */
    rmt_encode_state_t state = RMT_ENCODING_RESET;          /* 要返回给驱动的状态 */
    size_t encoded_symbols = 0;                             /* 本次填充的 symbol 总数 */

    switch (ws2815->state) {
    case 0:
        /* 第一阶段: 把 G/R/B 像素字节交给 bytes_encoder 逐比特展开成码型 symbol */
        encoded_symbols += ws2815->bytes_encoder->encode(ws2815->bytes_encoder, channel,
                                                          data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            /* 像素数据已全部编码完, 下一步该发复位码 */
            ws2815->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            /* 通道硬件内存已满, 立即返回让驱动先把已有数据发出去, 下次接着填 */
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* 故意不写 break(贯穿到 case 1): 若硬件内存还有空间, 顺手把复位码也一起填进去 */
        /* fall through */
    case 1:
        /* 第二阶段: 原样输出复位码(低电平 >280us), 让灯珠锁存这一帧颜色 */
        encoded_symbols += ws2815->copy_encoder->encode(ws2815->copy_encoder, channel,
                                                         &ws2815->reset_code,
                                                         sizeof(ws2815->reset_code),
                                                         &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            /* 复位码也发完, 整帧结束: 状态机归零, 并通过 COMPLETE 通知驱动 */
            ws2815->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
        break;
    }

out:
    *ret_state = state;          /* 回传状态位给 RMT 驱动 */
    return encoded_symbols;      /* 回传本次填充的 symbol 数量 */
}

/*
 * 删除编码器回调(对应 base.del, 由 rmt_del_encoder 调用)。
 * 释放内部用到的两个子编码器, 再释放自身内存。
 * 这里不关心错误, 所以固定返回 ESP_OK。
 */
static esp_err_t ws2815_encoder_del(rmt_encoder_t *encoder)
{
    ws2815_rmt_encoder_t *ws2815 =
        __containerof(encoder, ws2815_rmt_encoder_t, base);
    rmt_del_encoder(ws2815->bytes_encoder);
    rmt_del_encoder(ws2815->copy_encoder);
    free(ws2815);
    return ESP_OK;
}

/*
 * 复位编码器回调(对应 base.reset)。
 * 每次 rmt_transmit 开始前驱动都会调用它, 把状态机和两个子编码器恢复到初始状态;
 * 否则上一帧残留的状态可能串到下一帧(例如被误判为“已经在发复位码”)。
 */
RMT_ENCODER_FUNC_ATTR
static esp_err_t ws2815_encoder_reset(rmt_encoder_t *encoder)
{
    ws2815_rmt_encoder_t *ws2815 =
        __containerof(encoder, ws2815_rmt_encoder_t, base);
    rmt_encoder_reset(ws2815->bytes_encoder);
    rmt_encoder_reset(ws2815->copy_encoder);
    ws2815->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

bool ws2815_rmt_encoder_config_is_valid(const ws2815_rmt_encoder_config_t *config)
{
    if (config == NULL || config->reset_us < WS2815_MIN_RESET_US) {
        return false;
    }

    uint64_t bit0_high_ticks = (uint64_t)config->resolution_hz * 3 / 10000000U;
    uint64_t bit0_low_ticks = (uint64_t)config->resolution_hz * 9 / 10000000U;
    uint64_t half_reset_ticks = (uint64_t)config->resolution_hz * config->reset_us /
                                2000000U;

    return bit0_high_ticks > 0 && bit0_high_ticks <= RMT_DURATION_MAX &&
           bit0_low_ticks > 0 && bit0_low_ticks <= RMT_DURATION_MAX &&
           half_reset_ticks > 0 && half_reset_ticks <= RMT_DURATION_MAX;
}

/**
 * @brief  创建 WS2815B 专用的 RMT 编码器
 * @param  config      配置(时钟分辨率 + 复位时间)
 * @param  ret_encoder 输出, 返回编码器句柄
 * @retval ESP_OK 成功, 其它为错误码
 */
esp_err_t ws2815_new_rmt_encoder(const ws2815_rmt_encoder_config_t *config,
                                  rmt_encoder_handle_t *ret_encoder)
{
    /* 参数校验: 配置指针和输出句柄都不能为空 */
    ESP_RETURN_ON_FALSE(ret_encoder != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid encoder configuration");
    *ret_encoder = NULL;
    ESP_RETURN_ON_FALSE(ws2815_rmt_encoder_config_is_valid(config),
                        ESP_ERR_INVALID_ARG, TAG, "invalid encoder configuration");

    const uint32_t bit0_high_ticks =
        (uint32_t)((uint64_t)config->resolution_hz * 3 / 10000000U);
    const uint32_t bit0_low_ticks =
        (uint32_t)((uint64_t)config->resolution_hz * 9 / 10000000U);
    const uint32_t half_reset_ticks =
        (uint32_t)((uint64_t)config->resolution_hz * config->reset_us / 2000000U);

    /* 分配编码器对象内存(rmt_alloc_encoder_mem 会保证 DMA/IRAM 可访问) */
    ws2815_rmt_encoder_t *ws2815 = rmt_alloc_encoder_mem(sizeof(*ws2815));
    ESP_RETURN_ON_FALSE(ws2815, ESP_ERR_NO_MEM, TAG, "no memory for encoder");

    /* 挂载三个回调, 之后 rmt_transmit 会通过这些回调驱动编码器工作 */
    ws2815->base.encode = ws2815_encode;
    ws2815->base.del = ws2815_encoder_del;
    ws2815->base.reset = ws2815_encoder_reset;

    /* WS2815B 与 WS2812 使用相同的 800kHz、MSB 先发、GRB 顺序协议。 */
    /*
     * 每比特周期约 1.25us, 靠“高/低电平宽度”区分 0 和 1:
     *   0 码: 高 0.3us + 低 0.9us
     *   1 码: 高 0.9us + 低 0.3us
     * duration 的单位是 RMT 时钟 tick, 换算公式: tick = resolution_hz * 秒数
     *   0.3us -> resolution_hz * 0.3e-6 = resolution_hz * 3 / 10000000
     *   0.9us -> resolution_hz * 0.9e-6 = resolution_hz * 9 / 10000000
     * level0/level1 分别是该 symbol 前半段、后半段的电平(1 = 高, 0 = 低)。
     */
    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = bit0_high_ticks, /* 0.3 us 高电平 */
            .level1 = 0,
            .duration1 = bit0_low_ticks, /* 0.9 us 低电平 */
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = bit0_low_ticks, /* 0.9 us 高电平 */
            .level1 = 0,
            .duration1 = bit0_high_ticks, /* 0.3 us 低电平 */
        },
        .flags.msb_first = 1,   /* 高位先发(WS2815B 规定) */
    };

    /* 创建字节编码器: 负责把 0/1 比特展开成上面定义的码型 symbol */
    esp_err_t ret = rmt_new_bytes_encoder(&bytes_config, &ws2815->bytes_encoder);
    if (ret != ESP_OK) {
        free(ws2815);
        return ret;
    }

    /* 创建复制编码器: 不做比特展开, 用于原样输出复位符号 */
    rmt_copy_encoder_config_t copy_config = {};
    ret = rmt_new_copy_encoder(&copy_config, &ws2815->copy_encoder);
    if (ret != ESP_OK) {
        rmt_del_encoder(ws2815->bytes_encoder);   /* 回滚: 释放已创建的子编码器 */
        free(ws2815);
        return ret;
    }

    /*
     * WS2815B 要求帧尾低电平复位时间 >280us。
     * 单个 RMT symbol 的 duration 字段只有 15 bit, 单独一个 symbol 表示不了太长的低电平,
     * 所以把复位时间一分为二: 用两个 duration(电平都为 0)拼出足够长的低电平。
     * half_reset_ticks = resolution_hz * reset_us / 2e6, 即半个复位时间对应的 tick 数。
     */
    ws2815->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = half_reset_ticks,
        .level1 = 0,
        .duration1 = half_reset_ticks,
    };
    *ret_encoder = &ws2815->base;   /* 对外只暴露 base(编码器句柄) */
    return ESP_OK;
}
