idf_component_register(
    SRCS 
        "main.c"
        "app/turnout_storage.c"
        "app/turnout_manager.c"
        "app/lcc_node.cpp"
        "app/screen_timeout.c"
        "app/bootloader_hal.cpp"
        "app/bootloader_display.c"
        "ui/ui_common.c"
        "ui/ui_main.c"
        "ui/ui_turnouts.c"
        "ui/ui_add_turnout.c"
    INCLUDE_DIRS 
        "."
        "app"
        "ui"
    REQUIRES
        driver
        esp_lcd
        esp_timer
        fatfs
        nvs_flash
        board_drivers
        espressif__esp_jpeg
        lvgl__lvgl
        json
        OpenMRN
        app_update
)

# Set C++ standard for OpenMRN compatibility
set_source_files_properties(
    "app/lcc_node.cpp"
    "app/bootloader_hal.cpp"
    PROPERTIES COMPILE_FLAGS "-std=gnu++14 -fno-strict-aliasing"
)
