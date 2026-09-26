// Файл: components/system/system.c
/*
 * system.c — системний компонент: шина подій + dev-консоль bring-up.
 *
 * Шина подій (v2.2.5): system_post() окрім логування evmon доставляє подію
 * підписнику (ui) через чергу, зареєстровану system_register_queue().
 * Події стану моделі (SYSTEM_EVT_AUDIO_STATE, SYSTEM_EVENT_SOURCE_CHANGED)
 * постяться БЕЗ payload — споживач перечитує геттери.
 *
 * ISR-безпека: події SYSTEM_EVENT_INPUT частково постяться НАПРЯМУ
 * з контексту ISR (PCNT watch callback енкодера / GPIO ISR кнопок).
 * Тому dispatch у чергу підписника розгалужується за xPortInIsrContext():
 *   - ISR:  xQueueSendFromISR() + portYIELD_FROM_ISR(), БЕЗ логів;
 *   - task: xQueueSend() + evmon-логування.
 *
 * bring-up пункт 5: стек задачі console_rd 4096 -> 6144; команди src/mute
 * друкують feedback-рядок (bring-up UX: мовчазні команди плутали оператора).
 */

#include "system.h"
#include "esp_log.h"

#ifdef CONFIG_APP_DEV_CONSOLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "bsp.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tda7318.h"

/* Команду lcdtest реалізовано в components/ui/ui.c. Оголошуємо напряму,
 * щоб НЕ створювати залежність system -> ui (цикл ui <-> system).
 * Символ розв'язується на лінкуванні: обидві бібліотеки в одному ELF. */
extern int ui_cmd_lcdtest(int argc, char **argv);

/* Сетери/гетери гучності моделі audio — оголошуємо напряму з тієї ж причини
 * (без залежності system -> audio; команда vol потрібна для критерію 5:
 * провокація SYSTEM_EVT_AUDIO_STATE з dev-консолі). */
extern void  audio_set_volume_db(float db);
extern float audio_get_volume_db(void);
#endif

static const char *TAG = "system";

/* Черга підписника шини (ui). NULL — поки ніхто не підписаний. */
static QueueHandle_t s_subscriber_queue = NULL;

#ifdef CONFIG_APP_DEV_CONSOLE
static bool s_evmon_enabled = false;

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc; (void)argv;
    ESP_LOGI(TAG, "Сканування I2C шини...");
    i2c_master_bus_handle_t bus = tda7318_get_i2c_bus();
    if (!bus) { ESP_LOGE(TAG, "I2C шина не ініціалізована"); return 1; }
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(bus, addr, 100) == ESP_OK) {
            ESP_LOGI(TAG, "Знайдено пристрій за адресою: 0x%02X", addr); found++;
        }
    }
    if (found == 0) ESP_LOGW(TAG, "Пристроїв не знайдено");
    else ESP_LOGI(TAG, "Всього знайдено: %d пристроїв", found);
    return 0;
}

static struct { struct arg_int *src; struct arg_end *end; } src_args;
static int cmd_src(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&src_args) != 0) return 1;
    int source = src_args.src->ival[0];
    if (source < 0 || source >= BSP_AUDIO_SOURCE_MAX) {
        printf("src: invalid %d (0..%d)\n", source, BSP_AUDIO_SOURCE_MAX - 1);
        return 1;
    }
    esp_err_t ret = tda7318_switch_source((bsp_audio_source_t)source);
    /* feedback: no-op (те саме джерело) теж звітуємо, щоб оператор
     * не плутав мовчазність з невиконанням */
    printf("src %d: %s%s\n", source,
           ret == ESP_OK ? "ok" : esp_err_to_name(ret),
           (ret == ESP_OK && tda7318_get_source() == (bsp_audio_source_t)source) ? "" : " (ignored)");
    return ret == ESP_OK ? 0 : 1;
}

static struct { struct arg_int *mute; struct arg_end *end; } mute_args;
static int cmd_mute(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&mute_args) != 0) return 1;
    int m = mute_args.mute->ival[0];
    esp_err_t ret = tda7318_set_mute(m == 1);
    /* УВАГА: це апаратний mute СЕЛЕКТОРА, не DSP-mute моделі audio;
     * індикатор MUTE на Main відображає audio_get_mute() і тому
     * на цю команду не реагує (архітектура v2.2.5, різні шари). */
    printf("selector mute %d: %s (DSP-mute не змінюється)\n", m,
           ret == ESP_OK ? "ok" : esp_err_to_name(ret));
    return ret == ESP_OK ? 0 : 1;
}

/* vol [db] — без аргументу друк поточної гучності; з аргументом — сетер
 * моделі audio, який постить SYSTEM_EVT_AUDIO_STATE (перевірка критерію 5). */
static struct { struct arg_int *db; struct arg_end *end; } vol_args;
static int cmd_vol(int argc, char **argv)
{
    if (argc > 1 && arg_parse(argc, argv, (void **)&vol_args) != 0) return 1;
    if (argc > 1) audio_set_volume_db((float)vol_args.db->ival[0]);
    printf("volume: %.1f dB\n", (double)audio_get_volume_db());
    return 0;
}

static int cmd_stats(int argc, char **argv)
{
    (void)argc; (void)argv;
    tda7318_stats_t stats;
    tda7318_get_stats(&stats);
    ESP_LOGI(TAG, "Лічильник помилок I2C: %lu", (unsigned long)stats.i2c_errors);
    return 0;
}

static struct { struct arg_str *val; struct arg_end *end; } raw_args;
static int cmd_raw(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&raw_args) != 0) return 1;
    char *end = NULL;
    unsigned long v = strtoul(raw_args.val->sval[0], &end, 0);
    if (end == raw_args.val->sval[0] || v > 0xFF) return 1;
    return tda7318_raw_write((uint8_t)v) == ESP_OK ? 0 : 1;
}

static int cmd_evmon(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) { s_evmon_enabled = true; printf("evmon: enabled\n"); }
    else if (argc == 2 && strcmp(argv[1], "off") == 0) { s_evmon_enabled = false; printf("evmon: disabled\n"); }
    else printf("Usage: evmon on|off\n");
    return 0;
}

static void dev_console_reader_task(void *arg);

static void dev_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "audio-ctrl>";
    repl_config.max_cmdline_length = 256;
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    (void)repl;

    esp_console_cmd_t i2cscan_cmd = { .command = "i2cscan", .help = "Сканування I2C шини", .func = &cmd_i2cscan };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cscan_cmd));

    src_args.src = arg_int1(NULL, NULL, "<0..3>", "Джерело");
    src_args.end = arg_end(1);
    esp_console_cmd_t src_cmd = { .command = "src", .help = "Перемикання джерела (feedback: ok/ignored/fail)", .func = &cmd_src, .argtable = &src_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&src_cmd));

    mute_args.mute = arg_int1(NULL, NULL, "<0|1>", "Mute селектора");
    mute_args.end = arg_end(1);
    esp_console_cmd_t mute_cmd = { .command = "mute", .help = "Апаратний mute селектора (не DSP-mute)", .func = &cmd_mute, .argtable = &mute_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mute_cmd));

    vol_args.db = arg_int0(NULL, NULL, "<-90..0>", "Гучність, dB");
    vol_args.end = arg_end(1);
    esp_console_cmd_t vol_cmd = { .command = "vol", .help = "Гучність моделі audio (без арг. — друк); постить SYSTEM_EVT_AUDIO_STATE", .func = &cmd_vol, .argtable = &vol_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vol_cmd));

    esp_console_cmd_t stats_cmd = { .command = "stats", .help = "Статистика", .func = &cmd_stats };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stats_cmd));

    raw_args.val = arg_str1(NULL, NULL, "<byte>", "Байт команди");
    raw_args.end = arg_end(1);
    esp_console_cmd_t raw_cmd = { .command = "raw", .help = "Прямий запис байта", .func = &cmd_raw, .argtable = &raw_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&raw_cmd));

    esp_console_cmd_t evmon_cmd = { .command = "evmon", .help = "Монітор подій", .func = &cmd_evmon };
    ESP_ERROR_CHECK(esp_console_cmd_register(&evmon_cmd));

    esp_console_cmd_t lcdtest_cmd = {
        .command = "lcdtest",
        .help = "Тест дисплея: fill, gradient, corners, border, offset <x> <y>, orient <swap> <mx> <my>",
        .func = &ui_cmd_lcdtest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcdtest_cmd));

    int self_ret = 0;
    esp_console_run("stats", &self_ret);

    /* 6144: lcdtest-команди додають глибини стеку понад bring-up-мінімум 4096 */
    if (xTaskCreate(dev_console_reader_task, "console_rd", 6144, NULL, 5, NULL) != pdPASS) return;
}

/* Власний line-reader консолі (специфікація v2.2.4): читаємо stdin
 * у власній задачі, без blocking-read REPL. */
static void dev_console_reader_task(void *arg)
{
    (void)arg;
    char line[256]; size_t pos = 0; bool prev_cr = false;
    vTaskDelay(pdMS_TO_TICKS(300));
    fputs("audio-ctrl> ", stdout); fflush(stdout);
    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (c == '\n' && prev_cr) { prev_cr = false; continue; }
        prev_cr = (c == '\r');
        if (c == '\r' || c == '\n') {
            fputs("\r\n", stdout); line[pos] = '\0'; pos = 0;
            const char *pfx = "audio-ctrl> "; size_t pfx_len = strlen(pfx);
            if (strncmp(line, pfx, pfx_len) == 0) memmove(line, line + pfx_len, strlen(line + pfx_len) + 1);
            if (line[0] != '\0') {
                int ret = 0; esp_err_t err = esp_console_run(line, &ret);
                if (err == ESP_ERR_NOT_FOUND) printf("Unrecognized command: %s\n", line);
                else if (err != ESP_OK) printf("Command error: %s\n", esp_err_to_name(err));
            }
            fputs("audio-ctrl> ", stdout); fflush(stdout); continue;
        }
        if (c == 0x7F || c == 0x08) {
            if (pos > 0) { pos--; fputs("\b \b", stdout); fflush(stdout); } continue;
        }
        if (c >= 0x20 && pos < sizeof(line) - 1) { line[pos++] = (char)c; fputc(c, stdout); fflush(stdout); }
    }
}
#endif

esp_err_t system_init(void)
{
#ifdef CONFIG_APP_DEV_CONSOLE
    dev_console_init();
#endif
    return ESP_OK;
}

esp_err_t system_register_queue(QueueHandle_t queue)
{
    s_subscriber_queue = queue;
    return ESP_OK;
}

/* Доставка події підписнику (ui). Спільна для task/ISR-шляхів:
 * події стану моделі йдуть без payload — споживач перечитує геттери
 * (архітектура v2.2.5). Повертає прапорець необхідності yield. */
static BaseType_t deliver_to_subscriber(system_event_id_t event_id,
                                        const void *data, size_t data_size,
                                        BaseType_t from_isr)
{
    if (!s_subscriber_queue) return pdFALSE;

    ui_event_t ui_evt = { .id = event_id };
    if (event_id == SYSTEM_EVENT_INPUT && data && data_size == sizeof(input_event_t)) {
        ui_evt.input = *(const input_event_t *)data;
    }

    if (from_isr) {
        BaseType_t yield = pdFALSE;
        xQueueSendFromISR(s_subscriber_queue, &ui_evt, &yield);
        return yield;
    }
    xQueueSend(s_subscriber_queue, &ui_evt, 0);
    return pdFALSE;
}

esp_err_t system_post(system_event_id_t event_id, const void *data, size_t data_size)
{
    /* ── Шлях ISR (PCNT/GPIO-переривання input): тільки FromISR-API,
     *    жодних логів/printf/VFS — вони не ISR-безпечні. ── */
    if (xPortInIsrContext()) {
        BaseType_t yield = deliver_to_subscriber(event_id, data, data_size, pdTRUE);
        if (yield == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return ESP_OK;
    }

    /* ── Шлях задачі-викликача ── */
#ifdef CONFIG_APP_DEV_CONSOLE
    if (event_id == SYSTEM_EVENT_INPUT && s_evmon_enabled && data && data_size == sizeof(input_event_t)) {
        const input_event_t *evt = (const input_event_t *)data;
        const char *src_names[] = {"NONE", "BTN_POWER", "BTN_UP", "BTN_DOWN", "BTN_LEFT", "BTN_RIGHT", "BTN_OK", "ENC_BTN", "ENC_STEP"};
        const char *act_names[] = {"NONE", "SHORT", "LONG", "REPEAT", "STEP"};
        if (evt->source < INPUT_SRC_MAX && evt->action < 5) {
            printf("EVMON: %s %s arg=%d ts=%lu\n", src_names[evt->source], act_names[evt->action], evt->arg, (unsigned long)evt->timestamp_ms);
            fflush(stdout);
        }
    }
#endif
    deliver_to_subscriber(event_id, data, data_size, pdFALSE);
    return ESP_OK;
}