// Файл: components/ui/Kconfig
menu "Audio controller UI"

    choice APP_UI_ORIENT
        prompt "Display mounting orientation (ST7789 170x320, landscape)"
        default APP_UI_ORIENT_BENCH
        help
            Орієнтація монтування панелі. Обидва варіанти ландшафтні
            (swap_xy=1); відрізняються mirror-комбінацією під фізичний
            монтаж корпусу. Перемикається без правки коду; на bring-up
            додатково доступне runtime-перемикання командою
            "lcdtest orient <swap> <mx> <my>".

        config APP_UI_ORIENT_BENCH
            bool "Bench mount: swap=1, mirror=(0,1)"
        config APP_UI_ORIENT_FINAL
            bool "Final mount: swap=1, mirror=(1,0)"
    endchoice

endmenu