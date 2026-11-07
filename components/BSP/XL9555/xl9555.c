/**
 * @file        xl9555.c
 * @brief       XL9555 I2C IO扩展器驱动实现
 */

#include "xl9555.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "XL9555";

static i2c_master_dev_handle_t xl9555_dev = NULL;
/* 输出锁存的初始复位值为高电平；BEEP 为低电平有效。 */
static uint8_t s_output_port0 = 0xFF;

/**
 * @brief       写XL9555寄存器
 */
static esp_err_t xl9555_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t write_buf[2] = { reg, data };
    return i2c_master_transmit(xl9555_dev, write_buf, sizeof(write_buf), pdMS_TO_TICKS(100));
}

/**
 * @brief       读XL9555寄存器
 */
static esp_err_t xl9555_read_reg(uint8_t reg, uint8_t *data)
{
    return i2c_master_transmit_receive(xl9555_dev, &reg, 1, data, 1, pdMS_TO_TICKS(100));
}

/**
 * @brief       初始化XL9555
 */
esp_err_t xl9555_init(i2c_master_bus_handle_t bus_handle)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_ADDR,
        .scl_speed_hz = 400000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &xl9555_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add XL9555 device");
        return ret;
    }

    /*
     * 先显式关闭低电平有效的蜂鸣器，再将 EXIO0 配置为输出，避免初始化
     * 过程中意外鸣叫。其余端口维持高电平复位状态。
     */
    ret = xl9555_write_reg(XL9555_OUTPUT_PORT0, s_output_port0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set initial output state: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置端口: KEY0(IO1_0)和KEY1(IO0_7)为输入, 其余保持输出 */
    /* Port 0: IO0_7 (KEY1) = 输入, 其余保持输出(0) */
    /* Port 1: IO1_0 (KEY0) = 输入, 其余保持输出(0) */
    ret = xl9555_write_reg(XL9555_CFG_PORT0, XL9555_KEY1_BIT);   /* 仅bit7=1(输入) */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure port 0: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = xl9555_write_reg(XL9555_CFG_PORT1, XL9555_KEY0_BIT);   /* 仅bit0=1(输入) */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure port 1: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "XL9555 initialized (KEY0=EXIO8, KEY1=EXIO7)");
    return ESP_OK;
}

/**
 * @brief       读取KEY0状态 (EXIO8 = IO1_0)
 */
uint8_t xl9555_key0_read(void)
{
    uint8_t val = 0;
    if (xl9555_read_reg(XL9555_INPUT_PORT1, &val) == ESP_OK) {
        return (val & XL9555_KEY0_BIT) ? 1 : 0;
    }
    return 1; /* 读取失败返回松开 */
}

/**
 * @brief       读取KEY1状态 (EXIO7 = IO0_7)
 */
uint8_t xl9555_key1_read(void)
{
    uint8_t val = 0;
    if (xl9555_read_reg(XL9555_INPUT_PORT0, &val) == ESP_OK) {
        return (val & XL9555_KEY1_BIT) ? 1 : 0;
    }
    return 1; /* 读取失败返回松开 */
}

/**
 * @brief       控制蜂鸣器 (EXIO0，低电平有效)
 */
esp_err_t xl9555_beep_set(bool on)
{
    if (xl9555_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t output = on ? (s_output_port0 & ~XL9555_BEEP_BIT)
                        : (s_output_port0 | XL9555_BEEP_BIT);
    esp_err_t ret = xl9555_write_reg(XL9555_OUTPUT_PORT0, output);
    if (ret == ESP_OK) {
        s_output_port0 = output;
    }
    return ret;
}
