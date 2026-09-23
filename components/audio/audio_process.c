/*
 * audio_process.c — DSP-стан та реалізація API з audio.h.
 *
 * Керує двома копіями конфігурації (double-buffering) і атомарно
 * підмінює вказівник s_active. Hot-path читає лише вказівник на
 * початку блоку — жодних м'ютексів у audio_process_block().
 *
 * Стани фільтрів (s_dc, s_eq, s_ramp, s_lim) — статичні, у BSS,
 * в internal RAM. Жодних динамічних алокацій у process path.
 */

#include "audio.h"                 /* публічне API (volume, EQ, preset, trim) */
#include "audio_process.h"         /* audio_process_init / audio_process_block */
#include "dsp_math.h"              /* чиста DSP-математика */
#include "dsp_presets.h"           /* центри смуг і таблиці пресетів */
#include <stdatomic.h>
#include <string.h>
#include <math.h>

#define AUDIO_SAMPLE_RATE  48000.0f
#define DSP_MAX_SOURCES    BSP_AUDIO_SOURCE_MAX

/* ────────────────────────────────────────────────────────────────
 * Конфігурація (подвійне буферування, lock-free swap)
 * ──────────────────────────────────────────────────────────────── */
typedef struct {
    biquad_coeffs_t    eq_coeffs[DSP_NUM_EQ_BANDS];
    float              eq_gain_db[DSP_NUM_EQ_BANDS];  /* для audio_get_eq() */
    float              trim_gains[DSP_MAX_SOURCES];
    float              volume_db;
    bool               mute;
    bsp_audio_source_t active_source;
    audio_eq_preset_t  current_preset;
} audio_config_t;

static audio_config_t s_config_a, s_config_b;
static _Atomic(audio_config_t *) s_active = &s_config_a;
static _Atomic(audio_config_t *) s_shadow = &s_config_b;

/* ── Стани фільтрів (статичні, у BSS, internal RAM) ── */
static dc_blocker_state_t   s_dc[2];
static biquad_state_t       s_eq[DSP_NUM_EQ_BANDS][2];
static volume_ramp_state_t  s_ramp[2];
static limiter_state_t      s_lim[2];

/* ── Робочі буфери (static, internal RAM) ── */
static float s_buf_l[DSP_BLOCK_FRAMES];
static float s_buf_r[DSP_BLOCK_FRAMES];

/* ── Коефіцієнти, обчислені один раз в init ── */
static float s_R_dc          = 0.0f;
static float s_alpha_release = 0.0f;
static float s_ceiling_lin   = 0.0f;

/* ────────────────────────────────────────────────────────────────
 * Допоміжні функції (тільки control path)
 * ──────────────────────────────────────────────────────────────── */

/* Атомарний swap: shadow стає active, стара active — shadow. */
static void config_commit(void)
{
    audio_config_t *shd = atomic_load_explicit(&s_shadow, memory_order_relaxed);
    audio_config_t *act = atomic_exchange_explicit(&s_active, shd, memory_order_acq_rel);
    atomic_store_explicit(&s_shadow, act, memory_order_relaxed);
}

static void config_update_ramps(audio_config_t *cfg)
{
    float target_db = cfg->mute ? -90.0f : cfg->volume_db;
    /* Захист від log(0): gain < 1e-10 (mute-старт) підтягуємо до 1e-10 */
    float cl = (s_ramp[0].current_gain_lin < 1e-10f) ? 1e-10f : s_ramp[0].current_gain_lin;
    float cr = (s_ramp[1].current_gain_lin < 1e-10f) ? 1e-10f : s_ramp[1].current_gain_lin;

    dsp_update_ramp(&s_ramp[0], 20.0f * log10f(cl), target_db, AUDIO_SAMPLE_RATE);
    dsp_update_ramp(&s_ramp[1], 20.0f * log10f(cr), target_db, AUDIO_SAMPLE_RATE);
}

static void config_apply_preset(audio_config_t *cfg, audio_eq_preset_t preset)
{
    for (int i = 0; i < DSP_NUM_EQ_BANDS; i++) {
        cfg->eq_gain_db[i] = DSP_EQ_PRESETS[preset][i];
        dsp_calc_biquad_peaking(&cfg->eq_coeffs[i], DSP_EQ_CENTERS[i], 1.41f,
                                cfg->eq_gain_db[i], AUDIO_SAMPLE_RATE);
    }
    cfg->current_preset = preset;
}

/* ────────────────────────────────────────────────────────────────
 * Ініціалізація (викликається один раз з audio_init())
 * ──────────────────────────────────────────────────────────────── */
void audio_process_init(float fs)
{
    /* Постійні коефіцієнти */
    dsp_calc_dc_blocker(&s_R_dc, 7.0f, fs);
    s_ceiling_lin   = powf(10.0f, -1.0f / 20.0f);     /* -1 dBFS у лінійному */
    s_alpha_release = expf(-1.0f / (fs * 0.200f));    /* release 200 мс */

    memset(&s_config_a, 0, sizeof(s_config_a));
    memset(&s_config_b, 0, sizeof(s_config_b));

    /* Стани за замовчуванням (специфікація v2.2.2):
     * volume = -20 dB, trim усіх джерел = 0 dB, EQ = Flat, mute = false */
    s_config_a.volume_db = s_config_b.volume_db = -20.0f;
    s_config_a.mute      = s_config_b.mute      = false;
    s_config_a.active_source = s_config_b.active_source = BSP_AUDIO_SOURCE_TV_BOX;

    config_apply_preset(&s_config_a, AUDIO_EQ_PRESET_FLAT);
    config_apply_preset(&s_config_b, AUDIO_EQ_PRESET_FLAT);

    for (int i = 0; i < DSP_MAX_SOURCES; i++) {
        s_config_a.trim_gains[i] = s_config_b.trim_gains[i] = 1.0f;
    }

    /* Початковий gain ramp-у = -20 dB */
    float init_lin = powf(10.0f, -20.0f / 20.0f);
    s_ramp[0].current_gain_lin = s_ramp[1].current_gain_lin = init_lin;
    s_ramp[0].target_gain_lin  = s_ramp[1].target_gain_lin  = init_lin;
    s_ramp[0].multiplier       = s_ramp[1].multiplier       = 1.0f;

    config_update_ramps(&s_config_a);
}

/* ────────────────────────────────────────────────────────────────
 * HOT-PATH: обробка одного DMA-блоку (256 стерео-фреймів).
 * Порядок строгий: DC-blocker -> trim -> EQ -> volume ramp -> limiter.
 * Жодних алокацій, жодних логів, жодних м'ютексів.
 * ──────────────────────────────────────────────────────────────── */
void audio_process_block(int32_t *in_buf, int32_t *out_buf, uint32_t frames)
{
    /* Lock-free зчитування активної конфігурації (один раз на блок) */
    audio_config_t *cfg = atomic_load_explicit(&s_active, memory_order_acquire);

    /* Деінтерлівінг + конвертація int32 (24-bit MSB-aligned) -> float [-1..+1] */
    const float scale_in = 1.0f / 2147483648.0f;
    for (uint32_t i = 0; i < frames; i++) {
        s_buf_l[i] = (float)in_buf[i * 2]     * scale_in;
        s_buf_r[i] = (float)in_buf[i * 2 + 1] * scale_in;
    }

    /* 1) DC-blocker (per channel) */
    dsp_dc_blocker_process(s_buf_l, frames, s_R_dc, &s_dc[0]);
    dsp_dc_blocker_process(s_buf_r, frames, s_R_dc, &s_dc[1]);

    /* 2) Per-source trim (ДО EQ, згідно специфікації) */
    float trim = cfg->trim_gains[cfg->active_source];
    for (uint32_t i = 0; i < frames; i++) {
        s_buf_l[i] *= trim;
        s_buf_r[i] *= trim;
    }

    /* 3) 10-смуговий графічний EQ (послідовні peaking biquad) */
    for (int b = 0; b < DSP_NUM_EQ_BANDS; b++) {
        dsp_biquad_process(s_buf_l, frames, &cfg->eq_coeffs[b], &s_eq[b][0]);
        dsp_biquad_process(s_buf_r, frames, &cfg->eq_coeffs[b], &s_eq[b][1]);
    }

    /* 4) Volume з ramp 30 мс (лінійний у dB-домені, per-sample) */
    float gl = s_ramp[0].current_gain_lin, tl = s_ramp[0].target_gain_lin, ml = s_ramp[0].multiplier;
    float gr = s_ramp[1].current_gain_lin, tr = s_ramp[1].target_gain_lin, mr = s_ramp[1].multiplier;

    for (uint32_t i = 0; i < frames; i++) {
        /* L */
        if (ml == 1.0f)   gl = tl;
        else if (gl < tl) { gl *= ml; if (gl > tl) gl = tl; }
        else if (gl > tl) { gl /= ml; if (gl < tl) gl = tl; }
        s_buf_l[i] *= gl;

        /* R */
        if (mr == 1.0f)   gr = tr;
        else if (gr < tr) { gr *= mr; if (gr > tr) gr = tr; }
        else if (gr > tr) { gr /= mr; if (gr < tr) gr = tr; }
        s_buf_r[i] *= gr;
    }
    s_ramp[0].current_gain_lin = gl;
    s_ramp[1].current_gain_lin = gr;

    /* 5) Лімітер (стеля -1 dBFS, peak-catch attack, release 200 мс) */
    dsp_limiter_process(s_buf_l, frames, s_ceiling_lin, s_alpha_release, &s_lim[0]);
    dsp_limiter_process(s_buf_r, frames, s_ceiling_lin, s_alpha_release, &s_lim[1]);

    /* Конвертація float -> int32 (24-bit MSB-aligned) + soft-clip для безпеки */
    for (uint32_t i = 0; i < frames; i++) {
        float sl = (s_buf_l[i] >  1.0f) ?  1.0f : (s_buf_l[i] < -1.0f ? -1.0f : s_buf_l[i]);
        float sr = (s_buf_r[i] >  1.0f) ?  1.0f : (s_buf_r[i] < -1.0f ? -1.0f : s_buf_r[i]);

        out_buf[i * 2]     = ((int32_t)(sl * 2147483647.0f)) & 0xFFFFFF00;
        out_buf[i * 2 + 1] = ((int32_t)(sr * 2147483647.0f)) & 0xFFFFFF00;
    }
}

/* ────────────────────────────────────────────────────────────────
 * API (викликається з ctrl-задачі, потокобезпечно через commit)
 * Реалізація оголошень з audio.h
 * ──────────────────────────────────────────────────────────────── */

void audio_set_volume_db(float db)
{
    if (db < -90.0f) db = -90.0f;
    if (db >   0.0f) db =   0.0f;
    audio_config_t *shd = atomic_load_explicit(&s_shadow, memory_order_relaxed);
    shd->volume_db = db;
    config_update_ramps(shd);
    config_commit();
}

float audio_get_volume_db(void)
{
    audio_config_t *cfg = atomic_load_explicit(&s_active, memory_order_acquire);
    return cfg->volume_db;
}

void audio_set_mute(bool mute)
{
    audio_config_t *shd = atomic_load_explicit(&s_shadow, memory_order_relaxed);
    shd->mute = mute;
    config_update_ramps(shd);
    config_commit();
}

void audio_set_eq_band(uint8_t idx, float db)
{
    if (idx >= DSP_NUM_EQ_BANDS) return;
    if (db < -12.0f) db = -12.0f;
    if (db >  12.0f) db =  12.0f;
    audio_config_t *shd = atomic_load_explicit(&s_shadow, memory_order_relaxed);
    shd->eq_gain_db[idx] = db;
    dsp_calc_biquad_peaking(&shd->eq_coeffs[idx], DSP_EQ_CENTERS[idx], 1.41f,
                            db, AUDIO_SAMPLE_RATE);
    shd->current_preset = AUDIO_EQ_PRESET_MAX;  /* кастомний стан, не пресет */
    config_commit();
}

void audio_get_eq(float out[10])
{
    audio_config_t *cfg = atomic_load_explicit(&s_active, memory_order_acquire);
    for (int i = 0; i < DSP_NUM_EQ_BANDS; i++) {
        out[i] = cfg->eq_gain_db[i];
    }
}

void audio_set_preset(audio_eq_preset_t preset)
{
    if (preset >= AUDIO_EQ_PRESET_MAX) return;
    audio_config_t *shd = atomic_load_explicit(&s_shadow, memory_order_relaxed);
    config_apply_preset(shd, preset);
    config_commit();
}

audio_eq_preset_t audio_get_preset(void)
{
    audio_config_t *cfg = atomic_load_explicit(&s_active, memory_order_acquire);
    return cfg->current_preset;
}

void audio_set_source(bsp_audio_source_t source)
{
    if (source >= DSP_MAX_SOURCES) return;
    audio_config_t *shd = atomic_load_explicit(&s_shadow, memory_order_relaxed);
    shd->active_source = source;
    config_commit();
}

void audio_set_source_trim_db(bsp_audio_source_t source, float db)
{
    if (source >= DSP_MAX_SOURCES) return;
    if (db < -12.0f) db = -12.0f;
    if (db >   6.0f) db =   6.0f;
    audio_config_t *shd = atomic_load_explicit(&s_shadow, memory_order_relaxed);
    shd->trim_gains[source] = powf(10.0f, db / 20.0f);
    config_commit();
}

float audio_get_source_trim_db(bsp_audio_source_t source)
{
    if (source >= DSP_MAX_SOURCES) return 0.0f;
    audio_config_t *cfg = atomic_load_explicit(&s_active, memory_order_acquire);
    return 20.0f * log10f(cfg->trim_gains[source]);
}