#ifndef __MAIN_H
#define __MAIN_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "cyd_pins.h"
#include "esp_timer.h"
#include "driver/dac_oneshot.h"
#include "esp_check.h"
#include <math.h>


#define WAVE_POINTS_NUM     128 // Кількість точок на один період хвилі

// Структура для збереження поточних параметрів генератора
typedef struct {
    uint32_t frequency;   // Поточна частота в Гц
    uint8_t wave_type;    // 0: Синусоїда, 1: Квадрат, 2: Трикутник
    float amplitude;      // Амплітуда від 0.0 до 1.0
} fungen_config_t;

void create_demo_ui(lv_display_t *);

#endif // __MAIN_H