/**
 * @file lv_port_disp.c
 *
 */

 /*Copy this file as "lv_port_disp.c" and set this value to "1" to enable content*/
#if 1

/*********************
 * INCLUDES
 *********************/
#include "lv_port_disp.h"
#include "lvgl.h"
#include "lcd.h"  /* 你的底层的LCD驱动头文件 */

/*********************
 * DEFINES
 *********************/
#define MY_DISP_HOR_RES (800)   /* 屏幕宽度 */
#define MY_DISP_VER_RES (480)   /* 屏幕高度 */

/**********************
 * STATIC PROTOTYPES
 **********************/
/* 显示设备初始化函数 */
static void disp_init(void);

/* 显示设备刷新函数 (这里只保留声明) */
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);


/**********************
 * GLOBAL FUNCTIONS
 **********************/

/**
 * @brief       LCD加速绘制函数
 * @param       (sx,sy),(ex,ey): 矩形区域
 * @param       color: 颜色数组
 * @retval      无
 */
void lcd_draw_fast_rgb_color(int16_t sx, int16_t sy, int16_t ex, int16_t ey, uint16_t *color)
{
    uint16_t w = ex - sx + 1;
    uint16_t h = ey - sy + 1;
    uint32_t draw_size = w * h;
    
    lcd_set_window(sx, sy, w, h);
    lcd_write_ram_prepare();

    for(uint32_t i = 0; i < draw_size; i++)
    {
        lcd_wr_data(color[i]);
    }
}

/**
 * @brief       初始化并注册显示设备给LVGL
 * @param       无
 * @retval      无
 */
void lv_port_disp_init(void)
{
    /* 1. 初始化底层的显示设备 */
    disp_init();

    /* 2. 定义静态数组作为 LVGL 的显存 (极其重要，替代了原本的 malloc) */
    static lv_color_t buf_1[800 * 20]; 

    static lv_disp_draw_buf_t draw_buf_dsc_1;
    lv_disp_draw_buf_init(&draw_buf_dsc_1, buf_1, NULL, 800 * 20);   /* Initialize the display buffer */

    /* 3. 注册显示驱动 (把重复的删掉了，只留一个) */
    static lv_disp_drv_t disp_drv;      /* Descriptor of a display driver */
    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res = MY_DISP_HOR_RES;
    disp_drv.ver_res = MY_DISP_VER_RES;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf_dsc_1;
    
    lv_disp_drv_register(&disp_drv);
}

/**********************
 * STATIC FUNCTIONS
 **********************/

/**
 * @brief       初始化物理屏幕
 */
static void disp_init(void)
{
    /* 你的底层初始化代码 */
    lcd_init();         /* 初始化LCD */
    lcd_display_dir(1); /* 设置横屏 */
}

/**
 * @brief       LVGL用来把颜色刷到屏幕上的接口函数
 */
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    /* 把颜色数组丢给你自己写的底层刷屏函数 */
    lcd_draw_fast_rgb_color(area->x1, area->y1, area->x2, area->y2, (uint16_t*)color_p);

    /* 极其重要!!! 告诉LVGL这块区域已经刷完了 */
    lv_disp_flush_ready(disp_drv);
}

#else /*Enable this file at the top*/

/*This dummy typedef exists purely to silence -Wpedantic.*/
typedef int keep_pedantic_happy;
#endif