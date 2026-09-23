/*
 * dsp_math.c — платформо-незалежна чиста DSP-математика.
 * БЕЗ esp-заголовків: юніт-тести бігаються на host.
 */

#include "dsp_math.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ────────────────────────────────────────────────────────────────
 * Peaking biquad за RBJ Audio EQ Cookbook
 * ──────────────────────────────────────────────────────────────── */
void dsp_calc_biquad_peaking(biquad_coeffs_t *c, float freq, float q,
                             float gain_db, float fs)
{
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * (float)M_PI * freq / fs;
    float alpha = sinf(w0) / (2.0f * q);

    float b0 =  1.0f + alpha * A;
    float b1 = -2.0f * cosf(w0);
    float b2 =  1.0f - alpha * A;
    float a0 =  1.0f + alpha / A;
    float a1 = -2.0f * cosf(w0);
    float a2 =  1.0f - alpha / A;

    c->b0 = b0 / a0;  c->b1 = b1 / a0;  c->b2 = b2 / a0;
    c->a1 = a1 / a0;  c->a2 = a2 / a0;
}

/* Direct Form I: найстабільніший до стрибків коефіцієнтів та float-точності. */
void dsp_biquad_process(float *buf, uint32_t frames,
                        const biquad_coeffs_t *c, biquad_state_t *s)
{
    float x1 = s->x1, x2 = s->x2, y1 = s->y1, y2 = s->y2;
    float b0 = c->b0, b1 = c->b1, b2 = c->b2, a1 = c->a1, a2 = c->a2;

    for (uint32_t i = 0; i < frames; i++) {
        float x = buf[i];
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        buf[i] = y;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
    }
    s->x1 = x1; s->x2 = x2; s->y1 = y1; s->y2 = y2;
}

/* ────────────────────────────────────────────────────────────────
 * DC-blocker: y[n] = x[n] - x[n-1] + R*y[n-1], R = exp(-2*pi*fc/fs)
 * ──────────────────────────────────────────────────────────────── */
void dsp_calc_dc_blocker(float *R, float fc, float fs)
{
    *R = expf(-2.0f * (float)M_PI * fc / fs);
}

void dsp_dc_blocker_process(float *buf, uint32_t frames, float R,
                            dc_blocker_state_t *s)
{
    float x_prev = s->x_prev;
    float y_prev = s->y_prev;
    for (uint32_t i = 0; i < frames; i++) {
        float x = buf[i];
        float y = x - x_prev + R * y_prev;
        buf[i]  = y;
        x_prev  = x;
        y_prev  = y;
    }
    s->x_prev = x_prev; s->y_prev = y_prev;
}

/* ────────────────────────────────────────────────────────────────
 * Volume ramp: лінійний у dB-домені, 30 мс.
 * У hot-path gain оновлюється множенням на multiplier (без pow/log).
 * ──────────────────────────────────────────────────────────────── */
void dsp_update_ramp(volume_ramp_state_t *s, float current_db,
                     float target_db, float fs)
{
    s->target_gain_lin = powf(10.0f, target_db / 20.0f);
    float samples = 0.030f * fs;          /* 30 мс */
    if (samples < 1.0f) samples = 1.0f;
    float db_diff = fabsf(target_db - current_db);

    if (db_diff < 0.01f) {
        s->multiplier = 1.0f;             /* вже на цілі */
    } else {
        float db_step = db_diff / samples;
        s->multiplier = powf(10.0f, db_step / 20.0f);
    }
}

/* ────────────────────────────────────────────────────────────────
 * Лімітер: peak-catch attack + release 200 мс.
 *
 * Чому peak-catch, а не one-pole attack: однополюсний attack (tau=1 мс)
 * лишає envelope на ~2.4% нижче піку синуса у сталому режимі, що дає
 * перерегулювання +0.23 dB НАД стелею (баг, спійманий тестом 4a).
 * Peak-catch дає інваріанту env >= |x| для кожного семпла, отже
 * gain = ceiling/env <= ceiling/|x| і вихід ЗАВЖДИ <= стелі.
 * Attack = 0 мс (задовольняє "attack <= 1 мс"), release плавний —
 * без клацань і без помпування (на -3 dBFS env < стелі, gain = 1).
 * ──────────────────────────────────────────────────────────────── */
void dsp_limiter_process(float *buf, uint32_t frames, float ceiling_lin,
                         float alpha_r, limiter_state_t *s)
{
    float env = s->envelope;
    for (uint32_t i = 0; i < frames; i++) {
        float abs_x = fabsf(buf[i]);
        if (abs_x > env) {
            env = abs_x;                              /* attack: миттєвий */
        } else {
            env = abs_x + alpha_r * (env - abs_x);    /* release: 200 мс */
        }
        buf[i] *= (env > ceiling_lin) ? (ceiling_lin / env) : 1.0f;
    }
    s->envelope = env;
}