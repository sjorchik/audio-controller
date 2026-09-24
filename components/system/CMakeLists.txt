idf_component_register(
    SRCS "system.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common log
    # console = esp_console.h + argtable3. Код REPL у system.c сидить під
    # #ifdef CONFIG_APP_DEV_CONSOLE, тому у дефолтній збірці (n) об'єкти
    # компонента console не лінкуються (static lib: unused objects не
    # потрапляють у flash). Критерій 5 це не порушує.
    PRIV_REQUIRES driver tda7318 console
)