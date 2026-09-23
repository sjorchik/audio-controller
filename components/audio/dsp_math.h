/*
 * dsp_math.h — платформо-незалежний модуль чистої DSP-математики.
 * БЕЗ esp-заголовків: юніт-тести бігаються на host (pio test -e native).
 */

#pragma once

#include <stdint.h>

/* Спільні константи DSP-ланцюжка.
 * Guard-и: audio_process.h також оголошує ці макроси ідентично —
 * перевизначення однаковим тілом допустиме, guard прибирає попередження. */
#ifndef DSP_NUM_EQ_BANDS
#define DSP_NUM_EQ_BANDS  10
#endif

#ifndef DSP_BLOCK_FRAMES
#define DSP_BLOCK_FRAMES  256
#endif

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

/* DC-blocker 1-го порядку: y[n] = x[n] - x[n-1] + R*y[n-1], R=exp(-2*pi*fc/fs). */
void dsp_calc_dc_blocker(float *R, float fc, float fs);
void dsp_dc_blocker_process(float *buf, uint32_t frames, float R,
                            dc_blocker_state_t *s);

/* Volume ramp: обчислює multiplier для лінійного (у dB) переходу за 30 мс.
 * Викликається ТІЛЬКИ при зміні volume / mute (не в hot-path). */
void dsp_update_ramp(volume_ramp_state_t *s, float current_db,
                     float target_db, float fs);

/* Лімітер: feed-forward envelope follower з peak-catch attack.
 * Стеля -1 dBFS; attack миттєвий (0 мс, задовольняє "attack <= 1 мс");
 * release 200 мс. Інваріанта env >= |x| гарантує: вихід НИКОЛИ
 * не перевищує стелю (без перерегулювання, без клацань). */
void dsp_limiter_process(float *buf, uint32_t frames, float ceiling_lin,
                         float alpha_r, limiter_state_t *s);