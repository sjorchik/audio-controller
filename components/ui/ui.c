// Файл: components/ui/ui.c
/*
 * ui.c — дорога карта v2.2.1, пункт 5 (ui). Ландшафт, ревізія 5 (фінальна bring-up).
 *
 * Bring-up: esp_lcd SPI ST7789 (панель 170x320, встановлена ГОРИЗОНТАЛЬНО:
 * логічний екран 320x170 через swap_xy) @40 MHz + підсвітка LEDC 5 кГц +
 * dev-команда lcdtest (fill/gradient/corners/border/offset/orient),
 * яка малює СМУГАМИ (без full-screen алокацій у internal RAM).
 *
 * LVGL 9: задача ui (core 0, пріоритет 8) — tick 5 мс (esp_timer),
 * рендер event-driven (черга шини system) + fallback-оновлення 100 мс.
 *
 * Схема буферів (підтверджена фото bring-up):
 *   - PARTIAL-рендер, ОДИН draw-буфер 320*60*2 = 38 400 Б у PSRAM
 *     (чанк LVGL = весь буфер, px_map пакований).
 *   - ПОДВІЙНИЙ bounce у internal DMA RAM (2 x 38 400 Б), БЕЗ семафорів:
 *     черга DMA глибиною 1 серіалізує трансфери, тому memcpy чанка K+2
 *     у bounce[K%2] завжди ПО завершенні DMA K.
 *   - MUTEX s_panel_mtx навколо ВСІХ звернень esp_lcd_panel_* (flush_cb,
 *     lcdtest, orient/offset): polling-трансакції SPI не допускають
 *     одночасних викликів з двох задач (блокування console-задачі
 *     на bring-up). LVGL-домальовування брудних областей поверх
 *     lcdtest fill — очікувана поведінка сумісного доступу.
 *
 * Орієнтація/offsets: дефолт з Kconfig (APP_UI_ORIENT_*) + offsets (0,35),
 * підтверджені bring-up (border рівна, шуму немає); runtime-перемикання
 * лишиться командою "lcdtest orient".
 *
 * Навігація строго за картою керування v2.2.5:
 *   Main: ENC=гучність, LEFT/RIGHT=джерело, OK=меню;
 *   Меню: ENC=фокус, OK=вибір, LONG OK=назад;
 *   EQ:   ENC=смуга, LEFT/RIGHT=значення.
 */

#include "ui.h"
#include "bsp.h"
#include "system.h"
#include "audio.h"
#include "tda7318.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ui";

/* ── Панель ST7789 170x320, встановлена горизонтально ── */
#define LCD_PANEL_W        170   /* фізична панель, портрет */
#define LCD_PANEL_H        320
#define LCD_LOG_W          320   /* логічний екран, ландшафт */
#define LCD_LOG_H          170
#define LCD_SPI_MHZ        40   /* за спекою: старт 40 MHz; за шуму на
                                 * lcdtest fill на провідниках — зменшити до 20 */

/* ── Дефолти орієнтації/offsets (підтверджені bring-up) ── */
#if defined(CONFIG_APP_UI_ORIENT_FINAL)
#define LCD_DEF_MX         1
#define LCD_DEF_MY         0
#else
#define LCD_DEF_MX         0
#define LCD_DEF_MY         1
#endif
#define LCD_DEF_OX         0
#define LCD_DEF_OY         35   /* 240-170=70; з mirror по осі стовпців
                                 * видиме вікно панелі потребує 35 —
                                 * підтверждено border/corners на bring-up */

/* ── Буфери: чанк LVGL = 60 рядків на всю ширину ── */
#define CHUNK_ROWS         60
#define CHUNK_SIZE         (LCD_LOG_W * CHUNK_ROWS * sizeof(lv_color_t))  /* 38 400 Б */

/* Тимчасовий буфер lcdtest: 32 рядки (20 480 Б), малюємо смугами */
#define TEST_ROWS          32

/* Вікно самомірювання CPU задачі, µs */
#define CPU_WINDOW_US      5000000

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static SemaphoreHandle_t s_panel_mtx = NULL;   /* серіалізація esp_lcd panel API */
static QueueHandle_t s_ui_queue = NULL;

static uint8_t *s_draw_buf = NULL;              /* draw-буфер LVGL, PSRAM */
static uint8_t *s_bounce[2] = { NULL, NULL };   /* подвійний bounce, internal RAM */
static int s_bounce_idx = 0;

static lv_display_t *s_disp = NULL;

/* ── Екрани та віджети ── */
static lv_obj_t *s_scr[6];
static lv_obj_t *s_lbl_main_src, *s_lbl_main_vol, *s_lbl_main_mute, *s_lbl_main_preset;
static lv_obj_t *s_lbl_src_items[4];
static lv_obj_t *s_lbl_eq_bands[10];
static lv_obj_t *s_lbl_settings_items[4];
static lv_obj_t *s_lbl_info_fw, *s_lbl_info_heap, *s_lbl_info_uptime;

typedef enum {
    UI_SCREEN_MAIN = 0,
    UI_SCREEN_MENU,
    UI_SCREEN_SOURCE,
    UI_SCREEN_EQ,
    UI_SCREEN_SETTINGS,
    UI_SCREEN_INFO
} ui_screen_t;

static ui_screen_t s_current_screen = UI_SCREEN_MAIN;
static int s_focus_idx = 0;

/* Offsets GRAM у логічних (ландшафтних) координатах */
static int s_offset_x = LCD_DEF_OX;
static int s_offset_y = LCD_DEF_OY;

/* Шрифт з UA-діапазоном (див. font.c; для продакшну — lv_font_conv) */
const lv_font_t *get_my_ua_font(void);
static void apply_font(lv_obj_t *obj) { lv_obj_set_style_text_font(obj, get_my_ua_font(), 0); }

/* ── Flush LVGL → esp_lcd (PARTIAL, чанк = весь draw-буфер, px_map пакований) ── */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int x1 = area->x1, x2 = area->x2, y1 = area->y1, y2 = area->y2;
    const size_t bytes = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * sizeof(lv_color_t);

    uint8_t *bounce = s_bounce[s_bounce_idx];
    s_bounce_idx ^= 1;

    memcpy(bounce, px_map, bytes < CHUNK_SIZE ? bytes : CHUNK_SIZE);

    if (xSemaphoreTake(s_panel_mtx, portMAX_DELAY) == pdTRUE) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2 + 1, y2 + 1, bounce);
        xSemaphoreGive(s_panel_mtx);
    }

    lv_display_flush_ready(disp);
}

static void build_screens(void);
static void update_ui_state(void);
static void apply_focus(void);
static void navigate(ui_screen_t screen);
static void handle_input(const input_event_t *evt);

/* LVGL tick 5 мс: контекст задачі esp_timer (не ISR GPIO) */
static void tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "UI init: SPI ST7789 %dx%d panel -> %dx%d logical @%d MHz, LEDC 5 kHz, LVGL 9",
             LCD_PANEL_W, LCD_PANEL_H, LCD_LOG_W, LCD_LOG_H, LCD_SPI_MHZ);
    ESP_LOGI(TAG, "Orient default: swap=1 mirror=(%d,%d) offset=(%d,%d)",
             LCD_DEF_MX, LCD_DEF_MY, LCD_DEF_OX, LCD_DEF_OY);

    s_panel_mtx = xSemaphoreCreateMutex();
    if (!s_panel_mtx) return ESP_ERR_NO_MEM;

    /* ── 1. SPI-шина дисплея ── */
    spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_PIN_ST7789_SCLK,
        .mosi_io_num = BSP_PIN_ST7789_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_SIZE + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* ── 2. Panel IO (SPI). trans_queue_depth=1: черга з одного трансферу —
     *    умова серіалізації DMA, на якій стоїть безпека подвійного bounce. ── */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_PIN_ST7789_DC,
        .cs_gpio_num = BSP_PIN_ST7789_CS,
        .pclk_hz = LCD_SPI_MHZ * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    /* ── 3. Панель ST7789: ландшафт через swap_xy; орієнтація з Kconfig ── */
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 16,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = BSP_PIN_ST7789_RST,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, true)); /* IPS */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel_handle, true));      /* ландшафт */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, LCD_DEF_MX != 0, LCD_DEF_MY != 0));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle, s_offset_x, s_offset_y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    /* ── 4. Підсвітка: LEDC 5 кГц (поза чутним діапазоном), 10 біт duty ── */
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BSP_PIN_ST7789_BLK,
        .duty = 512,   /* старт ~50% */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));

    /* ── 5. LVGL: PARTIAL, один draw-буфер у PSRAM + подвійний bounce ── */
    lv_init();
    s_draw_buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    s_bounce[0] = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_bounce[1] = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_draw_buf || !s_bounce[0] || !s_bounce[1]) {
        ESP_LOGE(TAG, "Не вдалося виділити draw/bounce буфери");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL буфери: draw=%p (PSRAM); bounce0=%p bounce1=%p (internal, %d B each)",
             s_draw_buf, s_bounce[0], s_bounce[1], CHUNK_SIZE);

    s_disp = lv_display_create(LCD_LOG_W, LCD_LOG_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, CHUNK_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* ── 6. Tick 5 мс ── */
    const esp_timer_create_args_t tick_args = { .callback = &tick_timer_cb, .name = "lv_tick" };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 5000));

    /* ── 7. Екрани та початкова навігація ── */
    build_screens();
    navigate(UI_SCREEN_MAIN);

    /* ── 8. Підписка на шину system ── */
    s_ui_queue = xQueueCreate(10, sizeof(ui_event_t));
    if (!s_ui_queue) return ESP_ERR_NO_MEM;
    system_register_queue(s_ui_queue);

    ESP_LOGI(TAG, "UI init done");
    return ESP_OK;
}

/* ── Задача ui: event-driven рендер + fallback 100 мс + самомір CPU ── */
void ui_task_entry(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_update = last_wake;
    int64_t window_start_us = esp_timer_get_time();
    int64_t busy_us = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();

        ui_event_t ui_evt;
        bool event_received = false;
        while (xQueueReceive(s_ui_queue, &ui_evt, 0) == pdTRUE) {
            event_received = true;
            if (ui_evt.id == SYSTEM_EVENT_INPUT) {
                handle_input(&ui_evt.input);
            }
            /* SYSTEM_EVT_AUDIO_STATE / SYSTEM_EVENT_SOURCE_CHANGED:
             * payload немає — стан перечитується геттерами в update_ui_state() */
        }
        TickType_t now = xTaskGetTickCount();
        if (event_received || (now - last_update) >= pdMS_TO_TICKS(100)) {
            update_ui_state();
            last_update = now;
        }
        lv_timer_handler();

        busy_us += esp_timer_get_time() - t0;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5));

        /* Раз на 5 с: лог CPU ui + internal heap (контроль критеріїв 6 і 7) */
        int64_t now_us = esp_timer_get_time();
        if (now_us - window_start_us >= CPU_WINDOW_US) {
            uint32_t cpu_percent = (uint32_t)((busy_us * 100) / (now_us - window_start_us));
            ESP_LOGI(TAG, "ui cpu=%lu%% core 0 (вікно 5 с), internal heap=%u B",
                     (unsigned long)cpu_percent,
                     (unsigned)esp_get_free_internal_heap_size());
            window_start_us = now_us;
            busy_us = 0;
        }
    }
}

/* ── Допоміжна: намалювати одну смугу рядків [ys..ye] з тимчасового буфера ── */
static void test_draw_band(uint16_t *band_buf, int ys, int ye)
{
    if (xSemaphoreTake(s_panel_mtx, portMAX_DELAY) == pdTRUE) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, ys, LCD_LOG_W, ye + 1, band_buf);
        xSemaphoreGive(s_panel_mtx);
    }
}

/* ── Dev-команда lcdtest: смугова, без full-screen алокацій ── */
int ui_cmd_lcdtest(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lcdtest [fill|gradient|corners|border|offset <x> <y>|orient <swap> <mx> <my>]\n");
        return 1;
    }

    if (strcmp(argv[1], "offset") == 0 && argc == 4) {
        s_offset_x = atoi(argv[2]);
        s_offset_y = atoi(argv[3]);
        if (xSemaphoreTake(s_panel_mtx, portMAX_DELAY) == pdTRUE) {
            esp_lcd_panel_set_gap(s_panel_handle, s_offset_x, s_offset_y);
            xSemaphoreGive(s_panel_mtx);
        }
        printf("Offsets set to %d, %d\n", s_offset_x, s_offset_y);
        return 0;
    }
    if (strcmp(argv[1], "orient") == 0 && argc == 5) {
        int swap = atoi(argv[2]), mx = atoi(argv[3]), my = atoi(argv[4]);
        if (xSemaphoreTake(s_panel_mtx, portMAX_DELAY) == pdTRUE) {
            esp_lcd_panel_swap_xy(s_panel_handle, swap != 0);
            esp_lcd_panel_mirror(s_panel_handle, mx != 0, my != 0);
            xSemaphoreGive(s_panel_mtx);
        }
        printf("Orient: swap=%d mirror=(%d,%d)\n", swap, mx, my);
        return 0;
    }

    uint16_t *band = heap_caps_malloc((size_t)LCD_LOG_W * TEST_ROWS * 2,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!band) {
        printf("lcdtest: no mem for %dx%d band\n", LCD_LOG_W, TEST_ROWS);
        return 1;
    }

    const int passes = (strcmp(argv[1], "fill") == 0) ? 3 : 1;
    for (int p = 0; p < passes; p++) {
        for (int ys = 0; ys < LCD_LOG_H; ys += TEST_ROWS) {
            int ye = ys + TEST_ROWS - 1;
            if (ye >= LCD_LOG_H) ye = LCD_LOG_H - 1;

            for (int y = ys; y <= ye; y++) {
                uint16_t *row = band + (size_t)(y - ys) * LCD_LOG_W;
                for (int x = 0; x < LCD_LOG_W; x++) {
                    uint16_t c = 0;
                    if (strcmp(argv[1], "fill") == 0) {
                        const uint16_t cols[3] = { 0xF800, 0x07E0, 0x001F };
                        c = cols[p];
                    } else if (strcmp(argv[1], "gradient") == 0) {
                        uint8_t r = (x * 31) / LCD_LOG_W;
                        uint8_t g = (y * 63) / LCD_LOG_H;
                        uint8_t b = ((x + y) * 31) / (LCD_LOG_W + LCD_LOG_H);
                        c = (uint16_t)((r << 11) | (g << 5) | b);
                    } else if (strcmp(argv[1], "border") == 0) {
                        bool edge = (y == 0 || y == LCD_LOG_H - 1 || x == 0 || x == LCD_LOG_W - 1);
                        c = edge ? 0xFFFF : 0x0000;
                    } else if (strcmp(argv[1], "corners") == 0) {
                        c = 0x0000;
                        if (y == 0 && x == 0) c = 0xF800;
                        if (y == 0 && x == LCD_LOG_W - 1) c = 0x07E0;
                        if (y == LCD_LOG_H - 1 && x == 0) c = 0x001F;
                        if (y == LCD_LOG_H - 1 && x == LCD_LOG_W - 1) c = 0xFFFF;
                    } else {
                        printf("Usage: lcdtest [fill|gradient|corners|border|offset <x> <y>|orient <swap> <mx> <my>]\n");
                        heap_caps_free(band);
                        return 1;
                    }
                    row[x] = c;
                }
            }
            test_draw_band(band, ys, ye);
        }
        if (passes > 1) vTaskDelay(pdMS_TO_TICKS(500));
    }

    heap_caps_free(band);
    return 0;
}

/* ── Побудова 5 сторінок + Menu (ландшафт 320x170) ── */
static void build_screens(void)
{
    for (int i = 0; i < 6; i++) {
        s_scr[i] = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_scr[i], lv_color_black(), 0);
        lv_obj_set_style_text_color(s_scr[i], lv_color_white(), 0);
        lv_obj_set_style_pad_all(s_scr[i], 0, 0);
    }

    /* Main: джерело, гучність, mute-індикатор, пресет */
    s_lbl_main_src = lv_label_create(s_scr[UI_SCREEN_MAIN]); apply_font(s_lbl_main_src);
    lv_obj_align(s_lbl_main_src, LV_ALIGN_TOP_LEFT, 10, 6);
    s_lbl_main_vol = lv_label_create(s_scr[UI_SCREEN_MAIN]); apply_font(s_lbl_main_vol);
    lv_obj_align(s_lbl_main_vol, LV_ALIGN_TOP_RIGHT, -10, 6);
    s_lbl_main_mute = lv_label_create(s_scr[UI_SCREEN_MAIN]); apply_font(s_lbl_main_mute);
    lv_obj_align(s_lbl_main_mute, LV_ALIGN_TOP_MID, 0, 6);
    s_lbl_main_preset = lv_label_create(s_scr[UI_SCREEN_MAIN]); apply_font(s_lbl_main_preset);
    lv_obj_align(s_lbl_main_preset, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* Menu: 4 пункти, крок 36 px */
    const char *menu_items[] = {"1. Джерело", "2. Еквалайзер", "3. Налаштування", "4. Інфо"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl = lv_label_create(s_scr[UI_SCREEN_MENU]); apply_font(lbl);
        lv_label_set_text(lbl, menu_items[i]);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 12 + i * 36);
    }

    /* Source: 4 пункти з UA-назвами, крок 36 px */
    const char *src_names[] = {"TV BOX", "Комп'ютер", "Bluetooth", "AUX"};
    for (int i = 0; i < 4; i++) {
        s_lbl_src_items[i] = lv_label_create(s_scr[UI_SCREEN_SOURCE]); apply_font(s_lbl_src_items[i]);
        lv_label_set_text(s_lbl_src_items[i], src_names[i]);
        lv_obj_align(s_lbl_src_items[i], LV_ALIGN_TOP_MID, 0, 12 + i * 36);
    }

    /* EQ: 10 смуг = 2 стовпці x 5 рядків (крок 30 px), частота: значення */
    const char *eq_freqs[] = {"31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
    for (int i = 0; i < 10; i++) {
        s_lbl_eq_bands[i] = lv_label_create(s_scr[UI_SCREEN_EQ]); apply_font(s_lbl_eq_bands[i]);
        char txt[16]; snprintf(txt, sizeof(txt), "%s: 0", eq_freqs[i]);
        lv_label_set_text(s_lbl_eq_bands[i], txt);
        lv_obj_set_width(s_lbl_eq_bands[i], 150);
        int col = i / 5;          /* 0 = лівий стовпець, 1 = правий */
        int row = i % 5;
        lv_obj_align(s_lbl_eq_bands[i], col == 0 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT,
                     col == 0 ? 8 : -8, 8 + row * 30);
    }

    /* Settings: яскравість + сірі заглушки trim/IR/Wi-Fi, крок 36 px */
    const char *set_items[] = {"Яскравість", "Trim (stub)", "IR (stub)", "Wi-Fi (stub)"};
    for (int i = 0; i < 4; i++) {
        s_lbl_settings_items[i] = lv_label_create(s_scr[UI_SCREEN_SETTINGS]); apply_font(s_lbl_settings_items[i]);
        lv_label_set_text(s_lbl_settings_items[i], set_items[i]);
        if (i > 0) lv_obj_set_style_text_color(s_lbl_settings_items[i], lv_color_make(128, 128, 128), 0);
        lv_obj_align(s_lbl_settings_items[i], LV_ALIGN_TOP_MID, 0, 12 + i * 36);
    }

    /* Info: версія FW, heap, uptime */
    s_lbl_info_fw = lv_label_create(s_scr[UI_SCREEN_INFO]); apply_font(s_lbl_info_fw);
    lv_label_set_text_fmt(s_lbl_info_fw, "FW: v%d.%d.%d",
                          FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH);
    lv_obj_align(s_lbl_info_fw, LV_ALIGN_TOP_LEFT, 10, 12);
    s_lbl_info_heap = lv_label_create(s_scr[UI_SCREEN_INFO]); apply_font(s_lbl_info_heap);
    lv_label_set_text(s_lbl_info_heap, "Heap: ...");
    lv_obj_align(s_lbl_info_heap, LV_ALIGN_TOP_LEFT, 10, 50);
    s_lbl_info_uptime = lv_label_create(s_scr[UI_SCREEN_INFO]); apply_font(s_lbl_info_uptime);
    lv_label_set_text(s_lbl_info_uptime, "Up: 0s");
    lv_obj_align(s_lbl_info_uptime, LV_ALIGN_TOP_LEFT, 10, 88);
}

static void navigate(ui_screen_t screen)
{
    s_current_screen = screen;
    s_focus_idx = 0;
    lv_screen_load(s_scr[screen]);
    apply_focus();
    update_ui_state();
}

static void apply_focus(void)
{
    if (s_current_screen == UI_SCREEN_MENU || s_current_screen == UI_SCREEN_SOURCE) {
        lv_obj_t *scr = s_scr[s_current_screen];
        uint32_t cnt = lv_obj_get_child_count(scr);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_set_style_text_color(lv_obj_get_child(scr, i), lv_color_white(), 0);
        }
        if ((uint32_t)s_focus_idx < cnt) {
            lv_obj_set_style_text_color(lv_obj_get_child(scr, s_focus_idx), lv_color_make(0, 255, 255), 0);
        }
    } else if (s_current_screen == UI_SCREEN_EQ) {
        for (int i = 0; i < 10; i++) {
            lv_obj_set_style_text_color(s_lbl_eq_bands[i],
                                        i == s_focus_idx ? lv_color_make(0, 255, 255) : lv_color_white(), 0);
        }
    } else if (s_current_screen == UI_SCREEN_SETTINGS) {
        lv_obj_t *scr = s_scr[UI_SCREEN_SETTINGS];
        uint32_t cnt = lv_obj_get_child_count(scr);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_set_style_text_color(lv_obj_get_child(scr, i),
                                        i == 0 ? lv_color_white() : lv_color_make(128, 128, 128), 0);
        }
        if ((uint32_t)s_focus_idx < cnt) {
            lv_obj_set_style_text_color(lv_obj_get_child(scr, s_focus_idx), lv_color_make(0, 255, 255), 0);
        }
    }
}

/* Перечитування стану моделі геттерами (без payload у подіях) */
static void update_ui_state(void)
{
    float vol = audio_get_volume_db();
    bsp_audio_source_t src = tda7318_get_source();
    bool mute = audio_get_mute();
    audio_eq_preset_t preset = audio_get_preset();
    float eq[10];
    audio_get_eq(eq);

    const char *src_names[] = {"TV BOX", "Комп'ютер", "Bluetooth", "AUX"};
    lv_label_set_text_fmt(s_lbl_main_src, "%s", src < BSP_AUDIO_SOURCE_MAX ? src_names[src] : "?");
    lv_label_set_text_fmt(s_lbl_main_vol, "%.0f dB", (double)vol);
    lv_label_set_text(s_lbl_main_mute, mute ? "MUTE" : "");

    const char *preset_names[] = {"Flat", "Rock", "Pop", "Jazz", "Classic", "Vocal", "Night", "Custom"};
    lv_label_set_text_fmt(s_lbl_main_preset, "%s",
                          preset < AUDIO_EQ_PRESET_MAX ? preset_names[preset] : "Custom");

    const char *eq_freqs[] = {"31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
    for (int i = 0; i < 10; i++) {
        char txt[16];
        snprintf(txt, sizeof(txt), "%s: %.0f", eq_freqs[i], (double)eq[i]);
        lv_label_set_text(s_lbl_eq_bands[i], txt);
    }

    lv_label_set_text_fmt(s_lbl_info_heap, "Heap: %u", (unsigned)esp_get_free_heap_size());
    lv_label_set_text_fmt(s_lbl_info_uptime, "Up: %llds",
                          (long long)(esp_timer_get_time() / 1000000));
}

/* ── Навігація за картою керування v2.2.5 ── */
static void handle_input(const input_event_t *evt)
{
    switch (s_current_screen) {
    case UI_SCREEN_MAIN:
        if (evt->source == INPUT_SRC_ENC_STEP && evt->action == INPUT_ACTION_STEP) {
            float vol = audio_get_volume_db() + (float)evt->arg;   /* ENC=гучність */
            if (vol > 0.0f) vol = 0.0f;
            if (vol < -90.0f) vol = -90.0f;
            audio_set_volume_db(vol);
        } else if (evt->source == INPUT_SRC_BTN_LEFT && evt->action == INPUT_ACTION_SHORT) {
            bsp_audio_source_t src = tda7318_get_source();          /* LEFT=джерело- */
            tda7318_switch_source(src == 0 ? BSP_AUDIO_SOURCE_MAX - 1 : src - 1);
        } else if (evt->source == INPUT_SRC_BTN_RIGHT && evt->action == INPUT_ACTION_SHORT) {
            bsp_audio_source_t src = tda7318_get_source();          /* RIGHT=джерело+ */
            tda7318_switch_source((bsp_audio_source_t)((src + 1) % BSP_AUDIO_SOURCE_MAX));
        } else if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_SHORT) {
            navigate(UI_SCREEN_MENU);                               /* OK=меню */
        }
        break;
    case UI_SCREEN_MENU:
        if (evt->source == INPUT_SRC_ENC_STEP && evt->action == INPUT_ACTION_STEP) {
            s_focus_idx += evt->arg;                                /* ENC=фокус */
            if (s_focus_idx < 0) s_focus_idx = 0;
            if (s_focus_idx > 3) s_focus_idx = 3;
            apply_focus();
        } else if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_SHORT) {
            const ui_screen_t targets[4] = { UI_SCREEN_SOURCE, UI_SCREEN_EQ,
                                             UI_SCREEN_SETTINGS, UI_SCREEN_INFO };
            navigate(targets[s_focus_idx]);                         /* OK=вибір */
        } else if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_LONG) {
            navigate(UI_SCREEN_MAIN);                               /* LONG OK=назад */
        }
        break;
    case UI_SCREEN_SOURCE:
        if (evt->source == INPUT_SRC_ENC_STEP && evt->action == INPUT_ACTION_STEP) {
            s_focus_idx += evt->arg;
            if (s_focus_idx < 0) s_focus_idx = 0;
            if (s_focus_idx >= BSP_AUDIO_SOURCE_MAX) s_focus_idx = BSP_AUDIO_SOURCE_MAX - 1;
            apply_focus();
        } else if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_SHORT) {
            tda7318_switch_source((bsp_audio_source_t)s_focus_idx);
            navigate(UI_SCREEN_MAIN);
        } else if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_LONG) {
            navigate(UI_SCREEN_MENU);
        }
        break;
    case UI_SCREEN_EQ:
        if (evt->source == INPUT_SRC_ENC_STEP && evt->action == INPUT_ACTION_STEP) {
            s_focus_idx += evt->arg;                                /* ENC=смуга */
            if (s_focus_idx < 0) s_focus_idx = 0;
            if (s_focus_idx >= 10) s_focus_idx = 9;
            apply_focus();
        } else if ((evt->source == INPUT_SRC_BTN_LEFT || evt->source == INPUT_SRC_BTN_RIGHT) &&
                   evt->action == INPUT_ACTION_SHORT) {
            float eq[10];
            audio_get_eq(eq);                                       /* LEFT/RIGHT=значення */
            float delta = (evt->source == INPUT_SRC_BTN_RIGHT) ? 1.0f : -1.0f;
            audio_set_eq_band((uint8_t)s_focus_idx, eq[s_focus_idx] + delta);
        } else if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_LONG) {
            navigate(UI_SCREEN_MENU);
        }
        break;
    case UI_SCREEN_SETTINGS:
        if (evt->source == INPUT_SRC_ENC_STEP && evt->action == INPUT_ACTION_STEP) {
            s_focus_idx += evt->arg;
            if (s_focus_idx < 0) s_focus_idx = 0;
            if (s_focus_idx > 3) s_focus_idx = 3;
            apply_focus();
        } else if (evt->source == INPUT_SRC_BTN_LEFT && evt->action == INPUT_ACTION_SHORT && s_focus_idx == 0) {
            uint32_t duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            if (duty >= 51) {   /* -5% */
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty - 51);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            }
        } else if (evt->source == INPUT_SRC_BTN_RIGHT && evt->action == INPUT_ACTION_SHORT && s_focus_idx == 0) {
            uint32_t duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            if (duty <= 1023 - 51) {   /* +5% */
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty + 51);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            }
        } else if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_LONG) {
            navigate(UI_SCREEN_MENU);
        }
        break;
    case UI_SCREEN_INFO:
        if (evt->source == INPUT_SRC_BTN_OK && evt->action == INPUT_ACTION_LONG) {
            navigate(UI_SCREEN_MENU);
        }
        break;
    default:
        break;
    }
}