idf_component_register(
    SRCS "esp_button.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer freertos esp_at
)
