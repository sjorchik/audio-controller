# Файл: components/audio/CMakeLists.txt
idf_component_register(
    SRCS "audio.c" "audio_process.c" "dsp_math.c"
    INCLUDE_DIRS "include" "."
    REQUIRES bsp power driver esp_timer
    # system.h потрібен audio_process.c для broadcast-події
    # SYSTEM_EVT_AUDIO_STATE після будь-якого сетера (архітектура v2.2.5,
    # дорога карта v2.2.1 пункт 5). Тільки .c-файли інклюдять system.h,
    # тому достатньо PRIV_REQUIRES.
    PRIV_REQUIRES system
)