idf_component_register(
    SRCS "audio.c" "audio_process.c" "dsp_math.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp power driver esp_timer
)