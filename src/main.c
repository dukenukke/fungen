#include "main.h"
#include "touch_cal.h"


// Вказівники на об'єкти інтерфейсу
lv_obj_t *label_status;
lv_obj_t *screen_main;
lv_obj_t *screen_second;

// Хендл апаратного високоточного таймера
esp_timer_handle_t fungen_timer_handle;

volatile fungen_config_t g_fungen_config = {
    .frequency = 1000,
    .wave_type = 0,
    .amplitude = 1.0
};

uint8_t                  wave_buffer[WAVE_POINTS_NUM];
volatile uint32_t        phase_accumulator = 0;

// Хендл для ЦАП в режимі Oneshot
dac_oneshot_handle_t dac_oneshot_handle = NULL;

// --- ГЕНЕРАЦІЯ ТАБЛИЦЬ ФОРМИ ХВИЛЬ (DDS) ---
static void generate_sine_wave(uint8_t *buf, uint32_t points) {
    for (int i = 0; i < points; i++) {
        buf[i] = (uint8_t)((sin(i * 2.0 * M_PI / points) + 1.0) * 127.5);
    }
}

static void generate_square_wave(uint8_t *buf, uint32_t points) {
    for (int i = 0; i < points; i++) {
        buf[i] = (i < points / 2) ? 255 : 0;
    }
}

static void generate_triangle_wave(uint8_t *buf, uint32_t points) {
    for (int i = 0; i < points; i++) {
        if (i < points / 2) {
            buf[i] = (uint8_t)((i * 255) / (points / 2));
        } else {
            buf[i] = (uint8_t)(255 - (((i - points / 2) * 255) / (points / 2)));
        }
    }
}

static void generate_sawtooth_wave(uint8_t *buf, uint32_t points) {
    for (int i = 0; i < points; i++) {
        buf[i] = (uint8_t)((i * 255) / points);
    }
}

// --- ВИСОКОТОЧНИЙ АПАРАТНИЙ КОЛБЕК ТАЙМЕРА (Вивід сигналу) ---
static void IRAM_ATTR fungen_timer_callback(void* arg) {
    dac_oneshot_output_voltage(dac_oneshot_handle, wave_buffer[phase_accumulator]);
    phase_accumulator++;
    if (phase_accumulator >= WAVE_POINTS_NUM) {
        phase_accumulator = 0;
    }
}

// Колбек для логування координат тачскріна в Serial Port
static void touch_debug_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        lv_indev_t * indev = lv_indev_active();
        if(indev) {
            lv_point_t vect;
            lv_indev_get_point(indev, &vect);
            printf("Touch Detected! X: %ld, Y: %ld\n", (long)vect.x, (long)vect.y);
        }
    }
}

// --- Таск 1: Логіка генератора чи обчислень ---
void generator_task(void *pvParameters) {
    uint32_t counter = 0;
    char buf[32];
    while (1) {
        counter++;
        if (lvgl_port_lock(pdMS_TO_TICKS(20))) {
            if (is_calibrated) {
                snprintf(buf, sizeof(buf), "Fungen Runtime: %lu s", counter);
                lv_label_set_text(label_status, buf);
            }
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- ТАСК КЕРУВАННЯ ГЕНЕРАТОРОМ (FreeRTOS Core 1) ---
void fungen_control_task(void *pvParameters) {
    uint32_t last_freq = 0;
    generate_sine_wave(wave_buffer, WAVE_POINTS_NUM);

    while (1) {
        if (g_fungen_config.frequency != last_freq) {
            last_freq = g_fungen_config.frequency;
            uint64_t period_us = 1000000 / (g_fungen_config.frequency * WAVE_POINTS_NUM);
            if (period_us < 4) period_us = 4;

            esp_timer_stop(fungen_timer_handle);
            esp_timer_start_periodic(fungen_timer_handle, period_us);
            printf("Generator updated. Target Freq: %lu Hz, Step Period: %llu us\n", last_freq, period_us);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Навігаційні колбеки для кнопок переходу між вікнами
static void btn_scr_fwd_cb(lv_event_t * e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_screen_load_anim(screen_second, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}

static void btn_scr_bwd_cb(lv_event_t * e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_screen_load_anim(screen_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
}
static void btn_freq_up_cb(lv_event_t * e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        g_fungen_config.frequency += 100;
    }
}

static void btn_freq_down_cb(lv_event_t * e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (g_fungen_config.frequency > 100) {
            g_fungen_config.frequency -= 100;
        }
    }
}

static void dropdown_wave_cb(lv_event_t * e) {
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        uint16_t selected = lv_dropdown_get_selected(dropdown);
        g_fungen_config.wave_type = selected;
        if (selected == 0) generate_sine_wave(wave_buffer, WAVE_POINTS_NUM);
        else if (selected == 1) generate_square_wave(wave_buffer, WAVE_POINTS_NUM);
        else if (selected == 2) generate_triangle_wave(wave_buffer, WAVE_POINTS_NUM);
        else if (selected == 3) generate_sawtooth_wave(wave_buffer, WAVE_POINTS_NUM);
        printf("Waveform changed to index: %d\n", selected);
    }
}

void create_main_application_ui( void ) {
    // Явно створюємо два об'єкти екранів
    screen_main = lv_obj_create(NULL);
    screen_second = lv_obj_create(NULL);

    lv_obj_add_event_cb(screen_main, touch_debug_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(screen_second, touch_debug_event_cb, LV_EVENT_ALL, NULL);

    // =========================================================================
    // ЕКРАН 1: ГОЛОВНЕ ВІКНО ГЕНЕРАТОРА
    // =========================================================================
    lv_obj_t *btn_scr_fwd = lv_button_create(screen_main);
    lv_obj_set_size(btn_scr_fwd, 40, 35);
    lv_obj_align(btn_scr_fwd, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_t *lbl_fwd = lv_label_create(btn_scr_fwd);
    lv_label_set_text(lbl_fwd, LV_SYMBOL_RIGHT); 
    lv_obj_center(lbl_fwd);
    lv_obj_add_event_cb(btn_scr_fwd, btn_scr_fwd_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(screen_main);
    lv_label_set_text(title, "CYD Lab FunGen v1.0");
    lv_obj_align(title, LV_ALIGN_TOP_MID, -20, 15); 
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);

    label_status = lv_label_create(screen_main);
    lv_label_set_text(label_status, "Initializing...");
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_text_font(label_status, LV_FONT_DEFAULT, 0);

    lv_obj_t *btn_up = lv_button_create(screen_main);
    lv_obj_set_size(btn_up, 100, 45);
    lv_obj_align(btn_up, LV_ALIGN_BOTTOM_RIGHT, -20, -70);
    lv_obj_t *lbl_up = lv_label_create(btn_up);
    lv_label_set_text(lbl_up, "+100 Hz");
    lv_obj_center(lbl_up);
    lv_obj_add_event_cb(btn_up, btn_freq_up_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_down = lv_button_create(screen_main);
    lv_obj_set_size(btn_down, 100, 45);
    lv_obj_align(btn_down, LV_ALIGN_BOTTOM_LEFT, 20, -70);
    lv_obj_t *lbl_down = lv_label_create(btn_down);
    lv_label_set_text(lbl_down, "-100 Hz");
    lv_obj_center(lbl_down);
    lv_obj_add_event_cb(btn_down, btn_freq_down_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dd = lv_dropdown_create(screen_main);
    lv_dropdown_set_options(dd, "Sine Wave\nSquare\nTriangle\nSawtooth");
    lv_obj_set_size(dd, 160, 40);
    lv_obj_align(dd, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_add_event_cb(dd, dropdown_wave_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // =========================================================================
    // ЕКРАН 2: ДРУГЕ ВІКНО НАЛАШТУВАНЬ
    // =========================================================================
    lv_obj_t *btn_scr_bwd = lv_button_create(screen_second);
    lv_obj_set_size(btn_scr_bwd, 40, 35);
    lv_obj_align(btn_scr_bwd, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_t *lbl_bwd = lv_label_create(btn_scr_bwd);
    lv_label_set_text(lbl_bwd, LV_SYMBOL_LEFT); 
    lv_obj_center(lbl_bwd);
    lv_obj_add_event_cb(btn_scr_bwd, btn_scr_bwd_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_sec_title = lv_label_create(screen_second);
    lv_label_set_text(lbl_sec_title, "Secondary Settings Screen");
    lv_obj_align(lbl_sec_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(lbl_sec_title, LV_FONT_DEFAULT, 0);

    // Завантажуємо перший екран за замовчуванням
    lv_screen_load(screen_main);
}

void create_demo_ui(lv_display_t *disp) {
    init_nvs_calibration();
    if (load_calibration_data()) {
        is_calibrated = true;
        create_main_application_ui();
    } else {
        is_calibrated = false;
        start_interactive_calibration(disp);
    }
}

void app_main(void) {
    const lvgl_port_cfg_t lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << CYD_PIN_TFT_BL
    };
    gpio_config(&bk_gpio_config);
    gpio_set_level(CYD_PIN_TFT_BL, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = CYD_PIN_TFT_CLK,
        .mosi_io_num = CYD_PIN_TFT_MOSI,
        .miso_io_num = CYD_PIN_TFT_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1, // Unused quad-SPI signal; touch IRQ is configured below.
        .max_transfer_sz = CYD_RES_H * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CYD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    // 4. TFT panel communication setup via esp_lcd
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CYD_PIN_TFT_DC,
        .cs_gpio_num = CYD_PIN_TFT_CS,
        .pclk_hz = 20 * 1000 * 1000, // 20 MHz
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CYD_SPI_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CYD_PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true)); 
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true)); 
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true)); 

    // =========================================================================
    // 6. Ініціалізація порту дисплея в LVGL (Синхронізація розширення буфера)
    // =========================================================================
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = CYD_RES_H * 40,
        .double_buffer = true,
        .hres = CYD_RES_H,
        .vres = CYD_RES_V,
        .monochrome = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        }
    };
    lv_display_t * lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    // =========================================================================
    // 6.а. Ініціалізація ДРУГОЇ шини SPI3 спеціально для Тачскріна
    // =========================================================================
    esp_lcd_touch_handle_t touch_handle = NULL;

    spi_bus_config_t touch_buscfg = {
        .sclk_io_num = CYD_PIN_TOUCH_CLK,
        .mosi_io_num = CYD_PIN_TOUCH_MOSI,
        .miso_io_num = CYD_PIN_TOUCH_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CYD_TOUCH_SPI_HOST, &touch_buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t touch_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t touch_io_config = {
        .dc_gpio_num = -1, 
        .cs_gpio_num = CYD_PIN_TOUCH_CS,    // GPIO33
        .pclk_hz = 2 * 1000 * 1000, 
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 3,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CYD_TOUCH_SPI_HOST, &touch_io_config, &touch_io_handle));

    esp_lcd_touch_config_t touch_config = {
        .x_max = CYD_RES_H,
        .y_max = CYD_RES_V,
        .rst_gpio_num = -1,
        .int_gpio_num = CYD_PIN_TOUCH_INT,
        .levels = {
            .interrupt = 0, // XPT2046 PENIRQ is active low.
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,  
            .mirror_y = 0, // Calibration supplies the complete display-coordinate mapping.
        },
        .process_coordinates = touch_coordinate_transformer,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(touch_io_handle, &touch_config, &touch_handle));
    lv_display_set_user_data(lvgl_disp, touch_handle); 

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lv_indev_t *touch_indev = lvgl_port_add_touch(&touch_cfg);
    ESP_ERROR_CHECK(touch_indev ? ESP_OK : ESP_FAIL);

    // Чистий запуск Oneshot ЦАП під новий тулчейн
    dac_oneshot_config_t dac_cfg = {
        .chan_id = DAC_CHAN_1,
    };
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&dac_cfg, &dac_oneshot_handle));

    if (lvgl_port_lock(portMAX_DELAY)) { 
        create_demo_ui(lvgl_disp);
        lvgl_port_unlock(); 
    }

    const esp_timer_create_args_t fungen_timer_args = {
        .callback = &fungen_timer_callback,
        .name = "fungen_hardware_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&fungen_timer_args, &fungen_timer_handle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(fungen_timer_handle, 31));

    xTaskCreatePinnedToCore(fungen_control_task, "fungen_control_task", 4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(generator_task, "ui_refresh_task", 3072, NULL, 4, NULL, 0);

    printf("Fungen hardware and multi-screen touch tasks deployed successfully.\n");
}
