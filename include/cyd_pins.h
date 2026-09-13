#pragma once

// Шина Дисплея (HSPI / SPI2)
#define CYD_SPI_HOST         SPI2_HOST
#define CYD_PIN_TFT_MISO     12   // can be unused if not reading from display
#define CYD_PIN_TFT_MOSI     13
#define CYD_PIN_TFT_CLK      14
#define CYD_PIN_TFT_CS       15
#define CYD_PIN_TFT_DC       2
#define CYD_PIN_TFT_RST      -1  
#define CYD_PIN_TFT_BL       21  //backlight control pin

// Окрема Шина Тачскріна (VSPI / SPI3) за вашим YML
#define CYD_TOUCH_SPI_HOST   SPI3_HOST
#define CYD_PIN_TOUCH_CLK    25   // CLK з вашого фото
#define CYD_PIN_TOUCH_MOSI   32   // MOSI з вашого фото
#define CYD_PIN_TOUCH_MISO   39   // MISO з вашого фото
#define CYD_PIN_TOUCH_CS     33   // Стандартний CS для двошинної плати

#define CYD_RES_H            320
#define CYD_RES_V            240
