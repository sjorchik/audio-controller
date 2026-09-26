[33mcommit ca5cf80775f384ccb0f16d3ea6cdc1a4121f8df7[m[33m ([m[1;36mHEAD[m[33m -> [m[1;32mmain[m[33m)[m
Author: sjorchik <sjorchik@gmail.com>
Date:   Fri Sep 25 14:56:24 2026 +0300

    feat(input): buttons + encoder subsystem (GPIO ISR, PCNT, debounce SM, evmon)
    
    - Roadmap item: 4 (input)
    - Status: accepted
    - Buttons: 6 + encoder btn; ISR both edges -> queue (pin, level, esp_timer ts);
      debounce + SM only in input task (core 0, prio 10); periodic 4 ms level
      sampling drives hold timers (LONG 600 ms; REPEAT 250 ms after 1000 ms;
      POWER/OK without repeat)
    - button_sm: platform-independent SM (host-testable); deferred SHORT after
      stable release-debounce => exactly 1 event per press under contact bounce
    - Encoder: PCNT quadrature 2x (A=edge, B=level); hw glitch filter 10 us
      (S3 HW limit ~12.7 us); symmetric limits +/-1000 (fixes CCW wrap at
      low_limit=-1); delta read + clear every 8 ms + accumulator => no step loss
    - Events: system_post(SYSTEM_EVENT_INPUT, input_event_t{source, action, arg, ts})
    - Dev console: evmon on|off + enccal <n>|stop|off (dev builds only,
      CONFIG_APP_DEV_CONSOLE); absent in prod
    - Native tests: 15/15 passed (button_sm 6/6, dsp_math 9/9 regression)
    - Latency budget: ~26 ms from physical edge (ISR-timestamped)
    - Deviations: bench encoder has intermittent dead clicks (hw defect);
      criterion 2 final check on production encoder sample; CPD default 2,
      re-measure via enccal on production unit

 components/input/CMakeLists.txt          |   6 [32m+[m[31m-[m
 components/input/button_sm.c             | 117 [32m++++++++++[m
 components/input/include/button_sm.h     |  62 [32m++++++[m
 components/input/include/input.h         |  37 [32m++[m[31m--[m
 components/input/input.c                 | 353 [32m++++++++++++++++++++++++++++++[m[31m-[m
 components/system/include/system.h       |  37 [32m+++[m[31m-[m
 components/system/system.c               |  72 [32m++++[m[31m---[m
 platformio.ini                           |  16 [32m+[m[31m-[m
 sdkconfig.defaults                       |   2 [32m+[m[31m-[m
 src/app_main.c                           |   5 [32m+[m[31m-[m
 test/test_button_sm/test_button_sm.c     | 123 [32m+++++++++++[m
 test/{ => test_dsp_math}/test_dsp_math.c |   0
 12 files changed, 765 insertions(+), 65 deletions(-)
