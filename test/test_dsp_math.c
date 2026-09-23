/*
 * test/test_dsp_math.c
 * Юніт-тести платформо-незалежного DSP-модуля (roadmap item 2b).
 * Запуск: pio test -e native
 *
 * dsp_math.c включається як source, щоб уникнути лінкування з
 * ESP-IDF-залежними модулями (audio_process, audio, bsp).
 */

#include <unity.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Платформо-незалежний модуль — без esp-заголовків, можна на host */
#include "../components/audio/dsp_math.c"
#include "../components/audio/include/dsp_presets.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define FS 48000.0f

void setUp(void) {}
void tearDown(void) {}

/* ────────────────────────────────────────────────────────────────
 * Тест 1: Коефіцієнти peaking biquad (RBJ cookbook, Q=1.41)
 * Тестова точка: f=1000 Hz, Q=1.41, gain=+6 dB, fs=48000
 * Еталони — float64-розрахунок формул RBJ (перехресна перевірка float32).
 * ──────────────────────────────────────────────────────────────── */
static void test_biquad_coeffs_accuracy(void)
{
    biquad_coeffs_t c;
    dsp_calc_biquad_peaking(&c, 1000.0f, 1.41f, 6.0f, FS);

    /* Еталонні значення (float64):
     *   A = 10^(6/40) = 1.41253754
     *   w0 = 2*pi*1000/48000 = 0.13089969
     *   alpha = sin(w0)/(2*1.41) = 0.04628588
     *   b0/a0 = 1.0315779, b1/a0 = -1.9199762, b2/a0 = 0.9049656
     *   a1/a0 = -1.9199762, a2/a0 = 0.9365435
     */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,  1.0315779f, c.b0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -1.9199762f, c.b1);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,  0.9049656f, c.b2);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -1.9199762f, c.a1);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,  0.9365435f, c.a2);

    /* Додаткова точка: ТІ САМІ f/Q, але -6 dB -> b0/a0 < 1 */
    biquad_coeffs_t c2;
    dsp_calc_biquad_peaking(&c2, 1000.0f, 1.41f, -6.0f, FS);
    TEST_ASSERT_TRUE(c2.b0 < 1.0f);

    /* Симетрія RBJ: b0(+G) * b0(-G) = 1 точно.
     * УВАГА: симетрія справедлива ЛИШЕ за однакових f і Q —
     * тому c2 обчислено на тій самій частоті 1000 Гц. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, c.b0 * c2.b0);
}

/* ────────────────────────────────────────────────────────────────
 * Тест 2: АЧХ кожної з 10 смуг — gain на центрі = +6 dB ±0.5 dB.
 * Методика: warmup 8192 семпли (>=12 tau для 31.5 Гц, tau≈684),
 * вікно виміру = ЦІЛЕ число періодів (усуває похибку часткового періоду).
 * ──────────────────────────────────────────────────────────────── */
static void test_eq_frequency_response(void)
{
    for (int band = 0; band < DSP_NUM_EQ_BANDS; band++) {
        const float freq    = DSP_EQ_CENTERS[band];
        const float period  = FS / freq;
        const int   M       = (int)(4096.0f / period) + 1;  /* цілих періодів, >=4096 семплів */
        const int   N       = (int)(M * period + 0.5f);     /* вікно виміру = цілі періоди */
        const int   WARMUP  = 8192;
        const int   TOTAL   = WARMUP + N;

        biquad_coeffs_t coeffs[DSP_NUM_EQ_BANDS];
        biquad_state_t  states[DSP_NUM_EQ_BANDS];
        memset(states, 0, sizeof(states));

        /* Тільки тестована смуга має +6 dB, решта — 0 dB (точно unity) */
        for (int b = 0; b < DSP_NUM_EQ_BANDS; b++)
            dsp_calc_biquad_peaking(&coeffs[b], DSP_EQ_CENTERS[b], 1.41f,
                                    (b == band) ? 6.0f : 0.0f, FS);

        float buf[8192 + 4600];
        for (int i = 0; i < TOTAL; i++)
            buf[i] = 0.5f * sinf(2.0f * (float)M_PI * freq * i / FS);

        double rms_in = 0;
        for (int i = WARMUP; i < TOTAL; i++) rms_in += (double)buf[i] * buf[i];
        rms_in = sqrt(rms_in / N);

        for (int b = 0; b < DSP_NUM_EQ_BANDS; b++)
            dsp_biquad_process(buf, TOTAL, &coeffs[b], &states[b]);

        double rms_out = 0;
        for (int i = WARMUP; i < TOTAL; i++) rms_out += (double)buf[i] * buf[i];
        rms_out = sqrt(rms_out / N);

        double gain_db = 20.0 * log10(rms_out / rms_in);

        char msg[96];
        snprintf(msg, sizeof(msg), "Band %d (%7.1f Hz): gain = %.3f dB (expected 6.0)",
                 band, freq, gain_db);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5, 6.0, gain_db, msg);
    }
}

/* ────────────────────────────────────────────────────────────────
 * Тест 2b: Вплив на сусідні смуги.
 * Частина А (специфікація): крок +2 dB -> вплив на сусідніх центрах < 0.5 dB.
 * Частина Б (проєктна реальність): при +6 dB overlap смуг ~1 октава
 * (Q=1.41, фіксовано специфікацією) дає ~1.14 dB на сусідньому центрі —
 * це ПРАВИЛЬНА поведінка графічного EQ (ISO 1-octave), тому для +6 dB
 * перевіряємо лише розумну межу проєктного overlap (<1.5 dB).
 * ──────────────────────────────────────────────────────────────── */
static void test_eq_adjacent_band_influence(void)
{
    int neighbors[] = {4, 6};   /* 500 Hz та 2 kHz відносно band 5 (1 kHz) */

    for (int pass = 0; pass < 2; pass++) {
        const float set_gain = (pass == 0) ? 2.0f : 6.0f;
        const float limit_db = (pass == 0) ? 0.5f : 1.5f;

        biquad_coeffs_t coeffs[DSP_NUM_EQ_BANDS];
        for (int b = 0; b < DSP_NUM_EQ_BANDS; b++)
            dsp_calc_biquad_peaking(&coeffs[b], DSP_EQ_CENTERS[b], 1.41f,
                                    (b == 5) ? set_gain : 0.0f, FS);

        for (int n = 0; n < 2; n++) {
            const float freq    = DSP_EQ_CENTERS[neighbors[n]];
            const float period  = FS / freq;
            const int   M       = (int)(4096.0f / period) + 1;
            const int   N       = (int)(M * period + 0.5f);
            const int   WARMUP  = 8192;
            const int   TOTAL   = WARMUP + N;

            biquad_state_t s[DSP_NUM_EQ_BANDS];
            memset(s, 0, sizeof(s));

            float buf[8192 + 4600];
            for (int i = 0; i < TOTAL; i++)
                buf[i] = 0.5f * sinf(2.0f * (float)M_PI * freq * i / FS);

            double rms_in = 0;
            for (int i = WARMUP; i < TOTAL; i++) rms_in += (double)buf[i] * buf[i];
            rms_in = sqrt(rms_in / N);

            for (int b = 0; b < DSP_NUM_EQ_BANDS; b++)
                dsp_biquad_process(buf, TOTAL, &coeffs[b], &s[b]);

            double rms_out = 0;
            for (int i = WARMUP; i < TOTAL; i++) rms_out += (double)buf[i] * buf[i];
            rms_out = sqrt(rms_out / N);

            double gain_db = 20.0 * log10(rms_out / rms_in);
            char msg[110];
            snprintf(msg, sizeof(msg),
                     "Neighbor %d (%.0f Hz), set %+.0f dB: influence = %.3f dB (limit %.1f)",
                     neighbors[n], freq, set_gain, gain_db, limit_db);
            TEST_ASSERT_TRUE_MESSAGE(fabs(gain_db) < limit_db, msg);
        }
    }
}

/* ────────────────────────────────────────────────────────────────
 * Тест 3: Неперервність ramp; ціль досягається за ~30 мс ±1 блок
 * ──────────────────────────────────────────────────────────────── */
static void test_ramp_continuity(void)
{
    volume_ramp_state_t s = {0};
    s.current_gain_lin = powf(10.0f, -20.0f / 20.0f);   /* -20 dB */

    dsp_update_ramp(&s, -20.0f, 0.0f, FS);

    /* Очікуваний крок у dB на семпл: 20 dB / (0.030 * 48000) */
    float expected_db_step = 20.0f / (0.030f * FS);
    float actual_db_step   = 20.0f * log10f(s.multiplier);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expected_db_step, actual_db_step);

    const int RAMP_SAMPLES = (int)(0.030f * FS);        /* 1440 */
    float prev_db    = -20.0f;
    float max_step   = 0;
    int   reached_at = -1;

    for (int i = 0; i < RAMP_SAMPLES + 512; i++) {
        float g = s.current_gain_lin;
        if (s.multiplier == 1.0f) {
            g = s.target_gain_lin;
        } else if (g < s.target_gain_lin) {
            g *= s.multiplier;
            if (g > s.target_gain_lin) g = s.target_gain_lin;
        } else {
            g /= s.multiplier;
            if (g < s.target_gain_lin) g = s.target_gain_lin;
        }

        float g_db = 20.0f * log10f(g);
        float step = fabsf(g_db - prev_db);
        if (step > max_step) max_step = step;

        s.current_gain_lin = g;
        prev_db = g_db;

        if (fabsf(g_db - 0.0f) < 0.05f && reached_at < 0) {
            reached_at = i;
        }
    }

    /* Стрибок між сусідніми семплами не перевищує крок ramp (+epsilon) */
    TEST_ASSERT_FLOAT_WITHIN(0.002f, actual_db_step, max_step);

    /* Ціль досягнута в межах 30 мс ± 1 блок (256 семплів) */
    TEST_ASSERT_TRUE(reached_at > 0);
    TEST_ASSERT_TRUE(reached_at < RAMP_SAMPLES + 256);
}

/* ────────────────────────────────────────────────────────────────
 * Тест 4a: Лімітер, вхід +3 dBFS -> пік виходу <= -1 dBFS + 0.1 dB
 * ──────────────────────────────────────────────────────────────── */
static void test_limiter_ceiling(void)
{
    limiter_state_t s = {0};
    const float ceiling = powf(10.0f, -1.0f / 20.0f);    /* -1 dBFS ≈ 0.8913 */
    const float alpha_r = expf(-1.0f / (FS * 0.200f));   /* release 200 мс */

    const int N = 4096, SKIP = 480;
    const float amp_in = powf(10.0f, 3.0f / 20.0f);      /* +3 dBFS ≈ 1.4125 */
    float buf[N];
    for (int i = 0; i < N; i++)
        buf[i] = amp_in * sinf(2.0f * (float)M_PI * 100.0f * i / FS);

    dsp_limiter_process(buf, N, ceiling, alpha_r, &s);

    float peak = 0;
    for (int i = SKIP; i < N; i++) {
        float a = fabsf(buf[i]);
        if (a > peak) peak = a;
    }

    float max_allowed = powf(10.0f, (-1.0f + 0.1f) / 20.0f);  /* -0.9 dBFS ≈ 0.9016 */
    char msg[96];
    snprintf(msg, sizeof(msg), "Limiter peak = %.4f (allowed <= %.4f)", peak, max_allowed);
    TEST_ASSERT_TRUE_MESSAGE(peak <= max_allowed, msg);
}

/* ────────────────────────────────────────────────────────────────
 * Тест 4b: Лімітер, стаціонарний -3 dBFS -> варіація підсилення < 0.5 dB
 * (вхід нижче стелі -> gain = 1.0, помпування відсутнє)
 * ──────────────────────────────────────────────────────────────── */
static void test_limiter_no_pumping(void)
{
    limiter_state_t s = {0};
    const float ceiling = powf(10.0f, -1.0f / 20.0f);
    const float alpha_r = expf(-1.0f / (FS * 0.200f));

    const int N = 16000;          /* ~333 ms */
    const int SKIP = 8000;        /* прогрів */
    const float amp_in = powf(10.0f, -3.0f / 20.0f);  /* -3 dBFS ≈ 0.7079 */
    float buf[N];
    for (int i = 0; i < N; i++)
        buf[i] = amp_in * sinf(2.0f * (float)M_PI * 100.0f * i / FS);

    dsp_limiter_process(buf, N, ceiling, alpha_r, &s);

    double rms_in = 0, rms_out = 0;
    for (int i = SKIP; i < N; i++) {
        double x = amp_in * sinf(2.0 * M_PI * 100.0 * i / FS);
        rms_in  += x * x;
        rms_out += (double)buf[i] * buf[i];
    }
    rms_in  = sqrt(rms_in  / (N - SKIP));
    rms_out = sqrt(rms_out / (N - SKIP));

    double gain_db = 20.0 * log10(rms_out / rms_in);
    TEST_ASSERT_FLOAT_WITHIN(0.5, 0.0, gain_db);
}

/* ────────────────────────────────────────────────────────────────
 * Тест 5: DC-blocker, офсет 0.1 -> залишок < 0.001 після 0.5 с
 * ──────────────────────────────────────────────────────────────── */
static void test_dc_blocker(void)
{
    dc_blocker_state_t s = {0};
    float R;
    dsp_calc_dc_blocker(&R, 7.0f, FS);

    const int N = 24000;       /* 0.5 с */
    const int TAIL = 4800;     /* вимір на останніх 0.1 с */
    float buf[N];
    for (int i = 0; i < N; i++)
        buf[i] = 0.1f + 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * i / FS);

    dsp_dc_blocker_process(buf, N, R, &s);

    double mean = 0;
    for (int i = N - TAIL; i < N; i++) mean += buf[i];
    mean /= TAIL;

    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.0, fabs(mean));
}

/* ────────────────────────────────────────────────────────────────
 * Тест 6: Порядок ланцюжка — trim ДО EQ.
 * Trim +6 dB (x2.0) + EQ band 5 = -6 dB на тій самій частоті -> плоский вихід.
 * ──────────────────────────────────────────────────────────────── */
static void test_chain_order_trim_before_eq(void)
{
    const int N = 4096;
    const int WARMUP = 1024;
    const int MEASURE = N - WARMUP;

    biquad_coeffs_t coeffs[DSP_NUM_EQ_BANDS];
    biquad_state_t  states[DSP_NUM_EQ_BANDS];
    memset(states, 0, sizeof(states));

    for (int b = 0; b < DSP_NUM_EQ_BANDS; b++) {
        float g = (b == 5) ? -6.0f : 0.0f;
        dsp_calc_biquad_peaking(&coeffs[b], DSP_EQ_CENTERS[b], 1.41f, g, FS);
    }

    const float amp_in = 0.3f;
    float buf[N];
    for (int i = 0; i < N; i++)
        buf[i] = amp_in * sinf(2.0f * (float)M_PI * 1000.0f * i / FS);

    /* 1) Trim +6 dB (x2.0) — ПЕРЕД EQ */
    const float trim = 2.0f;
    for (int i = 0; i < N; i++) buf[i] *= trim;

    /* 2) EQ-ланцюжок (band 5 = -6 dB компенсує trim) */
    for (int b = 0; b < DSP_NUM_EQ_BANDS; b++)
        dsp_biquad_process(buf, N, &coeffs[b], &states[b]);

    double rms_out = 0;
    for (int i = WARMUP; i < N; i++) rms_out += (double)buf[i] * buf[i];
    rms_out = sqrt(rms_out / MEASURE);

    double rms_in = amp_in / sqrt(2.0);
    double gain   = rms_out / rms_in;

    /* Плоский вихід: gain ≈ 1.0 (±5%) */
    TEST_ASSERT_FLOAT_WITHIN(0.05, 1.0, gain);
}

/* ────────────────────────────────────────────────────────────────
 * Тест 7: Таблиці пресетів і центри смуг — байт-в-байт зі специфікацією
 * ──────────────────────────────────────────────────────────────── */
static void test_preset_tables(void)
{
    /* Еталон зі специфікації v2.2.2 */
    static const float EXPECTED[DSP_EQ_PRESET_COUNT][DSP_NUM_EQ_BANDS] = {
        /* FLAT    */ { 0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
        /* ROCK    */ { 5,  4,  3,  1,  0, -1,  0,  2,  3,  4},
        /* POP     */ {-1,  0,  1,  2,  3,  3,  2,  1,  0, -1},
        /* JAZZ    */ { 3,  2,  1,  2, -1, -1,  0,  1,  2,  3},
        /* CLASSIC */ { 4,  3,  2,  1,  0,  0, -1,  0,  2,  3},
        /* VOCAL   */ {-2, -1,  0,  2,  4,  4,  3,  2,  0, -1},
        /* NIGHT   */ { 3,  2,  1,  0,  0,  0,  0, -1, -2, -3},
    };

    for (int p = 0; p < DSP_EQ_PRESET_COUNT; p++) {
        for (int b = 0; b < DSP_NUM_EQ_BANDS; b++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Preset %d band %d", p, b);
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(EXPECTED[p][b],
                                            DSP_EQ_PRESETS[p][b], msg);
        }
    }

    static const float EXPECTED_CENTERS[DSP_NUM_EQ_BANDS] = {
        31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
        1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
    };
    for (int b = 0; b < DSP_NUM_EQ_BANDS; b++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "EQ center %d", b);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(EXPECTED_CENTERS[b],
                                        DSP_EQ_CENTERS[b], msg);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_biquad_coeffs_accuracy);
    RUN_TEST(test_eq_frequency_response);
    RUN_TEST(test_eq_adjacent_band_influence);
    RUN_TEST(test_ramp_continuity);
    RUN_TEST(test_limiter_ceiling);
    RUN_TEST(test_limiter_no_pumping);
    RUN_TEST(test_dc_blocker);
    RUN_TEST(test_chain_order_trim_before_eq);
    RUN_TEST(test_preset_tables);
    return UNITY_END();
}