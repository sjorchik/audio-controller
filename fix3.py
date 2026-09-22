import re

rep = []

# ---------- 1) src/app_main.c ----------
p = "src/app_main.c"
src = open(p, encoding="utf-8").read()
orig = src

# 1a) include audio.h, якщо немає
if '#include "audio.h"' not in src:
    anchor = '#include "power.h"' if '#include "power.h"' in src else '#include "bsp.h"'
    src = src.replace(anchor, anchor + '\n#include "audio.h"', 1)
    rep.append("app_main: додано #include \"audio.h\"")

# 1b) audio_task делегує в audio_task_entry
if "audio_task_entry" not in src:
    new_task = ("static void audio_task(void *arg)\n{\n    (void)arg;\n"
                "    audio_task_entry(arg);\n}\n")
    src, n = re.subn(r"static void audio_task\(void \*arg\)\s*\{.*?\n\}\n",
                     new_task, src, count=1, flags=re.S)
    rep.append("app_main: audio_task делегує audio_task_entry" if n else
               "УВАГА: тіло audio_task не знайдено!")

# 1c) audio_init() одразу після power_init() (fallback: перед tda7318_init)
if "audio_init();" not in src:
    src, n = re.subn(r"^([ \t]*)(\w+\(power_init\(\)\);)$",
                     lambda m: m.group(0) + "\n" + m.group(1) + "ESP_ERROR_CHECK(audio_init());",
                     src, count=1, flags=re.M)
    if not n:
        src, n = re.subn(r"^([ \t]*)(\w+\(tda7318_init\(\)\);)$",
                         lambda m: m.group(1) + "ESP_ERROR_CHECK(audio_init());\n" + m.group(0),
                         src, count=1, flags=re.M)
    rep.append("app_main: додано ESP_ERROR_CHECK(audio_init());" if n else
               "УВАГА: не знайдено місце для audio_init()!")

# 1d) audio_pipeline_start() в кінці app_main (після створення задач і power_set_state)
if "audio_pipeline_start();" not in src:
    i = src.rfind("\n}")
    if i != -1:
        src = src[:i] + "\n    ESP_ERROR_CHECK(audio_pipeline_start());" + src[i:]
        rep.append("app_main: додано ESP_ERROR_CHECK(audio_pipeline_start()); в кінці app_main")
    else:
        rep.append("УВАГА: не знайдено кінець app_main!")

if src != orig:
    open(p, "w", encoding="utf-8").write(src)

# ---------- 2) components/audio/audio.c: чесний CPU load ----------
p = "components/audio/audio.c"
a = open(p, encoding="utf-8").read()
orig_a = a

if "esp_timer.h" not in a:
    a = a.replace("#include <string.h>", '#include <string.h>\n#include "esp_timer.h"', 1)
    rep.append("audio.c: додано #include \"esp_timer.h\"")

if "s_busy_us_ema" not in a:
    a = a.replace("static uint32_t s_stats_cpu_load = 0;",
                  "static uint32_t s_stats_cpu_load = 0;\nstatic int64_t s_busy_us_ema = 0;", 1)
    a = a.replace("            audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, 1.0f);",
                  "            int64_t t_work = esp_timer_get_time();\n"
                  "            audio_process_block(rx_buf, tx_buf, AUDIO_BLOCK_FRAMES, 1.0f);", 1)
    a, n = re.subn(r"[ \t]*s_stats_cpu_load = 5;\s*\n",
                   "            {\n"
                   "                int64_t dt = esp_timer_get_time() - t_work;\n"
                   "                s_busy_us_ema = (s_busy_us_ema * 7 + dt * 3) / 10;\n"
                   "                s_stats_cpu_load = (uint32_t)((s_busy_us_ema * 100) /\n"
                   "                        (AUDIO_BLOCK_FRAMES * 1000000 / BSP_AUDIO_SAMPLE_RATE_HZ));\n"
                   "            }\n", a, count=1)
    rep.append("audio.c: CPU load тепер вимірюється (EMA зайнятого часу блоку)" if n else
               "УВАГА: не знайдено рядок 's_stats_cpu_load = 5;' — вимір не додано")

if a != orig_a:
    open(p, "w", encoding="utf-8").write(a)

# ---------- 3) CMake: esp_timer для audio ----------
p = "components/audio/CMakeLists.txt"
c = open(p, encoding="utf-8").read()
if "esp_timer" not in c:
    c = c.replace("esp_driver_i2s esp_pm)", "esp_driver_i2s esp_pm esp_timer)", 1)
    open(p, "w", encoding="utf-8").write(c)
    rep.append("audio CMakeLists: додано esp_timer")

print("Звіт:")
for r in rep:
    print(" -", r)
print("\n--- хвіст src/app_main.c (перевірка) ---")
print("\n".join(open("src/app_main.c", encoding="utf-8").read().splitlines()[-20:]))