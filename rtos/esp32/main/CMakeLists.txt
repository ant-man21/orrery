idf_component_register(
    SRCS "main.c" "stepper.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_task_wdt
)
