#include "touch_cal.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"
#include "cyd_pins.h"
#include <stdio.h>

touch_calibration_data_t cal_data;
bool is_calibrated = false;

typedef enum { CAL_STATE_NONE = 0, CAL_STATE_TOP_LEFT, CAL_STATE_TOP_RIGHT, CAL_STATE_BOTTOM_LEFT, CAL_STATE_BOTTOM_RIGHT, CAL_STATE_DONE } cal_state_t;
volatile cal_state_t calibration_state = CAL_STATE_NONE;

int32_t raw_x_accum[5] = {0};
int32_t raw_y_accum[5] = {0};

lv_obj_t *cal_screen = NULL;
lv_obj_t *cal_cross = NULL;
lv_obj_t *cal_label = NULL;

void create_main_application_ui(void); // Связь с main.c

static uint32_t calculate_cal_crc(touch_calibration_data_t *data) {
    return esp_rom_crc32_le(0, (uint8_t const *)data, sizeof(touch_calibration_data_t) - sizeof(uint32_t));
}

void init_nvs_calibration(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
}

bool load_calibration_data(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) != ESP_OK) return false;
    size_t required_size = sizeof(touch_calibration_data_t);
    esp_err_t err = nvs_get_blob(my_handle, "touch_cal", &cal_data, &required_size);
    nvs_close(my_handle);
    if (err != ESP_OK || cal_data.crc != calculate_cal_crc(&cal_data)) return false;
    return true;
}

static void save_calibration_data(void) {
    cal_data.crc = calculate_cal_crc(&cal_data);
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_blob(my_handle, "touch_cal", &cal_data, sizeof(touch_calibration_data_t));
        nvs_commit(my_handle);
        nvs_close(my_handle);
        printf("[CAL] Coefficients successfully saved to NVS.\n");
    }
}

void touch_coordinate_transformer(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {
    if (*point_num == 0) return;
    
    if (is_calibrated) {
        int32_t raw_x = *x;
        int32_t raw_y = *y;

        int32_t calc_x = ((raw_x - cal_data.x_min) * CYD_RES_H) / (cal_data.x_max - cal_data.x_min);
        int32_t calc_y = ((raw_y - cal_data.y_min) * CYD_RES_V) / (cal_data.y_max - cal_data.y_min);
        
        if (calc_x < 0) calc_x = 0;
        if (calc_x > CYD_RES_H) calc_x = CYD_RES_H;
        if (calc_y < 0) calc_y = 0;
        if (calc_y > CYD_RES_V) calc_y = CYD_RES_V;
        
        *x = (uint16_t)calc_x; 
        *y = (uint16_t)calc_y;
    }
}

// --- БЕЗОПАСНЫЙ СИНХРОННЫЙ ТАЙМЕР LVGL (Убирает Deadlock) ---
static void cal_timer_step_cb(lv_timer_t * timer) {
    // Удаляем отработавший одноразовый таймер
    lv_timer_delete(timer);

    if (calibration_state == CAL_STATE_TOP_LEFT) {
        calibration_state = CAL_STATE_TOP_RIGHT;
        lv_obj_align(cal_cross, LV_ALIGN_TOP_RIGHT, -15, 15);
        lv_label_set_text(cal_label, "Touch Top-Right Corner");
        printf("[CAL UI] Advanced to step: Top-Right\n");
    } else if (calibration_state == CAL_STATE_TOP_RIGHT) {
        calibration_state = CAL_STATE_BOTTOM_LEFT;
        lv_obj_align(cal_cross, LV_ALIGN_BOTTOM_LEFT, 15, -15);
        lv_label_set_text(cal_label, "Touch Bottom-Left Corner");
        printf("[CAL UI] Advanced to step: Bottom-Left\n");
    } else if (calibration_state == CAL_STATE_BOTTOM_LEFT) {
        calibration_state = CAL_STATE_BOTTOM_RIGHT;
        lv_obj_align(cal_cross, LV_ALIGN_BOTTOM_RIGHT, -15, -15);
        lv_label_set_text(cal_label, "Touch Bottom-Right Corner");
        printf("[CAL UI] Advanced to step: Bottom-Right\n");
    } else if (calibration_state == CAL_STATE_BOTTOM_RIGHT) {
        calibration_state = CAL_STATE_DONE;

        cal_data.x_min = (raw_x_accum[CAL_STATE_TOP_LEFT] + raw_x_accum[CAL_STATE_BOTTOM_LEFT]) / 2;
        cal_data.x_max = (raw_x_accum[CAL_STATE_TOP_RIGHT] + raw_x_accum[CAL_STATE_BOTTOM_RIGHT]) / 2;
        cal_data.y_min = (raw_y_accum[CAL_STATE_TOP_LEFT] + raw_y_accum[CAL_STATE_TOP_RIGHT]) / 2;
        cal_data.y_max = (raw_y_accum[CAL_STATE_BOTTOM_LEFT] + raw_y_accum[CAL_STATE_BOTTOM_RIGHT]) / 2;

        printf("[CAL MATH] Bounds calculated -> X_MIN: %ld, X_MAX: %ld | Y_MIN: %ld, Y_MAX: %ld\n", 
                (long)cal_data.x_min, (long)cal_data.x_max, (long)cal_data.y_min, (long)cal_data.y_max);

        save_calibration_data();
        is_calibrated = true;
        
        lv_obj_delete(cal_screen);
        calibration_state = CAL_STATE_NONE;
        create_main_application_ui();
    }
}

static void cal_button_click_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t * indev = lv_indev_active();
        if (indev) {
            lv_point_t pt;
            lv_indev_get_point(indev, &pt);
            
            raw_x_accum[calibration_state] = pt.x;
            raw_y_accum[calibration_state] = pt.y;
            
            printf("[CAL TOUCH] State: %d | Captured X: %ld, Y: %ld\n", calibration_state, (long)pt.x, (long)pt.y);
        }
    }

    if (code != LV_EVENT_CLICKED) return;

    printf("[CAL UI] Click verified for step: %d. Creating LVGL Timer...\n", calibration_state);
    
    // Вместо async_call запускаем встроенный таймер LVGL на 10 мс.
    // Он гарантированно выполнится в контексте потока графики и сдвинет хрестик.
    lv_timer_create(cal_timer_step_cb, 10, NULL);
}

void start_interactive_calibration(lv_display_t *disp) {
    printf("[CAL] Starting interactive 4-point calibration UI...\n");
    calibration_state = CAL_STATE_TOP_LEFT;
    is_calibrated = false;
    
    cal_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(cal_screen, lv_color_black(), 0);

    cal_label = lv_label_create(cal_screen);
    lv_label_set_text(cal_label, "Touch Top-Left Corner");
    lv_obj_align(cal_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(cal_label, lv_color_white(), 0);

    cal_cross = lv_button_create(cal_screen);
    lv_obj_set_size(cal_cross, 70, 70); 
    lv_obj_set_style_bg_color(cal_cross, lv_color_make(60, 0, 0), 0); 
    lv_obj_set_style_bg_opa(cal_cross, LV_OPA_40, 0);
    
    lv_obj_t *cross_label = lv_label_create(cal_cross);
    lv_label_set_text(cross_label, "+");
    lv_obj_center(cross_label);
    lv_obj_set_style_text_color(cross_label, lv_color_make(255, 0, 0), 0);

    lv_obj_align(cal_cross, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(cal_cross, cal_button_click_cb, LV_EVENT_ALL, NULL);
    lv_screen_load(cal_screen);
}
