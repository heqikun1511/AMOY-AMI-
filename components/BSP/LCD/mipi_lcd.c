/**
 ****************************************************************************************************
 * @file        mipi_lcd.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       mipilcd驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 ESP32-P4 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "mipi_lcd.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *mipi_lcd_tag = "mipi_lcd";
uint8_t mipi_id[2];     /* 存放MIPI屏驱动IC的ID */
_mipilcd_dev mipidev;   /* 管理MIPI重要参数 */

#if MIPI_DSI_ID_READ_DIAGNOSTIC
typedef struct {
    esp_lcd_panel_io_handle_t io;
    TaskHandle_t waiting_task;
    esp_err_t ret_da;
    esp_err_t ret_db;
    uint8_t id_da;
    uint8_t id_db;
} mipi_id_read_ctx_t;

/* DSI reads require D0 to turn around and drive the reply.  Run them in a
 * worker because the IDF v6.0.2 DBI read path has no caller-visible timeout. */
static void mipi_id_read_worker(void *arg)
{
    mipi_id_read_ctx_t *ctx = arg;
    ctx->ret_da = esp_lcd_panel_io_rx_param(ctx->io, 0xDA, &ctx->id_da, 1);
    if (ctx->ret_da == ESP_OK) {
        ctx->ret_db = esp_lcd_panel_io_rx_param(ctx->io, 0xDB, &ctx->id_db, 1);
    } else {
        ctx->ret_db = ESP_FAIL;
    }
    xTaskNotifyGive(ctx->waiting_task);
    vTaskDelete(NULL);
}

static bool mipi_lcd_diagnose_id_read(esp_lcd_panel_io_handle_t io)
{
    mipi_id_read_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        ESP_LOGE(mipi_lcd_tag, "DSI ID diagnostic: no memory for read context");
        return false;
    }
    ctx->io = io;
    ctx->waiting_task = xTaskGetCurrentTaskHandle();
    /* Keep this potentially blocking IDF read below the main task so the
     * caller's 200 ms timeout can always preempt it. */
    if (xTaskCreate(mipi_id_read_worker, "mipi_id_read", 4096, ctx,
                    tskIDLE_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(mipi_lcd_tag, "DSI ID diagnostic: cannot start read task");
        free(ctx);
        return false;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)) == 0) {
        /* Keep ctx allocated: the worker may still be blocked inside IDF. A
         * reset after this one-shot test releases it safely. */
        ESP_LOGE(mipi_lcd_tag,
                 "DSI ID diagnostic TIMEOUT: no D0 LP/BTA reply within 200 ms");
        return false;
    }
    ESP_LOGI(mipi_lcd_tag, "DSI ID diagnostic: 0xDA=0x%02X (%s), 0xDB=0x%02X (%s)",
             ctx->id_da, esp_err_to_name(ctx->ret_da),
             ctx->id_db, esp_err_to_name(ctx->ret_db));
    bool ok = (ctx->ret_da == ESP_OK && ctx->ret_db == ESP_OK);
    free(ctx);
    return ok;
}
#endif

static const mipi_lcd_init_cmd_t vendor_specific_init_code_default_800p[] = {
    /* {cmd, { data }, data_size} */
    /* CMD_Page 3 */
    {ILI9881C_CMD_CNDBKxSEL, (uint8_t []){ILI9881C_CMD_BKxSEL_BYTE0, ILI9881C_CMD_BKxSEL_BYTE1, ILI9881C_CMD_BKxSEL_BYTE2_PAGE3}, 3},
    {0x01, (uint8_t []){0x00}, 1},
    {0x02, (uint8_t []){0x00}, 1},
    {0x03, (uint8_t []){0x53}, 1},
    {0x04, (uint8_t []){0xD3}, 1},
    {0x05, (uint8_t []){0x00}, 1},
    {0x06, (uint8_t []){0x0D}, 1},
    {0x07, (uint8_t []){0x08}, 1},
    {0x08, (uint8_t []){0x00}, 1},
    {0x09, (uint8_t []){0x00}, 1},
    {0x0a, (uint8_t []){0x00}, 1},
    {0x0b, (uint8_t []){0x00}, 1},
    {0x0c, (uint8_t []){0x00}, 1},
    {0x0d, (uint8_t []){0x00}, 1},
    {0x0e, (uint8_t []){0x00}, 1},
    {0x0f, (uint8_t []){0x28}, 1},
    {0x10, (uint8_t []){0x28}, 1},
    {0x11, (uint8_t []){0x00}, 1},
    {0x12, (uint8_t []){0x00}, 1},
    {0x13, (uint8_t []){0x00}, 1},
    {0x14, (uint8_t []){0x00}, 1},
    {0x15, (uint8_t []){0x00}, 1},
    {0x16, (uint8_t []){0x00}, 1},
    {0x17, (uint8_t []){0x00}, 1},
    {0x18, (uint8_t []){0x00}, 1},
    {0x19, (uint8_t []){0x00}, 1},
    {0x1a, (uint8_t []){0x00}, 1},
    {0x1b, (uint8_t []){0x00}, 1},
    {0x1c, (uint8_t []){0x00}, 1},
    {0x1d, (uint8_t []){0x00}, 1},
    {0x1e, (uint8_t []){0x40}, 1},
    {0x1f, (uint8_t []){0x80}, 1},
    {0x20, (uint8_t []){0x06}, 1},
    {0x21, (uint8_t []){0x01}, 1},
    {0x22, (uint8_t []){0x00}, 1},
    {0x23, (uint8_t []){0x00}, 1},
    {0x24, (uint8_t []){0x00}, 1},
    {0x25, (uint8_t []){0x00}, 1},
    {0x26, (uint8_t []){0x00}, 1},
    {0x27, (uint8_t []){0x00}, 1},
    {0x28, (uint8_t []){0x33}, 1},
    {0x29, (uint8_t []){0x33}, 1},
    {0x2a, (uint8_t []){0x00}, 1},
    {0x2b, (uint8_t []){0x00}, 1},
    {0x2c, (uint8_t []){0x00}, 1},
    {0x2d, (uint8_t []){0x00}, 1},
    {0x2e, (uint8_t []){0x00}, 1},
    {0x2f, (uint8_t []){0x00}, 1},
    {0x30, (uint8_t []){0x00}, 1},
    {0x31, (uint8_t []){0x00}, 1},
    {0x32, (uint8_t []){0x00}, 1},
    {0x33, (uint8_t []){0x00}, 1},
    {0x34, (uint8_t []){0x03}, 1},
    {0x35, (uint8_t []){0x00}, 1},
    {0x36, (uint8_t []){0x00}, 1},
    {0x37, (uint8_t []){0x00}, 1},
    {0x38, (uint8_t []){0x96}, 1},
    {0x39, (uint8_t []){0x00}, 1},
    {0x3a, (uint8_t []){0x00}, 1},
    {0x3b, (uint8_t []){0x00}, 1},
    {0x3c, (uint8_t []){0x00}, 1},
    {0x3d, (uint8_t []){0x00}, 1},
    {0x3e, (uint8_t []){0x00}, 1},
    {0x3f, (uint8_t []){0x00}, 1},
    {0x40, (uint8_t []){0x00}, 1},
    {0x41, (uint8_t []){0x00}, 1},
    {0x42, (uint8_t []){0x00}, 1},
    {0x43, (uint8_t []){0x00}, 1},
    {0x44, (uint8_t []){0x00}, 1},
    {0x50, (uint8_t []){0x00}, 1},
    {0x51, (uint8_t []){0x23}, 1},
    {0x52, (uint8_t []){0x45}, 1},
    {0x53, (uint8_t []){0x67}, 1},
    {0x54, (uint8_t []){0x89}, 1},
    {0x55, (uint8_t []){0xab}, 1},
    {0x56, (uint8_t []){0x01}, 1},
    {0x57, (uint8_t []){0x23}, 1},
    {0x58, (uint8_t []){0x45}, 1},
    {0x59, (uint8_t []){0x67}, 1},
    {0x5a, (uint8_t []){0x89}, 1},
    {0x5b, (uint8_t []){0xab}, 1},
    {0x5c, (uint8_t []){0xcd}, 1},
    {0x5d, (uint8_t []){0xef}, 1},
    {0x5e, (uint8_t []){0x00}, 1},
    {0x5f, (uint8_t []){0x08}, 1},
    {0x60, (uint8_t []){0x08}, 1},
    {0x61, (uint8_t []){0x06}, 1},
    {0x62, (uint8_t []){0x06}, 1},
    {0x63, (uint8_t []){0x01}, 1},
    {0x64, (uint8_t []){0x01}, 1},
    {0x65, (uint8_t []){0x00}, 1},
    {0x66, (uint8_t []){0x00}, 1},
    {0x67, (uint8_t []){0x02}, 1},
    {0x68, (uint8_t []){0x15}, 1},
    {0x69, (uint8_t []){0x15}, 1},
    {0x6a, (uint8_t []){0x14}, 1},
    {0x6b, (uint8_t []){0x14}, 1},
    {0x6c, (uint8_t []){0x0D}, 1},
    {0x6d, (uint8_t []){0x0D}, 1},
    {0x6e, (uint8_t []){0x0C}, 1},
    {0x6f, (uint8_t []){0x0C}, 1},
    {0x70, (uint8_t []){0x0F}, 1},
    {0x71, (uint8_t []){0x0F}, 1},
    {0x72, (uint8_t []){0x0E}, 1},
    {0x73, (uint8_t []){0x0E}, 1},
    {0x74, (uint8_t []){0x02}, 1},
    {0x75, (uint8_t []){0x08}, 1},
    {0x76, (uint8_t []){0x08}, 1},
    {0x77, (uint8_t []){0x06}, 1},
    {0x78, (uint8_t []){0x06}, 1},
    {0x79, (uint8_t []){0x01}, 1},
    {0x7a, (uint8_t []){0x01}, 1},
    {0x7b, (uint8_t []){0x00}, 1},
    {0x7c, (uint8_t []){0x00}, 1},
    {0x7d, (uint8_t []){0x02}, 1},
    {0x7e, (uint8_t []){0x15}, 1},
    {0x7f, (uint8_t []){0x15}, 1},
    {0x80, (uint8_t []){0x14}, 1},
    {0x81, (uint8_t []){0x14}, 1},
    {0x82, (uint8_t []){0x0D}, 1},
    {0x83, (uint8_t []){0x0D}, 1},
    {0x84, (uint8_t []){0x0C}, 1},
    {0x85, (uint8_t []){0x0C}, 1},
    {0x86, (uint8_t []){0x0F}, 1},
    {0x87, (uint8_t []){0x0F}, 1},
    {0x88, (uint8_t []){0x0E}, 1},
    {0x89, (uint8_t []){0x0E}, 1},
    {0x8A, (uint8_t []){0x02}, 1},
    {ILI9881C_CMD_CNDBKxSEL, (uint8_t []){ILI9881C_CMD_BKxSEL_BYTE0, ILI9881C_CMD_BKxSEL_BYTE1, ILI9881C_CMD_BKxSEL_BYTE2_PAGE4}, 3},
    {0x6E, (uint8_t []){0x2B}, 1},
    {0x6F, (uint8_t []){0x37}, 1},
    {0x3A, (uint8_t []){0x24}, 1},
    {0x8D, (uint8_t []){0x1A}, 1},
    {0x87, (uint8_t []){0xBA}, 1},
    {0xB2, (uint8_t []){0xD1}, 1},
    {0x88, (uint8_t []){0x0B}, 1},
    {0x38, (uint8_t []){0x01}, 1},
    {0x39, (uint8_t []){0x00}, 1},
    {0xB5, (uint8_t []){0x02}, 1},
    {0x31, (uint8_t []){0x25}, 1},
    {0x3B, (uint8_t []){0x98}, 1},
    {ILI9881C_CMD_CNDBKxSEL, (uint8_t []){ILI9881C_CMD_BKxSEL_BYTE0, ILI9881C_CMD_BKxSEL_BYTE1, ILI9881C_CMD_BKxSEL_BYTE2_PAGE1}, 3},
    {0x22, (uint8_t []){0x0A}, 1},
    {0x31, (uint8_t []){0x00}, 1},
    {0x53, (uint8_t []){0x3D}, 1},
    {0x55, (uint8_t []){0x3D}, 1},
    {0x50, (uint8_t []){0xB5}, 1},
    {0x51, (uint8_t []){0xAD}, 1},
    {0x60, (uint8_t []){0x06}, 1},
    {0x62, (uint8_t []){0x20}, 1},
    {0xB7, (uint8_t []){0x03}, 1},
    {0xA0, (uint8_t []){0x00}, 1},
    {0xA1, (uint8_t []){0x21}, 1},
    {0xA2, (uint8_t []){0x35}, 1},
    {0xA3, (uint8_t []){0x19}, 1},
    {0xA4, (uint8_t []){0x1E}, 1},
    {0xA5, (uint8_t []){0x33}, 1},
    {0xA6, (uint8_t []){0x27}, 1},
    {0xA7, (uint8_t []){0x26}, 1},
    {0xA8, (uint8_t []){0xAF}, 1},
    {0xA9, (uint8_t []){0x1B}, 1},
    {0xAA, (uint8_t []){0x27}, 1},
    {0xAB, (uint8_t []){0x8D}, 1},
    {0xAC, (uint8_t []){0x1A}, 1},
    {0xAD, (uint8_t []){0x1B}, 1},
    {0xAE, (uint8_t []){0x50}, 1},
    {0xAF, (uint8_t []){0x26}, 1},
    {0xB0, (uint8_t []){0x2B}, 1},
    {0xB1, (uint8_t []){0x54}, 1},
    {0xB2, (uint8_t []){0x5E}, 1},
    {0xB3, (uint8_t []){0x23}, 1},
    {0xC0, (uint8_t []){0x00}, 1},
    {0xC1, (uint8_t []){0x21}, 1},
    {0xC2, (uint8_t []){0x35}, 1},
    {0xC3, (uint8_t []){0x19}, 1},
    {0xC4, (uint8_t []){0x1E}, 1},
    {0xC5, (uint8_t []){0x33}, 1},
    {0xC6, (uint8_t []){0x27}, 1},
    {0xC7, (uint8_t []){0x26}, 1},
    {0xC8, (uint8_t []){0xAF}, 1},
    {0xC9, (uint8_t []){0x1B}, 1},
    {0xCA, (uint8_t []){0x27}, 1},
    {0xCB, (uint8_t []){0x8D}, 1},
    {0xCC, (uint8_t []){0x1A}, 1},
    {0xCD, (uint8_t []){0x1B}, 1},
    {0xCE, (uint8_t []){0x50}, 1},
    {0xCF, (uint8_t []){0x26}, 1},
    {0xD0, (uint8_t []){0x2B}, 1},
    {0xD1, (uint8_t []){0x54}, 1},
    {0xD2, (uint8_t []){0x5E}, 1},
    {0xD3, (uint8_t []){0x23}, 1},
    {ILI9881C_CMD_CNDBKxSEL, (uint8_t []){ILI9881C_CMD_BKxSEL_BYTE0, ILI9881C_CMD_BKxSEL_BYTE1, ILI9881C_CMD_BKxSEL_BYTE2_PAGE0}, 3},
    {0x35, (uint8_t []){0x00}, 1},
    {0x29, (uint8_t []){0x00}, 0},
};

/* 5.5寸MIPI 1080P初始化序列 */
static const mipi_lcd_init_cmd_t vendor_specific_init_code_default_1080p[] = {
    /* {cmd, { data }, data_size} */
    /* 1080 * 1920 */
    {UD_SETEXTC,        (uint8_t []){0xFF, 0x83, 0x99}, 3},
    {UD_SETOFFSET,      (uint8_t []){0x77}, 1},
    {UD_SETPOWER,       (uint8_t []){0x02, 0x04, 0x74, 0x94, 0x01, 0x32, 0x33, 0x11, 0x11, 0xAB, 0x4D, 0x56, 0x73, 0x02, 0x02}, 15},
    {UD_SETDISP,        (uint8_t []){0x00, 0x80, 0x80, 0xAE, 0x05, 0x07, 0x5A, 0x11, 0x00, 0x00, 0x10, 0x1E, 0x70, 0x03, 0xD4}, 15},
    {UD_SETCYC,         (uint8_t []){0x00, 0xFF, 0x02, 0xC0, 0x02, 0xC0, 0x00, 0x00, 0x08, 0x00, 0x04, 0x06, 0x00, 0x32, 0x04, 0x0A, 0x08, 0x21, 0x03, 0x01, 0x00, 0x0F, 0xB8, 0x8B, 0x02, 0xC0, 0x02, 0xC0, 0x00, 0x00, 0x08, 0x00, 0x04, 0x06, 0x00, 0x32, 0x04, 0x0A, 0x08, 0x01, 0x00, 0x0F, 0xB8, 0x01}, 44},
    {UD_SETMIPI,        (uint8_t []){0x61}, 1},
    {UD_SETGIP0,        (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x10, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x05, 0x05, 0x07, 0x00, 0x00, 0x00, 0x05, 0x40}, 33},
    {UD_SETGIP1,        (uint8_t []){0x18, 0x18, 0x19, 0x19, 0x18, 0x18, 0x21, 0x20, 0x01, 0x00, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x2F, 0x2F, 0x30, 0x30, 0x31, 0x31, 0x18, 0x18, 0x18, 0x18}, 32},
    {UD_SETGIP2,        (uint8_t []){0x18, 0x18, 0x19, 0x19, 0x40, 0x40, 0x20, 0x21, 0x06, 0x07, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x2F, 0x2F, 0x30, 0x30, 0x31, 0x31, 0x40, 0x40, 0x40, 0x40}, 32},
    {UD_SETGIP3,        (uint8_t []){0xA2, 0xAA, 0x02, 0xA0, 0xA2, 0xA8, 0x02, 0xA0, 0xB0, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00}, 16},
    {UD_SETREGBANK,     (uint8_t []){0x01}, 1},
    {UD_SETGIP3,        (uint8_t []){0xB0, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00, 0xE2, 0xAA, 0x03, 0xF0, 0xE2, 0xAA, 0x03, 0xF0}, 16},
    {UD_SETREGBANK,     (uint8_t []){0x02}, 1},
    {UD_SETGIP3,        (uint8_t []){0xE2, 0xAA, 0x03, 0xF0, 0xE2, 0xAA, 0x03, 0xF0}, 8},
    {UD_SETREGBANK,     (uint8_t []){0x00}, 1},
    {UD_SETVCOM,        (uint8_t []){0x8D, 0x8D}, 2},
    {UD_SETGAMMA,       (uint8_t []){0x00, 0x0E, 0x19, 0x13, 0x2E, 0x39, 0x48, 0x44, 0x4D, 0x57, 0x5F, 0x66, 0x6C, 0x76, 0x7F, 0x85, 0x8A, 0x95, 0x9A, 0xA4, 0x9B, 0xAB, 0xB0, 0x5C, 0x58, 0x64, 0x77, 0x00, 0x0E, 0x19, 0x13, 0x2E, 0x39, 0x48, 0x44, 0x4D, 0x57, 0x5F, 0x66, 0x6C, 0x76, 0x7F, 0x85, 0x8A, 0x95, 0x9A, 0xA4, 0x9B, 0xAB, 0xB0, 0x5C, 0x58, 0x64, 0x77}, 54},
};

/* 5.5寸MIPI 720P初始化序列 */
static const mipi_lcd_init_cmd_t vendor_specific_init_code_default_720p[] = {
    /* {cmd, { data }, data_size} */
    /* 720 * 1280 */
    {UD_SETADDRESSMODE, (uint8_t []){0x01}, 1},
    {UD_SETEXTC,        (uint8_t []){0xFF, 0x83, 0x94}, 3},
    {UD_SETMIPI,        (uint8_t []){0x61, 0x03, 0x68, 0x6B, 0xB2, 0xC0}, 6},
    {UD_SETPOWER,       (uint8_t []){0x48, 0x12, 0x72, 0x09, 0x32, 0x54, 0x71, 0x71, 0x57, 0x47}, 10},
    {UD_SETDISP,        (uint8_t []){0x00, 0x80, 0x64, 0x0C, 0x0D, 0x2F}, 6},
    {UD_SETCYC,         (uint8_t []){0x73, 0x74, 0x73, 0x74, 0x73, 0x74, 0x01, 0x0C, 0x86, 0x75, 0x00, 0x3F, 0x73, 0x74, 0x73, 0x74, 0x73, 0x74, 0x01, 0x0C, 0x86}, 21},
    {UD_SETGIP0,        (uint8_t []){0x00, 0x00, 0x07, 0x07, 0x40, 0x07, 0x0C, 0x00, 0x08, 0x10, 0x08, 0x00, 0x08, 0x54, 0x15, 0x0A, 0x05, 0x0A, 0x02, 0x15, 0x06, 0x05, 0x06, 0x47, 0x44, 0x0A, 0x0A, 0x4B, 0x10, 0x07, 0x07, 0x0C, 0x40}, 33},
    {UD_SETGIP1,        (uint8_t []){0x1C, 0x1C, 0x1D, 0x1D, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x24, 0x25, 0x18, 0x18, 0x26, 0x27, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x20, 0x21, 0x18, 0x18, 0x18, 0x18}, 44},
    {UD_SETGIP2,        (uint8_t []){0x1C, 0x1C, 0x1D, 0x1D, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x0B, 0x0A, 0x09, 0x08, 0x21, 0x20, 0x18, 0x18, 0x27, 0x26, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x25, 0x24, 0x18, 0x18, 0x18, 0x18}, 44},
    {UD_SETVCOM,        (uint8_t []){0x6E, 0x6E}, 2},
    {UD_SETGAMMA,       (uint8_t []){0x00, 0x0A, 0x15, 0x1B, 0x1E, 0x21, 0x24, 0x22, 0x47, 0x56, 0x65, 0x66, 0x6E, 0x82, 0x88, 0x8B, 0x9A, 0x9D, 0x98, 0xA8, 0xB9, 0x5D, 0x5C, 0x61, 0x66, 0x6A, 0x6F, 0x7F, 0x7F, 0x00, 0x0A, 0x15, 0x1B, 0x1E, 0x21, 0x24, 0x22, 0x47, 0x56, 0x65, 0x65, 0x6E, 0x81, 0x87, 0x8B, 0x98, 0x9D, 0x99, 0xA8, 0xBA, 0x5D, 0x5D, 0x62, 0x67, 0x6B, 0x72, 0x7F, 0x7F}, 58},
    {0xC0,              (uint8_t []){0x1F, 0x31}, 2},
    {UD_SETPANEL,       (uint8_t []){0x03}, 1},
    {0xD4,              (uint8_t []){0x02}, 1},
    {UD_SETREGBANK,     (uint8_t []){0x02}, 1},
    {UD_SETGIP3,        (uint8_t []){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 12},
    {UD_SETREGBANK,     (uint8_t []){0x00}, 1},
    {UD_SETREGBANK,     (uint8_t []){0x01}, 1},
    {UD_SETPOWER,       (uint8_t []){0x00}, 1},
    {UD_SETREGBANK,     (uint8_t []){0x00}, 1},
    {0xBF,              (uint8_t []){0x40, 0x81, 0x50, 0x00, 0x1A, 0xFC, 0x01}, 7},
    {0xC6,              (uint8_t []){0xED}, 1},
};

/**
 * @brief       配置mipi phy电压
 * @param       无
 * @retval      无
 */
void mipi_dev_bsp_enable_dsi_phy_power(void)
{
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
#ifdef MIPI_DSI_PHY_PWR_LDO_CHAN
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id    = MIPI_DSI_PHY_PWR_LDO_CHAN,        /* 选择内存LDO */
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,  /* 输出标准电压提供VDD_MIPI_DPHY */
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(mipi_lcd_tag, "MIPI DSI PHY Powered on");
#endif
}

/**
 * @brief       删除LCD面板
 * @param       panel:LCD接口句柄
 * @retval      ESP_OK:删除成功
 */
static esp_err_t mipi_lcd_paneldel(esp_lcd_panel_t *panel)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    if (mipi_lcd->reset_gpio_num >= 0)
    {
        gpio_reset_pin(mipi_lcd->reset_gpio_num);
    }

    free(mipi_lcd);

    return ESP_OK;
}

/**
 * @brief       复位LCD面板
 * @param       panel:LCD接口句柄
 * @retval      ESP_OK:复位成功
 */
static esp_err_t mipi_lcd_panelreset(esp_lcd_panel_t *panel)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    esp_lcd_panel_io_handle_t io = mipi_lcd->io;

    /* 如果有GPIO控制LCD的复位管脚 */
    if (mipi_lcd->reset_gpio_num >= 0)      /* 硬件复位 */
    {
        gpio_set_level(mipi_lcd->reset_gpio_num, mipi_lcd->reset_level);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(mipi_lcd->reset_gpio_num, !mipi_lcd->reset_level);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    else                                    /* 软件复位 */
    { 
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), mipi_lcd_tag, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

/**
 * @brief       初始化LCD面板
 * @param       panel:LCD接口句柄
 * @retval      ESP_OK:复位成功
 */
static esp_err_t mipi_lcd_panelinit(esp_lcd_panel_t *panel)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    esp_lcd_panel_io_handle_t io = mipi_lcd->io;
    
    const mipi_lcd_init_cmd_t *init_cmds = {0};
    uint16_t init_cmds_size = 0;
    bool mirror_x = true;
    bool mirror_y = false;

    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: entering controller initialisation");

    if (mipidev.id == 0x8399)       /* 5寸,720P */
    {
        init_cmds = vendor_specific_init_code_default_1080p;
        init_cmds_size = sizeof(vendor_specific_init_code_default_1080p) / sizeof(mipi_lcd_init_cmd_t);
    }
    else if (mipidev.id == 0x8394)  /* 5寸,1080p */
    {
        init_cmds = vendor_specific_init_code_default_720p;
        init_cmds_size = sizeof(vendor_specific_init_code_default_720p) / sizeof(mipi_lcd_init_cmd_t);
    }
    else if (mipidev.id == 0x9881)  /* 10.1寸,800p */
    {
        init_cmds = vendor_specific_init_code_default_800p;
        init_cmds_size = sizeof(vendor_specific_init_code_default_800p) / sizeof(mipi_lcd_init_cmd_t);

        /* 返回 命令页 1 */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ILI9881C_CMD_CNDBKxSEL, (uint8_t[]) {
            ILI9881C_CMD_BKxSEL_BYTE0, ILI9881C_CMD_BKxSEL_BYTE1, ILI9881C_CMD_BKxSEL_BYTE2_PAGE1
        }, 3), mipi_lcd_tag, "send command failed");
        /* 设置2 lane */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ILI9881C_PAD_CONTROL, (uint8_t[]) {
            ILI9881C_DSI_2_LANE,
        }, 1), mipi_lcd_tag, "send command failed");

        /* 返回 命令页 0 */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ILI9881C_CMD_CNDBKxSEL, (uint8_t[]) {
            ILI9881C_CMD_BKxSEL_BYTE0, ILI9881C_CMD_BKxSEL_BYTE1, ILI9881C_CMD_BKxSEL_BYTE2_PAGE0
        }, 3), mipi_lcd_tag, "send command failed");
        mirror_x = false;
    }

#if MIPI_PANEL_ROTATE_180
    /* A 180-degree rotation is a mirror on both physical scan axes.  Apply
     * this after the panel-profile defaults so every supported controller
     * uses the same mounting orientation. */
    mirror_x = true;
    mirror_y = true;
#endif

    /* 发送初始化序列 */
    for (int i = 0; i < init_cmds_size; i++)
    {
        ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: init command %d/%d: 0x%02X",
                 i + 1, init_cmds_size, init_cmds[i].cmd);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), mipi_lcd_tag, "send command failed");
    }

    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: vendor command sequence complete");

    /* 退出睡眠 */
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: sending Sleep Out (0x11)");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), mipi_lcd_tag, "io tx param failed");
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: Sleep Out sent");
    /* HX8399 needs at least 120 ms after Sleep Out before format/display commands. */
    esp_rom_delay_us(120000);
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: Sleep Out delay complete");
    
    /* 根据MIPI屏放置位置调整显示方向(也可调用mipi_lcd_panelmirror函数设置) */
    if (mirror_x)
    {
        mipi_lcd->madctl_val |= HX_F_SS_PANEL;      /* 扫描方向水平翻转 */
    }
    else
    {
        mipi_lcd->madctl_val &= ~HX_F_SS_PANEL;     /* 扫描方向水平不翻转 */
    }

    if (mirror_y)
    {
        mipi_lcd->madctl_val |= HX_F_GS_PANEL;      /* 扫描方向垂直翻转 */
    }
    else
    {
        mipi_lcd->madctl_val &= ~HX_F_GS_PANEL;     /* 扫描方向垂直不翻转 */
    }
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: sending MADCTL (0x36)");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[])
    {
        mipi_lcd->madctl_val,
    }, 1), mipi_lcd_tag, "send command failed");    /* 配置MIPILCD的显示 */

    /* The P4 DSI DBI command FIFO needs time to consume the preceding short
     * packet before another short packet is queued. */
    esp_rom_delay_us(10000);
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: MADCTL command FIFO drain complete");

    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: sending COLMOD RGB565 (0x3A)");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[])
    {
        mipi_lcd->colmod_val,
    }, 1), mipi_lcd_tag, "send command failed");    /* 配置像素格式 */
    /* Keep this short delay independent of the FreeRTOS tick while DSI is active. */
    esp_rom_delay_us(120000);
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: controller initialisation complete");

    return ESP_OK;
}

/**
 * @brief       反转显示
 * @param       panel             :LCD接口句柄
 * @param       invert_color_data :true反转显示;false正常显示
 * @retval      ESP_OK:复位成功
 */
static esp_err_t mipi_lcd_panelinvert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    esp_lcd_panel_io_handle_t io = mipi_lcd->io;
    int command = 0;

    if (invert_color_data)
    {
        command = LCD_CMD_INVON;    /* 反转显示打开 */
    }
    else
    {
        command = LCD_CMD_INVOFF;   /* 反转显示关闭 */
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), mipi_lcd_tag, "send command failed");
    
    return ESP_OK;
}

/**
 * @brief       在特定轴上镜像LCD面板
 * @param       panel       :LCD接口句柄
 * @param       mirror_x    :true对x轴进行镜像,false不操作镜像
 * @param       mirror_y    :true对y轴进行镜像,false不进行镜像
 * @retval      ESP_OK:复位成功
 */
static esp_err_t mipi_lcd_panelmirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    esp_lcd_panel_io_handle_t io = mipi_lcd->io;

    if (mirror_x)
    {
        mipi_lcd->madctl_val |= HX_F_SS_PANEL;      /* 扫描方向水平翻转(镜像x) */
    }
    else
    {
        mipi_lcd->madctl_val &= ~HX_F_SS_PANEL;     /* 扫描方向水平不翻转 */
    }

    if (mirror_y)
    {
        mipi_lcd->madctl_val |= HX_F_GS_PANEL;      /* 扫描方向垂直翻转(镜像y) */
    }
    else
    {
        mipi_lcd->madctl_val &= ~HX_F_GS_PANEL;     /* 扫描方向垂直不翻转 */
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[])
    {
        mipi_lcd->madctl_val,
    }, 1), mipi_lcd_tag, "send command failed");    /* 配置MIPILCD的显示 */

    return ESP_OK;
}

/**
 * @brief       交换x和y轴
 * @param       panel       :LCD接口句柄
 * @param       swap_axes   :是否对x和y轴进行方向互换
 * @retval      ESP_ERR_NOT_SUPPORTED:适配的MIPILCD不支持
 */
static esp_err_t mipi_lcd_panelswap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    ESP_LOGW(mipi_lcd_tag, "Swap XY is not supported in HX8394/8399-F driver. Please use SW rotation.");
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief       设置x轴和y轴的偏移
 * @param       panel :LCD接口句柄
 * @param       x_gap :X方向偏移
 * @param       y_gap :Y方向偏移
 * @retval      ESP_OK:设置成功
 */
static esp_err_t mipi_lcd_panelset_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    mipi_lcd->x_gap = x_gap;
    mipi_lcd->y_gap = y_gap;
    return ESP_OK;
}

/**
 * @brief       打开或者关闭显示
 * @param       panel :LCD接口句柄
 * @param       on_off:true打开显示,flase关闭显示
 * @retval      ESP_OK:设置成功
 */
static esp_err_t mipi_lcd_paneldisp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    esp_lcd_panel_io_handle_t io = mipi_lcd->io;
    int command = 0;

    if (on_off)
    {
        command = LCD_CMD_DISPON;   /* 打开显示命令 */
    }
    else
    {
        command = LCD_CMD_DISPOFF;  /* 关闭显示命令 */
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), mipi_lcd_tag, "send command failed");
    
    return ESP_OK;
}

/**
 * @brief       进入或退出睡眠模式
 * @param       sleep :true进入睡眠模式,flase:退出睡眠模式
 * @retval      ESP_OK:设置成功
 */
static esp_err_t mipi_lcd_panelsleep(esp_lcd_panel_t *panel, bool sleep)
{
    mipi_panel_t *mipi_lcd = __containerof(panel, mipi_panel_t, base);
    esp_lcd_panel_io_handle_t io = mipi_lcd->io;
    int command = 0;

    if (sleep)
    {
        command = LCD_CMD_SLPIN;    /* 进入睡眠模式命令 */
    }
    else
    {
        command = LCD_CMD_SLPOUT;   /* 退出睡眠模式命令 */
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), mipi_lcd_tag, "io tx param failed");
    
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

/**
 * @brief       新建mipilcd句柄，并配置函数
 * @param       io              :MIPI IO句柄
 * @param       panel_dev_config:MIPI 设备配置结构体
 * @param       ret_panel       :MIPI屏句柄
 * @retval      esp_err_t 返回值
 */
esp_err_t mipi_lcd_new_panel(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, mipi_lcd_tag, "invalid argument");

    mipi_panel_t *mipi_lcd = (mipi_panel_t *)calloc(1, sizeof(mipi_panel_t));
    ESP_RETURN_ON_FALSE(mipi_lcd, ESP_ERR_NO_MEM, mipi_lcd_tag, "no mem for mipi_lcd panel");

    if (panel_dev_config->reset_gpio_num >= 0)      /* 配置LCD复位引脚 */
    {
        gpio_config_t io_conf = {
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, mipi_lcd_tag, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order)        /* 颜色顺序RGB/BGR */
    {
        case LCD_RGB_ELEMENT_ORDER_RGB:
            mipi_lcd->madctl_val = 0;
            break;
        case LCD_RGB_ELEMENT_ORDER_BGR:
            mipi_lcd->madctl_val |= LCD_CMD_BGR_BIT;
            break;
        default:
            ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, mipi_lcd_tag, "unsupported rgb element order");
            break;
    }

    switch (panel_dev_config->bits_per_pixel)       /* 像素格式 */
    {
        case 16:    /* RGB565 */
            mipi_lcd->colmod_val = 0x55;
            break;
        case 18:    /* RGB666 */
            mipi_lcd->colmod_val = 0x66;
            break;
        case 24:    /* RGB888 */
            mipi_lcd->colmod_val = 0x77;
            break;
        default:
            ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, mipi_lcd_tag, "unsupported pixel width");
            break;
    }

    mipi_lcd->io                = io;
    mipi_lcd->reset_gpio_num    = panel_dev_config->reset_gpio_num;         /* LCD的复位引脚 */
    mipi_lcd->reset_level       = panel_dev_config->flags.reset_active_high;/* 复位引脚的有效电平 */
    mipi_lcd->base.reset        = mipi_lcd_panelreset;                      /* 复位LCD面板 */
    mipi_lcd->base.init         = mipi_lcd_panelinit;                       /* 初始化LCD面板 */
    mipi_lcd->base.del          = mipi_lcd_paneldel;                        /* 删除LCD面板 */
    mipi_lcd->base.mirror       = mipi_lcd_panelmirror;                     /* 在特定轴上镜像LCD面板 */
    mipi_lcd->base.swap_xy      = mipi_lcd_panelswap_xy;                    /* 交换x轴和y轴 */
    mipi_lcd->base.set_gap      = mipi_lcd_panelset_gap;                    /* 设置x轴和y轴的偏移 */
    mipi_lcd->base.invert_color = mipi_lcd_panelinvert_color;               /* 反转显示 */
    mipi_lcd->base.disp_on_off  = mipi_lcd_paneldisp_on_off;                /* 打开或关闭显示 */
    mipi_lcd->base.disp_sleep   = mipi_lcd_panelsleep;                      /* 进入或退出睡眠模式 */

    *ret_panel = &mipi_lcd->base;

    return ESP_OK;

err:
    if (mipi_lcd)
    {
        mipi_lcd_paneldel(&mipi_lcd->base);
    }

    return ret;
}

/**
 * @brief       mipi_lcd初始化
 * @param       无
 * @retval      LCD控制句柄
 */
esp_lcd_panel_handle_t mipi_lcd_init(void)
{
    mipi_dev_bsp_enable_dsi_phy_power();                    /* 配置DSI PHY电源：2.5V */
    
    /* 创建DSI总线 */
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id             = 0,                            /* 总线ID */
        .num_data_lanes     = MIPI_DSI_LANE_NUM,            /* 2路数据信号 */
        /* 设为 0 交给 ESP-IDF 按芯片版本选择。
         * 旧宏 MIPI_DSI_PHY_CLK_SRC_DEFAULT 固定为 PLL_F20M，仅适用于
         * ESP32-P4 rev < 3.0；本工程 CONFIG_ESP32P4_REV_MIN_300=y 时应
         * 使用 rev >= 3.0 的 XTAL 参考时钟，否则在创建 DSI 总线时 panic。 */
        .phy_clk_src        = 0,
        .lane_bit_rate_mbps = MIPI_DSI_LANE_BITRATE_MBPS,   /* 数据通道比特率(Mbps) */
    };
    /* 不用 ESP_ERROR_CHECK：创建失败时先把实际错误码输出，避免 USB CDC 随 panic
     * 一起断开而无法诊断。调用者会识别 NULL 句柄并保持背光测试运行。 */
    esp_err_t dsi_ret = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "esp_lcd_new_dsi_bus failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }

    /* 配置DSI总线的DBI接口 */
    esp_lcd_panel_io_handle_t mipi_dbi_io;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,                               /* 虚拟通道(只有一个LCD连接,设置0即可) */
        .lcd_cmd_bits    = 8,                               /* 根据MIPI LCD驱动IC规格设置 命令位宽度 */
        .lcd_param_bits  = 8,                               /* 根据MIPI LCD驱动IC规格设置 参数位宽度 */
    };
    ESP_LOGI(mipi_lcd_tag, "DSI diagnostic: creating DBI command interface");
    dsi_ret = esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "esp_lcd_new_panel_io_dbi failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }
    ESP_LOGI(mipi_lcd_tag, "DSI diagnostic: DBI command interface ready");

    /* 创建LCD控制器驱动 */
    esp_lcd_panel_handle_t mipi_lcd_ctrl_panel;             /* MIPI控制句柄 */
    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .bits_per_pixel = 16,                               /* MIPILCD的像素位宽度(RGB565即16位,RGB888即24位) */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,         /* 像素数据的RGB元素顺序,根据实际色彩情况选择BGR或RGB */
        .reset_gpio_num = lcddev.ctrl.lcd_rst,              /* MIPILCD屏的复位引脚 */
    };
    ESP_LOGI(mipi_lcd_tag, "DSI diagnostic: creating LCD controller panel");
    dsi_ret = mipi_lcd_new_panel(mipi_dbi_io, &lcd_dev_config, &mipi_lcd_ctrl_panel);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "mipi_lcd_new_panel failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }
    ESP_LOGI(mipi_lcd_tag, "DSI diagnostic: LCD controller panel ready");

    ESP_LOGI(mipi_lcd_tag, "DSI diagnostic: resetting LCD controller");
    dsi_ret = esp_lcd_panel_reset(mipi_lcd_ctrl_panel);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "esp_lcd_panel_reset failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }
    ESP_LOGI(mipi_lcd_tag, "DSI diagnostic: LCD controller reset complete");

#if MIPI_DSI_ID_READ_DIAGNOSTIC
    ESP_LOGI(mipi_lcd_tag, "DSI ID diagnostic: reading HX8399 0xDA/0xDB via D0 LP/BTA");
    if (!mipi_lcd_diagnose_id_read(mipi_dbi_io)) {
        ESP_LOGE(mipi_lcd_tag, "DSI ID diagnostic failed; video initialisation is intentionally skipped");
        return NULL;
    }
#endif

    /*
     * ATK-MD0550M-10801920 is the 5.5-inch 1080x1920 HX8399 profile.
     * This panel does not answer the generic DCS 0xDA/0xDB ID reads used by
     * the original multi-panel BSP, leaving the DSI RX transaction pending
     * forever. Select its known profile directly; no DSI read is required.
     */
    mipidev.id = 0x8399;
    ESP_LOGI(mipi_lcd_tag, "LCD profile fixed: ATK-MD0550M-10801920 (HX8399, 1080x1920, 2-lane)");

    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: starting controller initialisation");
    dsi_ret = esp_lcd_panel_init(mipi_lcd_ctrl_panel);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "HX8399 controller initialisation failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: enabling display");
    dsi_ret = esp_lcd_panel_disp_on_off(mipi_lcd_ctrl_panel, true);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "HX8399 display-on command failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: display-on command complete");

    if (mipidev.id == 0x8394)                                   /* 5.5寸720P屏幕 */
    {
        mipidev.pwidth   = 720;                                 /* 面板宽度,单位:像素 */
        mipidev.pheight  = 1280;                                /* 面板高度,单位:像素 */
        mipidev.hbp      = 52;                                  /* 水平后廊 */
        mipidev.hfp      = 48;                                  /* 水平前廊 */
        mipidev.hsw      = 8;                                   /* 水平同步宽度 */
        mipidev.vbp      = 15;                                  /* 垂直后廊 */
        mipidev.vfp      = 16;                                  /* 垂直前廊 */
        mipidev.vsw      = 5;                                   /* 垂直同步宽度 */
        mipidev.pclk_mhz = 60;                                  /* 设置像素时钟 60Mhz */
        mipidev.dir      = 0;                                   /* 只能竖屏 */
    }
    else if (mipidev.id == 0x8399)                              /* 5.5寸1080P屏幕 */
    {
        mipidev.pwidth   = 1080;                                /* 面板宽度,单位:像素 */
        mipidev.pheight  = 1920;                                /* 面板高度,单位:像素 */
        mipidev.hbp      = 22;                                  /* 水平后廊 */
        mipidev.hfp      = 22;                                  /* 水平前廊 */
        mipidev.hsw      = 20;                                  /* 水平同步宽度 */
        mipidev.vbp      = 9;                                   /* 垂直后廊 */
        mipidev.vfp      = 7;                                   /* 垂直前廊 */
        mipidev.vsw      = 7;                                   /* 垂直同步宽度 */
        mipidev.pclk_mhz = 60;                                  /* 设置像素时钟 60Mhz */
        mipidev.dir      = 0;                                   /* 只能竖屏 */
    }
    else if (mipidev.id == 0x9881)                              /* 10.1寸800P屏幕 */
    {
        mipidev.pwidth   = 800;                                 /* 面板宽度,单位:像素 */
        mipidev.pheight  = 1280;                                /* 面板高度,单位:像素 */
        mipidev.hbp      = 24;                                  /* 水平后廊 */
        mipidev.hfp      = 15;                                  /* 水平前廊 */
        mipidev.hsw      = 24;                                  /* 水平同步宽度 */
        mipidev.vbp      = 9;                                   /* 垂直后廊 */
        mipidev.vfp      = 7;                                   /* 垂直前廊 */
        mipidev.vsw      = 2;                                   /* 垂直同步宽度 */
        mipidev.pclk_mhz = 60;                                  /* 设置像素时钟 60Mhz */
        mipidev.dir      = 0;                                   /* 只能竖屏 */
    }

    lcddev.id     = mipidev.id;                                 /* LCD_ID */
    lcddev.width  = mipidev.pwidth;                             /* 宽度 */
    lcddev.height = mipidev.pheight;                            /* 高度 */
    lcddev.lcd_dbi_io = mipi_dbi_io;                            /* LCD IO控制句柄 */
    esp_lcd_panel_handle_t mipi_dpi_panel;                      /* MIPILCD控制句柄 */
    esp_lcd_dpi_panel_config_t dpi_config = {                   /* DSI数据配置 */
        .virtual_channel    = 0,                                /* 虚拟通道 */
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,     /* 时钟源 */
        .dpi_clock_freq_mhz = mipidev.pclk_mhz,                 /* 像素时钟频率 */
        .in_color_format    = LCD_COLOR_FMT_RGB565,           /* 颜色格式 */
        .num_fbs            = 2,                                /* 帧缓冲区数量 */
        .video_timing       = {                                 /* LCD面板特定时序参数 */
            .h_size            = mipidev.pwidth,                /* 水平分辨率,即一行中的像素数 */
            .v_size            = mipidev.pheight,               /* 垂直分辨率,即帧中的行数 */
            .hsync_back_porch  = mipidev.hbp,                   /* 水平后廊,hsync和行活动数据开始之间的PCLK数 */
            .hsync_pulse_width = mipidev.hsw,                   /* 水平同步宽度,单位:PCLK周期*/
            .hsync_front_porch = mipidev.hfp,                   /* 水平前廊,活动数据结束和下一个hsync之间的PCLK数 */
            .vsync_back_porch  = mipidev.vbp,                   /* 垂直后廊,vsync和帧开始之间的无效行数 */
            .vsync_pulse_width = mipidev.vsw,                   /* 垂直同步宽度,单位:行数 */
            .vsync_front_porch = mipidev.vfp,                   /* 垂直前廊,帧结束和下一个vsync之间的无效行数 */
        },
    };
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: creating 1080x1920 DPI video panel");
    dsi_ret = esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &mipi_dpi_panel);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "create DPI video panel failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: initialising DPI video panel");
    dsi_ret = esp_lcd_panel_init(mipi_dpi_panel);
    if (dsi_ret != ESP_OK) {
        ESP_LOGE(mipi_lcd_tag, "initialise DPI video panel failed: %s (%d)",
                 esp_err_to_name(dsi_ret), dsi_ret);
        return NULL;
    }
    ESP_LOGI(mipi_lcd_tag, "HX8399 diagnostic: DPI video panel running");

    return mipi_dpi_panel;
}
