/*
 * system.c — системний компонент: шина подій + dev-консоль bring-up.
 *
 * Dev-консоль (CONFIG_APP_DEV_CONSOLE): команди i2cscan / src / mute / stats / raw
 * реалізовані через esp_console, але читання рядків виконує ВЛАСНА задача
 * (посимвольний fgetc з echo), бо linenoise-REPL у no-TTY режимі спотворює
 * символи завершення рядка (CR/LF) і команди не розпізнаються.
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
#endif

static const char *TAG = "system";

#ifdef CONFIG_APP_DEV_CONSOLE

/* ────────────────────────────────────────────────────────────────
 * Обробники команд
 * ──────────────────────────────────────────────────────────────── */

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ESP_LOGI(TAG, "Сканування I2C шини...");
    i2c_master_bus_handle_t bus = tda7318_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "I2C шина не ініціалізована");
        return 1;
    }

    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(bus, addr, 100) == ESP_OK) {
            ESP_LOGI(TAG, "Знайдено пристрій за адресою: 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "Пристроїв не знайдено");
    } else {
        ESP_LOGI(TAG, "Всього знайдено: %d пристроїв", found);
    }
    return 0;
}

static struct {
    struct arg_int *src;
    struct arg_end *end;
} src_args;

static int cmd_src(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&src_args) != 0) {
        ESP_LOGE(TAG, "Помилка парсингу аргументів");
        return 1;
    }
    int source = src_args.src->ival[0];
    if (source < 0 || source >= BSP_AUDIO_SOURCE_MAX) {
        ESP_LOGE(TAG, "Джерело має бути 0..%d", BSP_AUDIO_SOURCE_MAX - 1);
        return 1;
    }
    ESP_LOGI(TAG, "Перемикання на джерело %d", source);
    esp_err_t ret = tda7318_switch_source((bsp_audio_source_t)source);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Помилка перемикання: %s", esp_err_to_name(ret));
        return 1;
    }
    return 0;
}

static struct {
    struct arg_int *mute;
    struct arg_end *end;
} mute_args;

static int cmd_mute(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&mute_args) != 0) {
        ESP_LOGE(TAG, "Помилка парсингу аргументів");
        return 1;
    }
    int state = mute_args.mute->ival[0];
    esp_err_t ret = tda7318_set_mute(state == 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Помилка встановлення mute: %s", esp_err_to_name(ret));
        return 1;
    }
    ESP_LOGI(TAG, "Mute %s", state ? "ON" : "OFF");
    return 0;
}

static int cmd_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    tda7318_stats_t stats;
    tda7318_get_stats(&stats);
    ESP_LOGI(TAG, "Лічильник помилок I2C: %lu", (unsigned long)stats.i2c_errors);
    return 0;
}

static struct {
    struct arg_str *val;
    struct arg_end *end;
} raw_args;

static int cmd_raw(int argc, char **argv)
{
    (void)argc;
    if (arg_parse(argc, argv, (void **)&raw_args) != 0) {
        ESP_LOGE(TAG, "Помилка парсингу аргументів");
        return 1;
    }
    char *end = NULL;
    unsigned long v = strtoul(raw_args.val->sval[0], &end, 0);   // приймає 0x..
    if (end == raw_args.val->sval[0] || v > 0xFF) {
        ESP_LOGE(TAG, "Потрібен байт 0x00..0xFF");
        return 1;
    }
    esp_err_t ret = tda7318_raw_write((uint8_t)v);
    ESP_LOGI(TAG, "raw write 0x%02X -> %s", (unsigned)v, esp_err_to_name(ret));
    return (ret == ESP_OK) ? 0 : 1;
}

/* ────────────────────────────────────────────────────────────────
 * Власний читач рядків (замість linenoise-REPL)
 * ──────────────────────────────────────────────────────────────── */

static void dev_console_reader_task(void *arg);   // forward declaration

static void dev_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "audio-ctrl>";
    repl_config.max_cmdline_length = 256;

    // Ініціалізує консольний UART та реєстр команд esp_console.
    // REPL-задачу (linenoise) НЕ стартуємо — читання рядків власне (нижче).
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    (void)repl;

    // Команда i2cscan
    esp_console_cmd_t i2cscan_cmd = {
        .command = "i2cscan",
        .help = "Сканування I2C шини",
        .func = &cmd_i2cscan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cscan_cmd));

    // Команда src <0..3>
    src_args.src = arg_int1(NULL, NULL, "<0..3>",
                            "Джерело (0=TV_BOX, 1=COMPUTER, 2=BLUETOOTH, 3=AUX)");
    src_args.end = arg_end(1);
    esp_console_cmd_t src_cmd = {
        .command = "src",
        .help = "Перемикання джерела аудіо (безклікова послідовність)",
        .func = &cmd_src,
        .argtable = &src_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&src_cmd));

    // Команда mute <0|1>
    mute_args.mute = arg_int1(NULL, NULL, "<0|1>", "Mute селектора (0=unmute, 1=mute)");
    mute_args.end = arg_end(1);
    esp_console_cmd_t mute_cmd = {
        .command = "mute",
        .help = "Керування soft mute селектора",
        .func = &cmd_mute,
        .argtable = &mute_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mute_cmd));

    // Команда stats
    esp_console_cmd_t stats_cmd = {
        .command = "stats",
        .help = "Показати статистику драйвера селектора",
        .func = &cmd_stats,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stats_cmd));

    // Команда raw (bring-up діагностика кодів селектора)
    raw_args.val = arg_str1(NULL, NULL, "<byte>", "Байт команди (hex, напр. 0x00)");
    raw_args.end = arg_end(1);
    esp_console_cmd_t raw_cmd = {
        .command = "raw",
        .help = "Bring-up: прямий запис байта команди селектора",
        .func = &cmd_raw,
        .argtable = &raw_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&raw_cmd));

    // Self-check реєстру: програмний виклик команди, минаючи термінальний ввід
    int self_ret = 0;
    esp_err_t run_err = esp_console_run("stats", &self_ret);
    ESP_LOGI(TAG, "registry self-check: esp_console_run(\"stats\") -> %s",
             esp_err_to_name(run_err));

    // Власна задача читання рядків
    if (xTaskCreate(dev_console_reader_task, "console_rd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Не вдалося створити задачу читання консолі");
        return;
    }
    ESP_LOGI(TAG, "Dev-консоль запущена (власний reader, prompt: audio-ctrl>)");
}

/*
 * Мінімальний рядковий редактор: посимвольне читання з stdin, echo,
 * backspace, завершення по CR або LF (CRLF з'їдається коректно).
 * Не залежить від TTY/escape-підтримки термінала.
 * Логи інших задач можуть візуально перебивати рядок вводу — це
 * нормально для dev-консолі, введена команда все одно виконається.
 */
static void dev_console_reader_task(void *arg)
{
    (void)arg;
    char line[256];
    size_t pos = 0;
    bool prev_cr = false;

    vTaskDelay(pdMS_TO_TICKS(300));   // дочекатися старту UART-консолі
    fputs("audio-ctrl> ", stdout);
    fflush(stdout);

    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* CRLF: '\n' одразу після '\r' не вважаємо новим рядком */
        if (c == '\n' && prev_cr) {
            prev_cr = false;
            continue;
        }
        prev_cr = (c == '\r');

        if (c == '\r' || c == '\n') {
            fputs("\r\n", stdout);
            line[pos] = '\0';
            pos = 0;

            /* Захист від вставки рядка разом із prompt'ом: відрізаємо префікс */
            const char *pfx = "audio-ctrl> ";
            size_t pfx_len = strlen(pfx);
            if (strncmp(line, pfx, pfx_len) == 0) {
                memmove(line, line + pfx_len, strlen(line + pfx_len) + 1);
            }

            if (line[0] != '\0') {
                int ret = 0;
                esp_err_t err = esp_console_run(line, &ret);
                if (err == ESP_ERR_NOT_FOUND) {
                    printf("Unrecognized command: %s\n", line);
                } else if (err != ESP_OK) {
                    printf("Command error: %s\n", esp_err_to_name(err));
                }
            }
            fputs("audio-ctrl> ", stdout);
            fflush(stdout);
            continue;
        }

        if (c == 0x7F || c == 0x08) {          // Backspace
            if (pos > 0) {
                pos--;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
            continue;
        }

        if (c >= 0x20 && pos < sizeof(line) - 1) {
            line[pos++] = (char)c;
            fputc(c, stdout);                  // echo введеного символу
            fflush(stdout);
        }
    }
}

#endif /* CONFIG_APP_DEV_CONSOLE */

/* ────────────────────────────────────────────────────────────────
 * Публічне API компонента system
 * ──────────────────────────────────────────────────────────────── */

esp_err_t system_init(void)
{
#ifdef CONFIG_APP_DEV_CONSOLE
    dev_console_init();
#endif
    return ESP_OK;
}

esp_err_t system_post(system_event_id_t event_id, const void *data, size_t data_size)
{
    (void)event_id;
    (void)data;
    (void)data_size;
    // TODO: реалізація шини подій у майбутніх пунктах ROADMAP
    return ESP_ERR_NOT_SUPPORTED;
}