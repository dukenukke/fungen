#include "touch_cal.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"
#include "cyd_pins.h"
#include "sdkconfig.h"
#include <stddef.h>
#include <stdio.h>

#if CONFIG_XPT2046_CONVERT_ADC_TO_COORDS
#error "Touch calibration requires raw XPT2046 ADC coordinates"
#endif

#define CAL_VERSION 2
#define CAL_MIN_SAMPLES 3
#define CAL_MAX_SAMPLES 8
#define CAL_SAMPLE_TOLERANCE 120
#define CAL_MARGIN 40

touch_calibration_data_t cal_data;
bool is_calibrated = false;

/* All capture and UI work runs in the LVGL task under its port lock. */
static int calibration_step = -1;
static bool step_pending;
static uint32_t sample_x, sample_y;
static unsigned sample_count;
static touch_cal_point_t captured[4];
static const touch_cal_point_t targets[4] = {
    {CAL_MARGIN, CAL_MARGIN},
    {CYD_RES_H - 1 - CAL_MARGIN, CAL_MARGIN},
    {CAL_MARGIN, CYD_RES_V - 1 - CAL_MARGIN},
    {CYD_RES_H - 1 - CAL_MARGIN, CYD_RES_V - 1 - CAL_MARGIN},
};
static const char *instructions[4] = {
    "Hold the Top-Left cross, then release",
    "Hold the Top-Right cross, then release",
    "Hold the Bottom-Left cross, then release",
    "Hold the Bottom-Right cross, then release",
};
static lv_obj_t *cal_screen, *cal_cross, *cal_label;

extern void create_main_application_ui(void);

static uint32_t calculate_cal_crc(const touch_calibration_data_t *data) {
    return esp_rom_crc32_le(0, (const uint8_t *)data, offsetof(touch_calibration_data_t, crc));
}

void init_nvs_calibration(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

bool load_calibration_data(void) {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READONLY, &handle) != ESP_OK) return false;
    touch_calibration_data_t saved = {0};
    size_t size = sizeof(saved);
    esp_err_t err = nvs_get_blob(handle, "touch_cal", &saved, &size);
    nvs_close(handle);
    /* Old min/max calibration cannot correct rotation; require a fresh four-point fit. */
    if (err != ESP_OK || size != sizeof(saved) || saved.version != CAL_VERSION ||
        saved.crc != calculate_cal_crc(&saved) || !touch_cal_mapping_valid(&saved.mapping)) return false;
    cal_data = saved;
    return true;
}

void erase_calibration_data(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) return;
    err = nvs_erase_key(handle, "touch_cal");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) printf("[CAL] Calibration data erased from NVS.\n");
}

static void save_calibration_data(void) {
    cal_data.version = CAL_VERSION;
    cal_data.crc = calculate_cal_crc(&cal_data);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "touch_cal", &cal_data, sizeof(cal_data));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) printf("[CAL] Affine calibration saved to NVS.\n");
    else printf("[CAL] NVS save failed: %s; calibration applies until reboot.\n", esp_err_to_name(err));
}

static void reset_samples(void) {
    sample_x = sample_y = sample_count = 0;
}

void touch_coordinate_transformer(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                  uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {
    (void)tp;
    (void)strength;
    for (uint8_t i = 0; i < *point_num && i < max_point_num; ++i) {
        if (is_calibrated) {
            touch_cal_point_t p = touch_cal_map(&cal_data.mapping, (touch_cal_point_t){x[i], y[i]});
            x[i] = (uint16_t)lroundf(fminf(CYD_RES_H - 1, fmaxf(0, p.x)));
            y[i] = (uint16_t)lroundf(fminf(CYD_RES_V - 1, fmaxf(0, p.y)));
        } else {
            if (i == 0 && calibration_step >= 0 && calibration_step < 4 && !step_pending &&
                sample_count < CAL_MAX_SAMPLES) {
                /* Restart an unstable hold; freeze after eight samples to ignore lift-off drift. */
                if (sample_count && (fabsf(x[i] - (float)sample_x / sample_count) > CAL_SAMPLE_TOLERANCE ||
                                     fabsf(y[i] - (float)sample_y / sample_count) > CAL_SAMPLE_TOLERANCE)) {
                    reset_samples();
                }
                sample_x += x[i];
                sample_y += y[i];
                ++sample_count;
            }
            /* Route every physical press to the full-screen capture surface.
             * Raw ADC values must never participate in LVGL hit testing. */
            x[i] = CYD_RES_H / 2;
            y[i] = CYD_RES_V / 2;
        }
    }
}

static void show_target(void) {
    lv_obj_set_pos(cal_cross, (int32_t)targets[calibration_step].x - 15,
                             (int32_t)targets[calibration_step].y - 15);
    lv_label_set_text(cal_label, instructions[calibration_step]);
    printf("[CAL UI] Step %d: tap displayed cross at (%d, %d).\n", calibration_step + 1,
           (int)targets[calibration_step].x, (int)targets[calibration_step].y);
}

static void cal_timer_step_cb(lv_timer_t *timer) {
    (void)timer;
    reset_samples();
    step_pending = false;
    if (++calibration_step < 4) {
        show_target();
        return;
    }
    if (!touch_cal_fit(captured, targets, &cal_data.mapping)) {
        printf("[CAL] Inconsistent points; restarting calibration.\n");
        calibration_step = 0;
        show_target();
        lv_label_set_text(cal_label, "Calibration failed.\nHold the Top-Left cross, then release");
        return;
    }
    printf("[CAL MATH] X = %.6f * rawX + %.6f * rawY + %.2f; "
           "Y = %.6f * rawX + %.6f * rawY + %.2f\n",
           cal_data.mapping.xx, cal_data.mapping.xy, cal_data.mapping.x_offset,
           cal_data.mapping.yx, cal_data.mapping.yy, cal_data.mapping.y_offset);
    save_calibration_data();
    is_calibrated = true;
    calibration_step = -1;
    /* Switch away before deleting the old active screen. */
    create_main_application_ui();
    lv_obj_delete(cal_screen);
    cal_screen = cal_cross = cal_label = NULL;
}

static void cal_release_cb(lv_event_t *event) {
    (void)event;
    if (step_pending || calibration_step < 0 || calibration_step >= 4) return;
    if (sample_count < CAL_MIN_SAMPLES) {
        reset_samples();
        lv_label_set_text(cal_label, "Hold the displayed cross a little longer,\nthen release");
        return;
    }
    captured[calibration_step] = (touch_cal_point_t) {
        (float)sample_x / sample_count, (float)sample_y / sample_count,
    };
    printf("[CAL TOUCH] Step: %d | ADC X: %.1f, Y: %.1f | samples: %u\n",
           calibration_step + 1, captured[calibration_step].x, captured[calibration_step].y, sample_count);
    step_pending = true;
    /* Defer screen changes until LVGL has finished dispatching this release. */
    lv_timer_t *timer = lv_timer_create(cal_timer_step_cb, 10, NULL);
    if (timer) {
        lv_timer_set_repeat_count(timer, 1);
    } else {
        step_pending = false;
        reset_samples();
        lv_label_set_text(cal_label, "Unable to advance. Please tap again.");
    }
}

void start_interactive_calibration(lv_display_t *disp) {
    printf("[CAL] Starting raw ADC four-point calibration.\n");
    (void)disp;
    calibration_step = 0;
    is_calibrated = false;
    step_pending = false;
    reset_samples();

    cal_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(cal_screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(cal_screen, 0, 0);
    lv_obj_set_style_border_width(cal_screen, 0, 0);
    lv_obj_remove_flag(cal_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cal_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cal_screen, cal_release_cb, LV_EVENT_RELEASED, NULL);

    cal_label = lv_label_create(cal_screen);
    lv_obj_set_width(cal_label, CYD_RES_H - 20);
    lv_obj_set_style_text_align(cal_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(cal_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(cal_label, lv_color_white(), 0);
    lv_obj_remove_flag(cal_label, LV_OBJ_FLAG_CLICKABLE);

    cal_cross = lv_obj_create(cal_screen);
    lv_obj_remove_style_all(cal_cross);
    lv_obj_set_size(cal_cross, 31, 31);
    lv_obj_remove_flag(cal_cross, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *horizontal = lv_obj_create(cal_cross);
    lv_obj_remove_style_all(horizontal);
    lv_obj_set_size(horizontal, 31, 1);
    lv_obj_center(horizontal);
    lv_obj_set_style_bg_color(horizontal, lv_color_hex(0xff4040), 0);
    lv_obj_set_style_bg_opa(horizontal, LV_OPA_COVER, 0);
    lv_obj_remove_flag(horizontal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *vertical = lv_obj_create(cal_cross);
    lv_obj_remove_style_all(vertical);
    lv_obj_set_size(vertical, 1, 31);
    lv_obj_center(vertical);
    lv_obj_set_style_bg_color(vertical, lv_color_hex(0xff4040), 0);
    lv_obj_set_style_bg_opa(vertical, LV_OPA_COVER, 0);
    lv_obj_remove_flag(vertical, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    show_target();
    lv_screen_load(cal_screen);
}
