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
#include "touch.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui/ui.h"

/* MIPI 竖屏使用横屏 UI；最终帧由 flush_cb 旋转到物理面板。 */
static bool g_need_rotate = false;
static uint16_t *g_rot_buf = NULL;

/* Keep the display rotation in software.  Flipping the HX8399 scan order via
 * MADCTL corrupts output on this panel, while this mapping rotates the whole
 * logical landscape frame by 180 degrees reliably. */
#define LVGL_DISPLAY_ROTATE_180 1

/* Forward declarations */
void increase_lvgl_tick(void *arg);
static bool touchpad_is_pressed(void);
static void touchpad_get_xy(lv_coord_t *x, lv_coord_t *y);

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
        lv_timer_handler();             /* LVGL计时器处理 */
        esp_task_wdt_reset();           /* 喂狗 */
        vTaskDelay(pdMS_TO_TICKS(10));  /* 延时10毫秒 */
    }
}

/**
 * @brief       lvgl_demo入口函数
 * @param       无
 * @retval      无
 */
void lvgl_demo(void)
{
    lv_init();              /* 初始化LVGL图形to_rgb565.c:256库 */
    lv_port_disp_init();    /* lvgl显示接口初始化,放在lv_init()的后面 */
    lv_port_indev_init();   /* lvgl输入接口初始化,放在lv_init()的后面 */

    /* 为LVGL提供时基单元 */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,    /* 设置定时器回调 */
        .name = "lvgl_tick"                 /* 定时器名称 */
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));     /* 创建定时器 */
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1 * 1000));           /* 启动定时器 */

    /* EEZ Studio UI */
    ui_init();

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

    /* 全屏刷新 + 单缓冲: 避免部分刷新时临时缓冲区池耗尽导致死锁 */
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
    disp_drv.full_refresh = 1;

    return lv_disp_drv_register(&disp_drv);
}

/**
 * @brief       初始化并注册输入设备 (LVGL v8)
 * @param       无
 * @retval      lvgl输入设备指针
 */
lv_indev_t *lv_port_indev_init(void)
{
    static lv_indev_drv_t indev_drv;

    /* 初始化触摸屏 */
    tp_dev.init();

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;

    return lv_indev_drv_register(&indev_drv);
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
        lv_coord_t logical_w = lv_area_get_width(area);
        lv_coord_t phys_w = lcddev.width;
        lv_coord_t phys_h = lcddev.height;
#if !LVGL_DISPLAY_ROTATE_180
        lv_coord_t logical_total_w = disp_drv->hor_res;
#endif

        for (lv_coord_t ly = area->y1; ly <= area->y2; ly++) {
            for (lv_coord_t lx = area->x1; lx <= area->x2; lx++) {
                lv_coord_t px;
                lv_coord_t py;

#if LVGL_DISPLAY_ROTATE_180
                /* Landscape (lx, ly) -> portrait, with a 180 degree turn. */
                px = phys_w - 1 - ly;
                py = lx;
#else
                px = ly;
                py = logical_total_w - 1 - lx;
#endif
                if (px >= 0 && px < phys_w && py >= 0 && py < phys_h) {
                    uint32_t argb = px32[(ly - area->y1) * logical_w + (lx - area->x1)];
                    uint8_t r = (argb >> 16) & 0xFF;
                    uint8_t g_comp = (argb >> 8) & 0xFF;
                    uint8_t b = argb & 0xFF;
                    uint16_t rgb565 = ((r >> 3) << 11) | ((g_comp >> 2) << 5) | (b >> 3);
                    g_rot_buf[py * phys_w + px] = rgb565;
                }
            }
        }
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, phys_w, phys_h, (uint8_t *)g_rot_buf);
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

/**
 * @brief       获取触摸屏设备的状态
 * @param       无
 * @retval      返回触摸屏设备是否被按下
 */
static bool touchpad_is_pressed(void)
{
    tp_dev.scan(0);     /* 触摸按键扫描 */

    if (tp_dev.sta & TP_PRES_DOWN)
    {
        return true;
    }

    return false;
}


/**
 * @brief       在触摸屏被按下的时候读取 x、y 坐标
 * @param       x   : x坐标的指针
 * @param       y   : y坐标的指针
 * @retval      无
 */
static void touchpad_get_xy(lv_coord_t *x, lv_coord_t *y)
{
    if (g_need_rotate) {
        /* Apply the inverse of the flush mapping so touch remains aligned. */
#if LVGL_DISPLAY_ROTATE_180
        *x = tp_dev.y[0];
        *y = lcddev.width - 1 - tp_dev.x[0];
#else
        *x = lcddev.height - 1 - tp_dev.y[0];
        *y = tp_dev.x[0];
#endif
    } else {
        (*x) = tp_dev.x[0];
        (*y) = tp_dev.y[0];
    }
}

/**
 * @brief       图形库的触摸屏读取回调函数
 * @param       indev_drv   : 触摸屏设备
 * @param       data        : 输入设备数据结构体
 * @retval      无
 */
void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    /* 保存按下的坐标和状态 */
    if(touchpad_is_pressed())
    {
        touchpad_get_xy(&last_x, &last_y);  /* 在触摸屏被按下的时候读取 x、y 坐标 */
        data->state = LV_INDEV_STATE_PR;
    } 
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }

    /* 设置最后按下的坐标 */
    data->point.x = last_x;
    data->point.y = last_y;
}
