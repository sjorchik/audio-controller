#include "ir.h"
#include "settings.h"
#include "bsp.h"
#include "driver/gpio.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ir_pair_t s_ir_map[32];
extern uint16_t s_ir_map_count;
extern volatile ir_action_t s_learn_action;
extern volatile bool s_learn_success;
extern volatile uint8_t s_learn_system;
extern volatile uint8_t s_learn_command;

static int cmd_irlearn(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: irlearn <action>\n");
        return 1;
    }
    ir_action_t action = ir_action_from_string(argv[1]);
    if (action == IR_ACTION_NONE) {
        printf("Unknown action. Use 'iractions' to see available.\n");
        return 1;
    }

    s_learn_action = action;
    s_learn_success = false;

    printf("Waiting for IR code (10s)...\n");
    for (int i = 0; i < 100; i++) {
        if (s_learn_success) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_learn_success) {
        printf("Learned sys=%u, cmd=%u -> %s\n", s_learn_system, s_learn_command, argv[1]);
        bool updated = false;
        for (int i = 0; i < s_ir_map_count; i++) {
            if (s_ir_map[i].system == s_learn_system && s_ir_map[i].command == s_learn_command) {
                s_ir_map[i].action = action;
                updated = true;
                break;
            }
        }
        if (!updated && s_ir_map_count < 32) {
            s_ir_map[s_ir_map_count].system = s_learn_system;
            s_ir_map[s_ir_map_count].command = s_learn_command;
            s_ir_map[s_ir_map_count].action = action;
            s_ir_map_count++;
        }
        settings_ir_map_save(s_ir_map, s_ir_map_count);
    } else {
        printf("Timeout.\n");
        s_learn_action = IR_ACTION_NONE;
    }
    return 0;
}

static int cmd_irunlearn(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "all") == 0) {
        esp_err_t err = ir_map_clear();
        printf("map cleared: %s\n", esp_err_to_name(err));
        return 0;
    }
    if (argc < 3) {
        printf("Usage: irunlearn <system> <command> | irunlearn all\n");
        return 1;
    }
    int sys = atoi(argv[1]);
    int cmd = atoi(argv[2]);
    esp_err_t err = ir_map_remove((uint8_t)sys, (uint8_t)cmd);
    if (err == ESP_OK) {
        printf("removed sys=%d cmd=%d\n", sys, cmd);
    } else {
        printf("not found: sys=%d cmd=%d\n", sys, cmd);
    }
    return 0;
}

static int cmd_iractions(int argc, char **argv) {
    printf("Available actions:\n");
    for (int i = 1; i < IR_ACTION_MAX; i++) {
        printf("  %s\n", ir_action_to_string((ir_action_t)i));
    }
    return 0;
}

static int cmd_irmap(int argc, char **argv) {
    printf("Current IR map (%u items):\n", s_ir_map_count);
    for (int i = 0; i < s_ir_map_count; i++) {
        printf("  sys=%u, cmd=%u -> %s\n", s_ir_map[i].system, s_ir_map[i].command, ir_action_to_string(s_ir_map[i].action));
    }
    return 0;
}

static int cmd_irstats(int argc, char **argv) {
    ir_stats_t st;
    ir_get_stats(&st);
    printf("ir stats: rx_blocks=%lu frames_ok=%lu manchester_err=%lu crc_err=%lu drops=%lu latency_ema=%.2f ms gpio47=%d rmt_err=%d\n",
           (unsigned long)st.rx_blocks, (unsigned long)st.frames_ok,
           (unsigned long)st.manchester_errors, (unsigned long)st.crc_errors,
           (unsigned long)st.drops, (double)st.latency_ema_ms,
           (int)gpio_get_level(BSP_PIN_IR_OUT), (int)ir_get_rmt_err());
    return 0;
}

static int cmd_irdump(int argc, char **argv) {
    ir_dump_last_block();
    return 0;
}

void ir_dev_register_commands(void) {
    const esp_console_cmd_t cmds[] = {
        { .command = "irlearn", .help = "Навчання IR: захопити наступний RC5-код і прив'язати до дії (таймаут 10 с)", .func = &cmd_irlearn },
        { .command = "irunlearn", .help = "Видалити прив'язку IR-коду: irunlearn <system> <command> або irunlearn all", .func = &cmd_irunlearn },
        { .command = "iractions", .help = "Список дій, доступних для прив'язки IR", .func = &cmd_iractions },
        { .command = "irmap", .help = "Друк поточної карти прив'язок IR-кодів (code -> action)", .func = &cmd_irmap },
        { .command = "irstats", .help = "Статистика IR-декодера (rx blocks, frames, errors, latency EMA, gpio47, rmt_err)", .func = &cmd_irstats },
        { .command = "irdump", .help = "Дамп останнього прийнятого IR-блоку (тривалості імпульсів і пів-біти) для діагностики протоколу", .func = &cmd_irdump }
    };
    for (int i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
}