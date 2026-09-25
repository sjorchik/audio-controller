#include "button_sm.h"

/* Стани стан-машини */
enum {
    STATE_IDLE = 0,
    STATE_DEBOUNCE_PRESS,
    STATE_HELD,
    STATE_LONG_HOLD,
    STATE_DEBOUNCE_RELEASE,
};

/* Пороги таймінгів (мс) */
#define DEBOUNCE_MS         25
#define SHORT_MAX_MS       500
#define LONG_THRESHOLD_MS  600
#define REPEAT_THRESHOLD_MS 1000
#define REPEAT_INTERVAL_MS 250

void button_sm_init(button_sm_t *sm, bool repeat_allowed)
{
    sm->state = STATE_IDLE;
    sm->event_time_ms = 0;
    sm->press_time_ms = 0;
    sm->last_repeat_ms = 0;
    sm->long_fired = false;
    sm->short_pending = false;
    sm->cfg.repeat_allowed = repeat_allowed;
}

btn_action_t button_sm_update(button_sm_t *sm, bool level_active, uint32_t now_ms)
{
    btn_action_t action = BTN_ACTION_NONE;

    switch (sm->state) {
    case STATE_IDLE:
        if (level_active) {
            sm->state = STATE_DEBOUNCE_PRESS;
            sm->press_time_ms = now_ms;
            sm->event_time_ms = now_ms;
            sm->long_fired = false;
            sm->short_pending = false;
        }
        break;

    case STATE_DEBOUNCE_PRESS:
        if ((now_ms - sm->event_time_ms) >= DEBOUNCE_MS) {
            if (level_active) {
                sm->state = STATE_HELD;
            } else {
                /* Glitch: кнопка відпустилася раніше дебаунсу — події немає */
                sm->state = STATE_IDLE;
            }
        }
        break;

    case STATE_HELD:
        if (!level_active) {
            uint32_t held_time = now_ms - sm->press_time_ms;
            sm->event_time_ms = now_ms;
            if (held_time >= DEBOUNCE_MS) {
                /* Рішення фіксуємо, але ПОДІЮ ВІДКЛАДАЄМО до стабільного
                 * release-дебанусу: інакше бренькіт на відпусканні дав би
                 * SHORT, а потім ще й LONG на те саме натискання. */
                sm->short_pending = (held_time < SHORT_MAX_MS);
                /* Якщо 500 <= held_time < 600 — dead zone: short_pending=false */
                sm->state = STATE_DEBOUNCE_RELEASE;
            } else {
                sm->state = STATE_IDLE;
            }
        } else {
            uint32_t held_time = now_ms - sm->press_time_ms;
            if (held_time >= LONG_THRESHOLD_MS && !sm->long_fired) {
                action = BTN_ACTION_LONG;
                sm->long_fired = true;
                sm->state = STATE_LONG_HOLD;
                sm->last_repeat_ms = now_ms;
            }
        }
        break;

    case STATE_LONG_HOLD:
        if (!level_active) {
            sm->event_time_ms = now_ms;
            sm->short_pending = false; /* LONG вже був: SHORT неможливий */
            sm->state = STATE_DEBOUNCE_RELEASE;
        } else if (sm->cfg.repeat_allowed) {
            uint32_t held_time = now_ms - sm->press_time_ms;
            if (held_time >= REPEAT_THRESHOLD_MS &&
                (now_ms - sm->last_repeat_ms) >= REPEAT_INTERVAL_MS) {
                action = BTN_ACTION_REPEAT;
                sm->last_repeat_ms = now_ms;
            }
        }
        break;

    case STATE_DEBOUNCE_RELEASE:
        if ((now_ms - sm->event_time_ms) >= DEBOUNCE_MS) {
            if (!level_active) {
                /* Відпускання стабільне — видаємо відкладений SHORT */
                if (sm->short_pending) {
                    action = BTN_ACTION_SHORT;
                    sm->short_pending = false;
                }
                sm->state = STATE_IDLE;
            } else {
                /* Бренькіт на відпусканні: рівень знову активний.
                 * Скасовуємо SHORT і повертаємось в утримання. */
                sm->short_pending = false;
                sm->state = sm->long_fired ? STATE_LONG_HOLD : STATE_HELD;
            }
        }
        /* Вікно ще не минуло — ігноруємо семпли (поглинання бренькоту) */
        break;
    }

    return action;
}