#pragma once
#include "lvgl.h"
#include "esp_lcd_touch.h"

// Структура для збереження коефіцієнтів у пам'яті NVS Flash
typedef struct {
    int32_t x_min;
    int32_t x_max;
    int32_t y_min;
    int32_t y_max;
    uint32_t crc;
} touch_calibration_data_t;

extern touch_calibration_data_t cal_data;
extern bool is_calibrated;

// Функції модуля калібрування
void init_nvs_calibration(void);
bool load_calibration_data(void);
void start_interactive_calibration(lv_display_t *disp);
void touch_coordinate_transformer(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
