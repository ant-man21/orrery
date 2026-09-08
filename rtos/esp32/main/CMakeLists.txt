idf_component_register(
    SRCS "main.c" "stepper.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)
# esp_task_wdt.h lives in the esp_system component (not a separate
# "esp_task_wdt" component) -- esp_system is one of the components ESP-IDF
# links into every target by default, so no REQUIRES entry is needed for it.
