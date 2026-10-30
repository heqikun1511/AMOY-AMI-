idf_component_register(
    SRCS "can_recovery_main.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash app_update esp_hw_support
)
