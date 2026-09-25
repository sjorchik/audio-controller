/*
 * test/button_sm/test_button_sm.c
 * Юніт-тести платформо-незалежної стан-машини кнопок (roadmap item 4).
 * Запуск: pio test -e native (сьют button_sm).
 *
 * button_sm.c включається як source, щоб уникнути лінкування з
 * ESP-IDF-залежними модулями (аналогічно до сьюту dsp_math).
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>

/* Платформо-незалежний модуль — без esp-заголовків, можна на host.
 * "button_sm.h" всередині button_sm.c знаходиться через
 * -Icomponents/input/include у platformio.ini. */
#include "../../components/input/button_sm.c"

#define BTN_ACTIVE   true
#define BTN_INACTIVE false

static button_sm_t sm;

void setUp(void) {
    button_sm_init(&sm, false); // за замовчуванням без repeat
}
void tearDown(void) {}

/* Тест 1: Дебаунс (глітч) не генерує подій */
static void test_debounce_glitch(void) {
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 0));
    // Глітч 10 мс
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 10));
    // Дебаунс не завершено, тому повертаємося в IDLE
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 30));
}

/* Тест 2: SHORT press */
static void test_short_press(void) {
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 0));
    // Дебаунс завершено
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 25));
    // Утримуємо трохи (200 мс)
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 200));
    // Відпускаємо
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 200));
    // Дебаунс відпускання завершено
    TEST_ASSERT_EQUAL(BTN_ACTION_SHORT, button_sm_update(&sm, BTN_INACTIVE, 225));
}

/* Тест 3: LONG press (без repeat) */
static void test_long_press(void) {
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 0));
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 25));
    // Утримуємо 600 мс
    TEST_ASSERT_EQUAL(BTN_ACTION_LONG, button_sm_update(&sm, BTN_ACTIVE, 600));
    // Утримуємо далі (не має бути повторного LONG)
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 700));
    // Відпускаємо
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 800));
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 825));
}

/* Тест 4: REPEAT press (для стрілок) */
static void test_repeat_press(void) {
    button_sm_init(&sm, true); // з repeat
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 0));
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 25));
    // LONG на 600 мс
    TEST_ASSERT_EQUAL(BTN_ACTION_LONG, button_sm_update(&sm, BTN_ACTIVE, 600));
    // REPEAT починається після 1000 мс утримання
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 900));
    TEST_ASSERT_EQUAL(BTN_ACTION_REPEAT, button_sm_update(&sm, BTN_ACTIVE, 1000));
    // Наступний REPEAT через 250 мс
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 1100));
    TEST_ASSERT_EQUAL(BTN_ACTION_REPEAT, button_sm_update(&sm, BTN_ACTIVE, 1250));
    // Відпускаємо
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 1300));
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 1325));
}

/* Тест 5: Dead zone 500-600 мс */
static void test_dead_zone(void) {
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 0));
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 25));
    // Утримуємо 550 мс
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_ACTIVE, 550));
    // Відпускаємо (має бути DEBOUNCE_RELEASE, але без SHORT)
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 550));
    TEST_ASSERT_EQUAL(BTN_ACTION_NONE, button_sm_update(&sm, BTN_INACTIVE, 575));
}

/* Тест 6: 100 натискань SHORT (швидкий темп, без подвійних спрацювань) */
static void test_100_short_presses(void) {
    uint32_t t = 0;
    int short_count = 0;
    for (int i = 0; i < 100; i++) {
        // Натискання
        button_sm_update(&sm, BTN_ACTIVE, t);
        t += 25;
        button_sm_update(&sm, BTN_ACTIVE, t);
        t += 100; // Утримуємо 100 мс
        button_sm_update(&sm, BTN_INACTIVE, t);
        t += 25; // Дебаунс відпускання
        btn_action_t a = button_sm_update(&sm, BTN_INACTIVE, t);
        if (a == BTN_ACTION_SHORT) short_count++;
        t += 50; // Пауза між натисканнями
    }
    TEST_ASSERT_EQUAL(100, short_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_debounce_glitch);
    RUN_TEST(test_short_press);
    RUN_TEST(test_long_press);
    RUN_TEST(test_repeat_press);
    RUN_TEST(test_dead_zone);
    RUN_TEST(test_100_short_presses);
    UNITY_END();
    return 0;
}