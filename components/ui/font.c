// Файл: components/ui/font.c
#include "lvgl.h"

/*
 * ЗАГЛУШКА ДЛЯ ЗБІРКИ.
 * Для реального рендерингу українських гліфів (критерій 4) згенеруйте шрифт:
 * lv_font_conv --font DejaVuSans.ttf --size 20 --bpp 4 \
 *   --range 0x20-0x7F,0x400-0x45F,0x490-0x491 \
 *   --format lvgl --lcd --output components/ui/font_ua.c
 */
const lv_font_t * get_my_ua_font(void) {
    extern const lv_font_t lv_font_montserrat_14;
    return &lv_font_montserrat_14;
}