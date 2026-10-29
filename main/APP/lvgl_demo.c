/**
 ****************************************************************************************************
 * @file        lvgl_demo.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       LVGL V8移植 实验
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

#include "lvgl_demo.h"
#include "lcd.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui/ui.h"
#include "freertos/semphr.h"


/*互斥锁*/
static SemaphoreHandle_t g_lvgl_mutex = NULL;

bool lvgl_port_lock(TickType_t timeout)
{
    return g_lvgl_mutex &&
           xSemaphoreTake(g_lvgl_mutex, timeout) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (g_lvgl_mutex) {
        xSemaphoreGive(g_lvgl_mutex);
    }
}


/* MIPI竖屏旋转和PSRAM旋转缓冲区 */
static bool g_need_rotate = false;
static uint16_t *g_rot_buf = NULL;

/* Rotate the completed logical landscape UI by 180 degrees in software.  This
 * avoids altering the HX8399 video-panel scan order (MADCTL), which can cause
 * corrupted output when both controller scan axes are flipped. */
#define LVGL_DISPLAY_ROTATE_180 1

/* Forward declarations */
void increase_lvgl_tick(void *arg);

/**
 * @brief       LVGL定时器处理任务(独立任务,避免阻塞app_main)
 * @param       arg : 未使用
 * @retval      无
 */
static void lvgl_timer_task(void *arg)
{
    ESP_LOGI("lvgl_demo", "LVGL timer task started");
    esp_task_wdt_add(NULL);

    while (1)
    {
        if (lvgl_port_lock(portMAX_DELAY)) {
    lv_timer_handler();
    lvgl_port_unlock();
}
        esp_task_wdt_reset();           /* 喂狗 */
        vTaskDelay(pdMS_TO_TICKS(10));  /* 延时10毫秒 */
    }
}

/**
 * @brief       lvgl_demo入口函数
 * @param       无
 * @retval      无
 */
static void lvgl_init_common(bool enable_app_ui)
{
    g_lvgl_mutex = xSemaphoreCreateMutex();
    assert(g_lvgl_mutex);
    lv_init();              /* 初始化LVGL图形to_rgb565.c:256库 */
    if (lv_port_disp_init() == NULL) {
        /* A DSI diagnostic failure must not continue into UI allocation with
         * a 0x0 display. Keep USB CDC alive so the actual LCD error remains
         * visible instead of turning into an unrelated reset. */
        ESP_LOGE("lvgl_demo", "Display initialisation failed; LVGL/UI deliberately not started");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    /* 为LVGL提供时基单元 */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,    /* 设置定时器回调 */
        .name = "lvgl_tick"                 /* 定时器名称 */
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));     /* 创建定时器 */
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1 * 1000));           /* 启动定时器 */

    if (enable_app_ui) {
        /* Build and submit the boot UI while BL is off.  Showing the panel
         * earlier exposes its reset/default white frame while the UI and the
         * software rotation buffer are still being prepared. */
        ui_init();
        lv_refr_now(NULL);
        if (!lcd_wait_for_next_refresh(500)) {
            ESP_LOGW("lvgl_demo", "Timed out waiting for the first UI VSYNC; enabling backlight anyway");
        }
        LCD_BACKLIGHT_ON();
        ESP_LOGI("lvgl_demo", "First UI frame ready; backlight enabled");
    }

    /* 创建独立任务运行 lv_timer_handler 循环, 避免阻塞 app_main
     * 注意: LVGL软件渲染需要较大栈空间, 8192为安全值
     */
    xTaskCreatePinnedToCore(
        lvgl_timer_task,
        "lvgl_timer",
        8192,
        NULL,
        2,              /* 较低优先级, 让CAN和UI更新任务优先 */
        NULL,
        tskNO_AFFINITY
    );
}

void lvgl_demo(void)
{
    lvgl_init_common(true);
}

void lvgl_display_only_init(void)
{
    /* CAN screen test: retain the complete MIPI/LVGL display path but avoid
     * touch, I2C and the application UI entirely. */
    lvgl_init_common(false);
}

/**
 * @brief       初始化并注册显示设备 (LVGL v8)
 * @param       无
 * @retval      lvgl显示设备指针
 */
lv_disp_t *lv_port_disp_init(void)
{
    void *lvgl_buf[2];                      /* LVGL绘图缓冲区(32-bit ARGB) */

    /* 初始化显示设备LCD */
    lcd_init();                             /* LCD初始化 */
    if (lcddev.lcd_panel_handle == NULL) {
        ESP_LOGE("lvgl_demo", "LCD panel handle is unavailable");
        return NULL;
    }

    /* 判断是否需要旋转: MIPI竖屏用逻辑横屏 + 手动旋转 + PSRAM缓冲 */
    g_need_rotate = (lcddev.height > lcddev.width);
    int32_t hor_res, ver_res;
    size_t buf_size;
    size_t pixel_count;

    if (g_need_rotate) {
        hor_res = lcddev.height;    /* 逻辑横屏 1920 */
        ver_res = lcddev.width;     /* 逻辑横屏 1080 */
    } else {
        hor_res = lcddev.width;
        ver_res = lcddev.height;
    }

    /* Keep one full LVGL render buffer for a fast first page, but submit only
     * dirty rectangles afterwards.  LVGL supplies compact pixels for each
     * dirty area in normal (non-full-refresh) mode. */
    pixel_count = hor_res * ver_res;        /* 全屏像素数 */
    buf_size = pixel_count * sizeof(uint32_t);
    lvgl_buf[0] = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lvgl_buf[1] = NULL;                     /* 单缓冲, 不分配第二个 */
    ESP_LOGI("lvgl_demo", "LVGL draw buf: %p, size=%dKB (%dx%d)",
             lvgl_buf[0], buf_size / 1024, hor_res, ver_res);

    if (!lvgl_buf[0]) {
        ESP_LOGE("lvgl_demo", "Failed to allocate LVGL draw buffer!");
        return NULL;
    }

    /* 旋转缓冲区: 全屏 RGB565 */
    if (!g_rot_buf) {
        g_rot_buf = (uint16_t *)heap_caps_malloc(lcddev.width * lcddev.height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        ESP_LOGI("lvgl_demo", "Rotate buffer: %p", g_rot_buf);
    }
    if (!g_rot_buf) {
        ESP_LOGE("lvgl_demo", "Failed to allocate rotate buffer!");
        return NULL;
    }

    /* 创建LVGL显示设备 (LVGL v8) */
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;

    lv_disp_draw_buf_init(&draw_buf, lvgl_buf[0], lvgl_buf[1], pixel_count);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = hor_res;
    disp_drv.ver_res = ver_res;
    disp_drv.flush_cb = lvgl_disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = lcddev.lcd_panel_handle;
    disp_drv.full_refresh = 0;

    return lv_disp_drv_register(&disp_drv);
}

/**
* @brief        将内部缓冲区的内容刷新到显示屏上的特定区域 (LVGL v8)
* @param        disp_drv : 显示设备驱动
* @param        area : 要刷新的区域
* @param        color_p : 像素数据
* @retval       无
*/
void lvgl_disp_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)disp_drv->user_data;
    uint32_t *px32 = (uint32_t *)color_p;    /* LVGL 32-bit ARGB8888 */

    if (g_need_rotate) {
        const lv_coord_t logical_w = lv_area_get_width(area);
        const lv_coord_t logical_h = lv_area_get_height(area);

        /* The DPI driver's external source must be a compact rectangle.
         * Previously we wrote a dirty region into a full-screen rotated
         * canvas, then copied all 1080x1920 pixels for every small label
         * update.  Pack just this physical dirty rectangle at g_rot_buf[0].
         * The display rotation remains unchanged:
         * logical(lx, ly) -> physical(phys_w - 1 - ly, lx). */
#if LVGL_DISPLAY_ROTATE_180
        const lv_coord_t dst_x = lcddev.width - 1 - area->y2;
        const lv_coord_t dst_y = area->x1;
        const lv_coord_t dst_w = logical_h;
        const lv_coord_t dst_h = logical_w;

        for (lv_coord_t dy = 0; dy < dst_h; ++dy) {
            uint16_t *dst = g_rot_buf + (size_t)dy * dst_w;
            for (lv_coord_t dx = 0; dx < dst_w; ++dx) {
                const uint32_t argb = px32[(size_t)(logical_h - 1 - dx) * logical_w + dy];
                dst[dx] = (uint16_t)(((argb >> 8) & 0xF800) |
                                     ((argb >> 5) & 0x07E0) |
                                     ((argb >> 3) & 0x001F));
            }
        }
        const esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle,
                                                         dst_x, dst_y,
                                                         dst_x + dst_w, dst_y + dst_h,
                                                         g_rot_buf);
#else
        const lv_coord_t dst_x = area->y1;
        const lv_coord_t dst_y = disp_drv->hor_res - 1 - area->x2;
        const lv_coord_t dst_w = logical_h;
        const lv_coord_t dst_h = logical_w;

        for (lv_coord_t dy = 0; dy < dst_h; ++dy) {
            uint16_t *dst = g_rot_buf + (size_t)dy * dst_w;
            for (lv_coord_t dx = 0; dx < dst_w; ++dx) {
                const uint32_t argb = px32[(size_t)dx * logical_w + (logical_w - 1 - dy)];
                dst[dx] = (uint16_t)(((argb >> 8) & 0xF800) |
                                     ((argb >> 5) & 0x07E0) |
                                     ((argb >> 3) & 0x001F));
            }
        }
        const esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle,
                                                         dst_x, dst_y,
                                                         dst_x + dst_w, dst_y + dst_h,
                                                         g_rot_buf);
#endif
        if (ret != ESP_OK) {
            ESP_LOGE("lvgl_demo", "Partial rotated flush failed: %s", esp_err_to_name(ret));
        }
    } else {
        lv_coord_t area_w = lv_area_get_width(area);
        lv_coord_t area_h = lv_area_get_height(area);
        size_t pixel_cnt = area_w * area_h;
        if (!g_rot_buf || pixel_cnt > (size_t)(lcddev.width * lcddev.height)) {
            if (!g_rot_buf)
                g_rot_buf = (uint16_t *)heap_caps_malloc(lcddev.width * lcddev.height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        }
        for (lv_coord_t y = 0; y < area_h; y++) {
            for (lv_coord_t x = 0; x < area_w; x++) {
                uint32_t argb = px32[y * area_w + x];
                uint8_t r = (argb >> 16) & 0xFF;
                uint8_t g_comp = (argb >> 8) & 0xFF;
                uint8_t b = argb & 0xFF;
                uint16_t rgb565 = ((r >> 3) << 11) | ((g_comp >> 2) << 5) | (b >> 3);
                g_rot_buf[y * area_w + x] = rgb565;
            }
        }
        esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                                  area->x2 + 1, area->y2 + 1, (uint8_t *)g_rot_buf);
    }
    lv_disp_flush_ready(disp_drv);
}

/**
 * @brief       告诉LVGL运行时间
 * @param       arg : 传入参数(未用到)
 * @retval      无
 */
void increase_lvgl_tick(void *arg)
{
    /* 告诉LVGL已经过了多少毫秒 */
    lv_tick_inc(1);
}
