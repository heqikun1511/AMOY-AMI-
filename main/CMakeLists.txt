idf_component_register(
    SRC_DIRS 
        "."
        "APP" 
        "ui"
        "BSP" 
    EXCLUDE_SRCS
        "rotary_input.c"
        "ws2815_rmt_encoder.c"
    INCLUDE_DIRS 
        "."
        "APP"
        "ui"
        "BSP"
    REQUIRES
        driver
        nvs_flash
        esp_timer
        BSP)

# Compatibility implementations for EEZ native variables referenced by screens.c.
target_sources(${COMPONENT_LIB} PRIVATE "ui_compat.c")
