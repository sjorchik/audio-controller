#include "ir.h"
#include "bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "audio.h"
#include "tda7318.h"
#include "system.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ir";

/* Тайм-база RC5: пів-біт 889 мкс, біт 1778 мкс.
 * Вікна ТРИВАЛОСТЕЙ — рівнезалежні, бо VS1838B дає асиметричний duty:
 * Low (несуча) довший, High (пауза) коротший; сума пари L+H = 1778 мкс.
 * Виміряно на реальних дампах: L single 971..1234, L double 1880..2184,
 * H single 556..843, H double 1511..1686. */
#define IR_L_SINGLE_MIN 800
#define IR_L_SINGLE_MAX 1400
#define IR_L_DOUBLE_MIN 1600
#define IR_L_DOUBLE_MAX 2800
#define IR_H_SINGLE_MIN 450
#define IR_H_SINGLE_MAX 950
#define IR_H_DOUBLE_MIN 1000
#define IR_H_DOUBLE_MAX 2000

#define IR_RX_SYMBOLS 64

static ir_stats_t s_stats = {0};
static esp_err_t s_rmt_err = ESP_OK;
ir_pair_t s_ir_map[32];
uint16_t s_ir_map_count = 0;

volatile ir_action_t s_learn_action = IR_ACTION_NONE;
volatile bool s_learn_success = false;
volatile uint8_t s_learn_system = 0;
volatile uint8_t s_learn_command = 0;

typedef struct {
    uint8_t system;
    uint8_t command;
    uint8_t last_toggle;
    TickType_t last_event_tick;
} ir_last_state_t;

static ir_last_state_t s_last_state = {0};

/* Буфер кадру для передачі з ISR-колбеку у задачу декодера. */
typedef struct {
    uint16_t num_symbols;
    rmt_symbol_word_t symbols[IR_RX_SYMBOLS];
} ir_frame_buf_t;

static QueueHandle_t s_frame_q = NULL;
static rmt_channel_handle_t s_rx_chan = NULL;
static rmt_symbol_word_t s_rx_buf[IR_RX_SYMBOLS];
static ir_frame_buf_t s_last_block;
static bool s_have_last = false;
static rmt_receive_config_t s_rx_cfg = {
    /* Апаратний фільтр глюків обмежений 8-бітним порогом у тіках APB 80 MHz
     * (<= 255 тіків = 3187 нс), тому ставимо 1 мкс. Усе коротше за вікна
     * допуску додатково відсікається програмно у декодері. */
    .signal_range_min_ns = 1000,
    .signal_range_max_ns = 4445 * 1000,   /* idle > 4.445 мс = кінець фрейму */
};

const char* ir_action_to_string(ir_action_t action) {
    switch (action) {
        case IR_ACTION_VOL_UP: return "vol_up";
        case IR_ACTION_VOL_DOWN: return "vol_down";
        case IR_ACTION_MUTE_TOGGLE: return "mute";
        case IR_ACTION_SRC_NEXT: return "src_next";
        case IR_ACTION_SRC_PREV: return "src_prev";
        case IR_ACTION_POWER_TOGGLE: return "power";
        default: return "none";
    }
}

ir_action_t ir_action_from_string(const char *str) {
    if (strcmp(str, "vol_up") == 0) return IR_ACTION_VOL_UP;
    if (strcmp(str, "vol_down") == 0) return IR_ACTION_VOL_DOWN;
    if (strcmp(str, "mute") == 0) return IR_ACTION_MUTE_TOGGLE;
    if (strcmp(str, "src_next") == 0) return IR_ACTION_SRC_NEXT;
    if (strcmp(str, "src_prev") == 0) return IR_ACTION_SRC_PREV;
    if (strcmp(str, "power") == 0) return IR_ACTION_POWER_TOGGLE;
    return IR_ACTION_NONE;
}

static ir_action_t ir_get_mapped_action(uint8_t system, uint8_t command) {
    for (int i = 0; i < s_ir_map_count; i++) {
        if (s_ir_map[i].system == system && s_ir_map[i].command == command) {
            return s_ir_map[i].action;
        }
    }
    return IR_ACTION_NONE;
}

/* Видалення прив'язки за ключем (system, command) із збереженням у NVS.
 * Використовується dev-командою irunlearn. */
esp_err_t ir_map_remove(uint8_t system, uint8_t command) {
    for (int i = 0; i < s_ir_map_count; i++) {
        if (s_ir_map[i].system == system && s_ir_map[i].command == command) {
            for (int j = i; j < s_ir_map_count - 1; j++) {
                s_ir_map[j] = s_ir_map[j + 1];
            }
            s_ir_map_count--;
            return settings_ir_map_save(s_ir_map, s_ir_map_count);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Повне очищення карти прив'язок із збереженням порожньої у NVS. */
esp_err_t ir_map_clear(void) {
    s_ir_map_count = 0;
    return settings_ir_map_save(s_ir_map, s_ir_map_count);
}

static void ir_execute_action(ir_action_t action) {
    switch (action) {
        case IR_ACTION_VOL_UP:
            audio_set_volume_db(audio_get_volume_db() + 1.0f);
            break;
        case IR_ACTION_VOL_DOWN:
            audio_set_volume_db(audio_get_volume_db() - 1.0f);
            break;
        case IR_ACTION_MUTE_TOGGLE:
            audio_set_mute(!audio_get_mute());
            break;
        case IR_ACTION_SRC_NEXT: {
            bsp_audio_source_t src = tda7318_get_source();
            src = (src + 1) % BSP_AUDIO_SOURCE_MAX;
            tda7318_switch_source(src);
            break;
        }
        case IR_ACTION_SRC_PREV: {
            bsp_audio_source_t src = tda7318_get_source();
            src = (src - 1 + BSP_AUDIO_SOURCE_MAX) % BSP_AUDIO_SOURCE_MAX;
            tda7318_switch_source(src);
            break;
        }
        case IR_ACTION_POWER_TOGGLE:
            system_post(SYSTEM_EVT_POWER_TOGGLE, NULL, 0);
            break;
        default:
            break;
    }
}

/* Кількість пів-бітів у імпульсі: 1 або 2, залежно від тривалості І рівня. */
static int ir_pulse_count(int level, int d) {
    if (level == 0) {
        if (d >= IR_L_SINGLE_MIN && d <= IR_L_SINGLE_MAX) return 1;
        if (d >= IR_L_DOUBLE_MIN && d <= IR_L_DOUBLE_MAX) return 2;
    } else {
        if (d >= IR_H_SINGLE_MIN && d <= IR_H_SINGLE_MAX) return 1;
        if (d >= IR_H_DOUBLE_MIN && d <= IR_H_DOUBLE_MAX) return 2;
    }
    return 0;
}

/* Розгортання RMT-символів у пів-біти Manchester.
 * Імпульси поза вікнами ДО першого валідного пів-біта пропускаються
 * (вхідний AGC-глюк приймача), перший невалідний ПІСЛЯ початку кадру = межа фрейму. */
static int ir_to_half_bits(const rmt_symbol_word_t *items, int num_items, bool *half_bits, int max_hb) {
    int nhb = 0;
    for (int i = 0; i < num_items; i++) {
        int ds[2] = { (int)items[i].duration0, (int)items[i].duration1 };
        int ls[2] = { (int)items[i].level0, (int)items[i].level1 };
        bool stop = false;
        for (int k = 0; k < 2 && !stop; k++) {
            if (ds[k] <= 0) continue;
            int count = ir_pulse_count(ls[k], ds[k]);
            if (count == 0) {
                if (nhb == 0) continue;   /* вхідне сміття: пропускаємо */
                stop = true;              /* межа фрейму */
                break;
            }
            for (int j = 0; j < count && nhb < max_hb; j++) {
                half_bits[nhb++] = (ls[k] == 1);
            }
        }
        if (stop) break;
    }
    return nhb;
}

/* Розбір npairs Manchester-пар, починаючи з пів-біта o, у frame[frame_o..].
 * one_first — рівень першої половини логічної 1 (визначає полярність).
 * Повертає 0, якщо всі пари валідні. */
static int ir_try_pairs(const bool *hb, int nhb, int o, int npairs, int one_first, int *frame, int frame_o) {
    for (int k = 0; k < npairs; k++) {
        int a = o + 2 * k;
        int b = a + 1;
        if (b >= nhb) return -1;
        if (hb[a] == hb[b]) return -1;
        frame[frame_o + k] = (hb[a] == (bool)one_first) ? 1 : 0;
    }
    return 0;
}

/* RC5/RC5X, Manchester, tick 889 мкс. Розбір ПОЛЯМИ (жодних магічних чисел):
 * S1=1, S2∈{0,1} (RC5X: S2=0 -> 7-й біт команди), toggle, system 5 біт, command 6 біт.
 * Режими синхронізації: повний фрейм (28 пів-бітів) та обрізані варіанти
 * (27/26 пів-бітів), коли RMT-захват втрачає перший/останній пів-біт.
 * Полярність приймача визначається автоматично у кожному режимі.
 * Toggle використовується ЛИШЕ для repeat-логіки поза декодером. */
static int decode_rc5(const rmt_symbol_word_t *items, int num_items, uint8_t *system, uint8_t *command, uint8_t *toggle) {
    bool hb[64];
    int nhb = ir_to_half_bits(items, num_items, hb, 64);
    int num_bits = nhb / 2;
    int frame[14];
    bool decoded = false;

    /* M0: повний фрейм, синхронізація по S1 (перший валідний біт = 1). */
    for (int s = 0; s <= num_bits - 14 && !decoded; s++) {
        if (hb[2*s] == hb[2*s+1]) continue;
        int one_first = hb[2*s];
        if (ir_try_pairs(hb, nhb, 2*s, 14, one_first, frame, 0) == 0) {
            decoded = true;
        }
    }

    /* M3: обрізані обидва кінці (26 пів-бітів): перший одинокий пів-біт =
     * друга половина S1 (з нього полярність), останній = перша половина bit13. */
    if (!decoded && nhb == 26) {
        int one_first = hb[0] ? 0 : 1;
        frame[0] = 1;
        if (ir_try_pairs(hb, nhb, 1, 12, one_first, frame, 1) == 0) {
            frame[13] = (hb[25] == (bool)one_first) ? 1 : 0;
            decoded = true;
        }
    }

    /* M1: обрізаний лише хвіст (27 пів-бітів): полярність по S1 з початку. */
    if (!decoded && nhb == 27 && hb[0] != hb[1]) {
        int one_first = hb[0];
        if (ir_try_pairs(hb, nhb, 0, 13, one_first, frame, 0) == 0 && frame[0] == 1) {
            frame[13] = (hb[26] == (bool)one_first) ? 1 : 0;
            decoded = true;
        }
    }

    /* M2: обрізана лише голова (27 пів-бітів): перший одинокий = 2-га половина S1. */
    if (!decoded && nhb == 27) {
        int one_first = hb[0] ? 0 : 1;
        frame[0] = 1;
        if (ir_try_pairs(hb, nhb, 1, 13, one_first, frame, 1) == 0) {
            decoded = true;
        }
    }

    if (!decoded) return -1;

    *toggle = (uint8_t)frame[2];
    uint8_t sys = 0;
    for (int i = 0; i < 5; i++) sys = (uint8_t)((sys << 1) | frame[3 + i]);
    uint8_t cmd = 0;
    for (int i = 0; i < 6; i++) cmd = (uint8_t)((cmd << 1) | frame[8 + i]);
    if (frame[1] == 0) cmd |= 0x40;   /* RC5X: S2=0 -> 7-й біт команди */

    *system = sys;
    *command = cmd;
    return 0;
}

/* Діагностика поза decode-path: друк останнього блоку (імпульси, пів-біти, розбір полів). */
void ir_dump_last_block(void) {
    if (!s_have_last) {
        printf("no block captured yet\n");
        return;
    }
    printf("last block: %u symbols\n", (unsigned)s_last_block.num_symbols);
    printf("pulses: ");
    for (int i = 0; i < s_last_block.num_symbols; i++) {
        const rmt_symbol_word_t *s = &s_last_block.symbols[i];
        if (s->duration0) printf("%c%u ", s->level0 ? 'H' : 'L', (unsigned)s->duration0);
        if (s->duration1) printf("%c%u ", s->level1 ? 'H' : 'L', (unsigned)s->duration1);
    }
    printf("\n");
    bool half_bits[64];
    int nhb = ir_to_half_bits(s_last_block.symbols, s_last_block.num_symbols, half_bits, 64);
    printf("half_bits(%d): ", nhb);
    for (int i = 0; i < nhb; i++) printf("%d", half_bits[i] ? 1 : 0);
    printf("\n");
    uint8_t sys = 0, cmd = 0, tog = 0;
    if (decode_rc5(s_last_block.symbols, s_last_block.num_symbols, &sys, &cmd, &tog) == 0) {
        printf("decoded: system=%u command=%u toggle=%u\n", sys, cmd, tog);
    } else {
        printf("decoded: FAIL\n");
    }
}

/* ISR-контекст: жодних логів. Копіюємо символи у чергу і перезапускаємо прийом. */
static bool IRAM_ATTR ir_rx_done_cb(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata, void *user_ctx) {
    ir_frame_buf_t fb;
    uint16_t n = (uint16_t)edata->num_symbols;
    if (n > IR_RX_SYMBOLS) n = IR_RX_SYMBOLS;
    fb.num_symbols = n;
    memcpy(fb.symbols, edata->received_symbols, (size_t)n * sizeof(rmt_symbol_word_t));

    BaseType_t yield = pdFALSE;
    if (xQueueSendFromISR(s_frame_q, &fb, &yield) != pdPASS) {
        s_stats.drops++;   /* черга повна: кадр втрачено */
    }
    rmt_receive(chan, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg);
    return (yield == pdTRUE);
}

static void ir_task(void *arg) {
    while (1) {
        ir_frame_buf_t fb;
        if (xQueueReceive(s_frame_q, &fb, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_stats.rx_blocks++;
            s_last_block = fb;
            s_have_last = true;
            TickType_t decode_start = xTaskGetTickCount();

            uint8_t sys = 0, cmd = 0, tog = 0;
            if (decode_rc5(fb.symbols, fb.num_symbols, &sys, &cmd, &tog) == 0) {
                s_stats.frames_ok++;
                TickType_t decode_end = xTaskGetTickCount();
                float latency = (decode_end - decode_start) * portTICK_PERIOD_MS;
                s_stats.latency_ema_ms = 0.9f * s_stats.latency_ema_ms + 0.1f * latency;

                if (s_learn_action != IR_ACTION_NONE) {
                    s_learn_system = sys;
                    s_learn_command = cmd;
                    s_learn_success = true;
                    s_learn_action = IR_ACTION_NONE;
                } else {
                    TickType_t now_tick = xTaskGetTickCount();
                    TickType_t dt_tick = now_tick - s_last_state.last_event_tick;
                    int64_t dt_ms = dt_tick * portTICK_PERIOD_MS;
                    bool is_repeat = false;

                    if (dt_ms < 300 && sys == s_last_state.system && cmd == s_last_state.command) {
                        if (tog == s_last_state.last_toggle) is_repeat = true;
                    }

                    s_last_state.system = sys;
                    s_last_state.command = cmd;
                    s_last_state.last_toggle = tog;
                    s_last_state.last_event_tick = now_tick;

                    ir_action_t action = ir_get_mapped_action(sys, cmd);
                    if (action != IR_ACTION_NONE) {
                        bool dispatch = false;
                        if (is_repeat) {
                            if (action == IR_ACTION_VOL_UP || action == IR_ACTION_VOL_DOWN) dispatch = true;
                        } else {
                            dispatch = true;
                        }
                        if (dispatch) ir_execute_action(action);
                    }
                }
            } else {
                s_stats.manchester_errors++;
            }
        }
    }
}

esp_err_t ir_init(void) {
    /* Явно конфігуруємо GPIO47 як вход. */
    gpio_config_t io = {
        .pin_bit_mask = BIT64(BSP_PIN_IR_OUT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    s_frame_q = xQueueCreate(3, sizeof(ir_frame_buf_t));
    if (s_frame_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_channel_config_t chan_cfg = {
        .gpio_num = BSP_PIN_IR_OUT,
        .clk_src = RMT_CLK_SRC_APB,
        .resolution_hz = 1000000,   /* 1 MHz: тривалості символів = 1 мкс */
        .mem_block_symbols = IR_RX_SYMBOLS,
        .flags = {
            .invert_in = 0,
        },
    };
    esp_err_t err = rmt_new_rx_channel(&chan_cfg, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        s_rmt_err = err;
        return err;
    }

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = ir_rx_done_cb,
    };
    err = rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_rx_register_event_callbacks failed: %s", esp_err_to_name(err));
        s_rmt_err = err;
        return err;
    }

    /* Канал має бути явно вмкнений ПЕРЕД першим rmt_receive(). */
    err = rmt_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        s_rmt_err = err;
        return err;
    }

    err = rmt_receive(s_rx_chan, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive failed: %s", esp_err_to_name(err));
        s_rmt_err = err;
        return err;
    }

    settings_ir_map_load(s_ir_map, &s_ir_map_count);
    ir_dev_register_commands();
    BaseType_t ok = xTaskCreate(ir_task, "ir_task", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ir_get_stats(ir_stats_t *stats) {
    *stats = s_stats;
}

esp_err_t ir_get_rmt_err(void) {
    return s_rmt_err;
}