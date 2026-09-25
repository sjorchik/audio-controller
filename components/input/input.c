#include "input.h"
#include "button_sm.h"
#include "bsp.h"
#include "system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"

#ifdef CONFIG_APP_DEV_CONSOLE
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#endif

static const char *TAG = "input";

/* ────────────────────────────────────────────────────────────────
 * Конфігурація пінів
 * ──────────────────────────────────────────────────────────────── */

typedef struct {
    int pin;
    input_source_t source;
    bool repeat_allowed;
} btn_cfg_t;

/* Щільний масив: індекс 0..BUTTONS_COUNT-1 синхронний зі sm[].
 * (Раніше sparse-індексація за enum пропускала ENC_BTN у циклі.) */
#define BUTTONS_COUNT 7 /* 6 кнопок + кнопка енкодера */

static const btn_cfg_t s_btn_cfg[BUTTONS_COUNT] = {
    { BSP_PIN_BTN_POWER,   INPUT_SRC_BTN_POWER, false },
    { BSP_PIN_BTN_UP,      INPUT_SRC_BTN_UP,    true  },
    { BSP_PIN_BTN_DOWN,    INPUT_SRC_BTN_DOWN,  true  },
    { BSP_PIN_BTN_LEFT,    INPUT_SRC_BTN_LEFT,  true  },
    { BSP_PIN_BTN_RIGHT,   INPUT_SRC_BTN_RIGHT, true  },
    { BSP_PIN_BTN_OK,      INPUT_SRC_BTN_OK,    false },
    { BSP_PIN_ENCODER_BTN, INPUT_SRC_ENC_BTN,   false },
};

/* Період опитування рівнів кнопок задачею (мс) */
#define BUTTON_POLL_MS 4

/* Імпульсів PCNT на один детент енкодера.
 * ВАЖЛИВО: це параметр МОДЕЛІ енкодера, а не платформи.
 * Процедура для production-зразка:
 *   1) dev-збірка на цільовому енкодері: enccal 20 -> enccal stop;
 *   2) отримане ціле значення записати сюди як default;
 *   3) у prod-збірці enccal відсутній (CONFIG_APP_DEV_CONSOLE=n),
 *      тому default має бути коректним для встановленої моделі. */
#define ENC_COUNTS_PER_STEP_DEFAULT 2

/* Апаратний glitch-фільтр, нс.
 * ОБМЕЖЕННЯ HW: регістр фільтра ~1023 такти PCNT-годинника
 * (APB 80 MHz => максимум ~12.7 мкс). 10 000 нс = 800 тактів @80MHz
 * і 400 тактів @40MHz — безпечно в обох конфігураціях. */
#define PCNT_GLITCH_FILTER_NS 10000

/* Симетричні межі лічильника PCNT.
 * ОБОВ'ЯЗКОВО симетричні: при low_limit < 0 обертання CCW одразу
 * впирається в межу і переноситься у high_limit (wrap), через що
 * напрямок "втрачається", а мікро-реверси на старті з'їдають імпульси.
 * За опитування кожні 8 мс |дельта| << 1000, тому wrap виключений. */
#define PCNT_COUNT_LIMIT 1000

/* ────────────────────────────────────────────────────────────────
 * ISR та черга подій
 * ──────────────────────────────────────────────────────────────── */

typedef struct {
    int pin;
    int level;
    int64_t timestamp_us;
} isr_event_t;

static QueueHandle_t s_isr_queue = NULL;
static pcnt_unit_handle_t s_pcnt_unit = NULL;

/* Поточне CPD (імпульсів на детент); змінюється dev-командою enccal */
static int s_cpd = ENC_COUNTS_PER_STEP_DEFAULT;

#ifdef CONFIG_APP_DEV_CONSOLE
/* Калібрування енкодера: цільова кількість детентів і накопичені імпульси */
static volatile int s_enccal_target = 0;
static volatile int s_enccal_pulses = 0;

/* Толерантний розбір: heartbeat-лог може влізти всередину рядка
 * ("enccal 10I (411475) ..."), тому порівнюємо префікси і використовуємо
 * strtol замість вимоги argc == 2. */
static int cmd_enccal(int argc, char **argv)
{
    if (argc >= 2) {
        const char *a = argv[1];

        if (strncmp(a, "stop", 4) == 0) {
            if (s_enccal_target <= 0) {
                printf("enccal: not armed (usage: enccal <detents>)\n");
                return 1;
            }
            int pulses = (s_enccal_pulses < 0) ? -s_enccal_pulses : s_enccal_pulses;
            int cpd = (pulses + s_enccal_target / 2) / s_enccal_target;
            if (cpd < 1) cpd = 1;
            printf("enccal: pulses=%d (signed %d), detents=%d => %d pulses/detent\n",
                   pulses, (int)s_enccal_pulses, s_enccal_target, cpd);
            s_cpd = cpd;
            printf("enccal: ENC_COUNTS_PER_STEP set to %d (runtime, до reboot)\n", s_cpd);
            s_enccal_target = 0;
            s_enccal_pulses = 0;
            return 0;
        }

        if (strncmp(a, "off", 3) == 0) {
            s_enccal_target = 0;
            s_enccal_pulses = 0;
            printf("enccal: cancelled\n");
            return 0;
        }

        char *end = NULL;
        long d = strtol(a, &end, 10);
        if (end != a && d > 0 && d <= 1000) {
            s_enccal_pulses = 0;
            s_enccal_target = (int)d;
            printf("enccal: armed for %d detents; поверніть енкодер В ОДНУ СТОРОНУ,\n"
                   "потім введіть 'enccal stop'\n", (int)d);
            return 0;
        }
    }
    printf("Usage: enccal <detents> | enccal stop | enccal off\n");
    return 1;
}
#endif /* CONFIG_APP_DEV_CONSOLE */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    int pin = (int)(intptr_t)arg;
    isr_event_t evt = {
        .pin = pin,
        .level = gpio_get_level(pin),
        .timestamp_us = esp_timer_get_time(),
    };
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_isr_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* ────────────────────────────────────────────────────────────────
 * Ініціалізація апаратної частини
 * ──────────────────────────────────────────────────────────────── */

static esp_err_t input_gpio_init(void)
{
    s_isr_queue = xQueueCreate(32, sizeof(isr_event_t));
    if (!s_isr_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    for (int i = 0; i < BUTTONS_COUNT; i++) {
        io_conf.pin_bit_mask = (1ULL << s_btn_cfg[i].pin);
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        ESP_ERROR_CHECK(gpio_isr_handler_add(s_btn_cfg[i].pin, gpio_isr_handler,
                                             (void *)(intptr_t)s_btn_cfg[i].pin));
    }

    return ESP_OK;
}

static esp_err_t input_pcnt_init(void)
{
    /* ВИПРАВЛЕНО: симетричні межі замість (32767, -1).
     * За старих меж CCW-обертання переносило лічильник у high_limit
     * (wrap на межі) — напрямок не працював, а мікро-реверси на старті
     * з'їдали імпульси (pulses=18 замість 20 у enccal). */
    pcnt_unit_config_t unit_config = {
        .high_limit = PCNT_COUNT_LIMIT,
        .low_limit = -PCNT_COUNT_LIMIT,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &s_pcnt_unit));

    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = PCNT_GLITCH_FILTER_NS, /* 10 мкс, в межах HW-ліміту */
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_config));

    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = BSP_PIN_ENCODER_A,
        .level_gpio_num = BSP_PIN_ENCODER_B,
    };
    pcnt_channel_handle_t chan_a;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_a_config, &chan_a));

    /* Квадратурне 2x-декодування: обидва фронти A, знак за рівнем B */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, /* Коли B=0 */
        PCNT_CHANNEL_EDGE_ACTION_INCREASE  /* Коли B=1 */
    ));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE
    ));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));

    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────
 * Хелпери публікації подій
 * ──────────────────────────────────────────────────────────────── */

static void post_event(input_source_t source, input_action_t action, int8_t arg)
{
    input_event_t evt = {
        .source = source,
        .action = action,
        .arg = arg,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    esp_err_t ret = system_post(SYSTEM_EVENT_INPUT, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "system_post failed: %s", esp_err_to_name(ret));
    }
}

static void emit_btn_action(input_source_t source, btn_action_t action)
{
    switch (action) {
    case BTN_ACTION_SHORT:  post_event(source, INPUT_ACTION_SHORT, 0);  break;
    case BTN_ACTION_LONG:   post_event(source, INPUT_ACTION_LONG, 0);   break;
    case BTN_ACTION_REPEAT: post_event(source, INPUT_ACTION_REPEAT, 0); break;
    default: break;
    }
}

/* ────────────────────────────────────────────────────────────────
 * Input-задача
 * ──────────────────────────────────────────────────────────────── */

void input_task_entry(void *arg)
{
    (void)arg;
    button_sm_t sm[BUTTONS_COUNT];
    for (int i = 0; i < BUTTONS_COUNT; i++) {
        button_sm_init(&sm[i], s_btn_cfg[i].repeat_allowed);
    }

    TickType_t last_btn_tick = xTaskGetTickCount();
    TickType_t last_pcnt_tick = last_btn_tick;
    int enc_accum = 0; /* залишок імпульсів між опитуваннями */
    isr_event_t isr_evt;

    while (1) {
        /* 1. Фронти від ISR: точний таймстамп фізичного фронту */
        while (xQueueReceive(s_isr_queue, &isr_evt, 0) == pdTRUE) {
            int btn_idx = -1;
            for (int i = 0; i < BUTTONS_COUNT; i++) {
                if (s_btn_cfg[i].pin == isr_evt.pin) {
                    btn_idx = i;
                    break;
                }
            }
            if (btn_idx < 0) continue;

            bool is_pressed = (isr_evt.level == BSP_INPUT_ACTIVE_LEVEL);
            uint32_t ts_ms = (uint32_t)(isr_evt.timestamp_us / 1000);
            emit_btn_action(s_btn_cfg[btn_idx].source,
                            button_sm_update(&sm[btn_idx], is_pressed, ts_ms));
        }

        /* 2. Періодичні семпли рівнів: таймери утримання */
        TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_btn_tick) >= pdMS_TO_TICKS(BUTTON_POLL_MS)) {
            last_btn_tick = now_tick;
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            for (int i = 0; i < BUTTONS_COUNT; i++) {
                bool lvl = (gpio_get_level(s_btn_cfg[i].pin) == BSP_INPUT_ACTIVE_LEVEL);
                emit_btn_action(s_btn_cfg[i].source,
                                button_sm_update(&sm[i], lvl, now_ms));
            }
        }

        /* 3. Енкодер: дельта PCNT кожні 8 мс + акумулятор залишку.
         *    Кількість подій STEP = накопичені імпульси / s_cpd,
         *    тому флік на N детентів дає рівно N подій. */
        if ((now_tick - last_pcnt_tick) >= pdMS_TO_TICKS(8)) {
            last_pcnt_tick = now_tick;
            int count = 0;
            pcnt_unit_get_count(s_pcnt_unit, &count);
            pcnt_unit_clear_count(s_pcnt_unit);

#ifdef CONFIG_APP_DEV_CONSOLE
            if (s_enccal_target > 0) {
                s_enccal_pulses += count;
            }
#endif

            enc_accum += count;
            int steps = enc_accum / s_cpd;
            enc_accum -= steps * s_cpd;

            int8_t dir = (steps > 0) ? 1 : ((steps < 0) ? -1 : 0);
            int abs_steps = (steps >= 0) ? steps : -steps;
            for (int s = 0; s < abs_steps; s++) {
                post_event(INPUT_SRC_ENC_STEP, INPUT_ACTION_STEP, dir);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ────────────────────────────────────────────────────────────────
 * Публічне API
 * ──────────────────────────────────────────────────────────────── */

esp_err_t input_init(void)
{
    esp_err_t ret = input_gpio_init();
    if (ret != ESP_OK) return ret;

    ret = input_pcnt_init();
    if (ret != ESP_OK) return ret;

#ifdef CONFIG_APP_DEV_CONSOLE
    esp_console_cmd_t enccal_cmd = {
        .command = "enccal",
        .help = "Калібрування енкодера: enccal <detents> | enccal stop | enccal off",
        .func = &cmd_enccal,
    };
    esp_err_t ce = esp_console_cmd_register(&enccal_cmd);
    if (ce != ESP_OK) {
        ESP_LOGW(TAG, "enccal register failed: %s", esp_err_to_name(ce));
    }
#endif

    ESP_LOGI(TAG, "input_init: PCNT + GPIO SM initialized");
    return ESP_OK;
}