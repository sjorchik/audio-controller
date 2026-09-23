/*
 * dsp_math.h — платформо-незалежний модуль чистої DSP-математики.
 *
 * БЕЗ esp-заголовків: може бігатися на host у юніт-тестах.
 */

#pragma once

#include <stdint.h>

typedef struct { float b0, b1, b2, a1, a2; } biquad_coeffs_t;
typedef struct { float x1, x2, y1, y2; }     biquad_state_t;
typedef struct { float x_prev, y_prev; }     dc_blocker_state_t;
typedef struct {
    float current_gain_lin;
    float target_gain_lin;
    float multiplier;
} volume_ramp_state_t;
typedef struct { float envelope; }           limiter_state_t;

/* Peaking biquad (RBJ Audio EQ Cookbook, Q=1.41 для графічного EQ). */
void dsp_calc_biquad_peaking(biquad_coeffs_t *c, float freq, float q,
                             float gain_db, float fs);
void dsp_biquad_process(float *buf, uint32_t frames,
                        const biquad_coeffs_t *c, biquad_state_t *s);

/* DC-blocker 1-го порядку: y[n] = x[n] - x[n-1] + R*y[n-1], R=exp(-2πfc/fs). */
void dsp_calc_dc_blocker(float *R, float fc, float fs);
void dsp_dc_blocker_process(float *buf, uint32_t frames, float R,
                            dc_blocker_state_t *s);

/* Volume ramp: обчислює multiplier для лінійного (у dB) переходу
 * за 30 мс. Викликається при зміні volume / mute. */
void dsp_update_ramp(volume_ramp_state_t *s, float current_db,
                     float target_db, float fs);

/* Лімітер: feed-forward envelope follower. Стеля -1 dBFS,
 * attack 1 мс, release 200 мс. */
void dsp_limiter_process(float *buf, uint32_t frames, float ceiling_lin,
                         float alpha_a, float alpha_r, limiter_state_t *s);