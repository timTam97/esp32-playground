idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_peer esp32-camera esp_wifi esp_event esp_netif nvs_flash
             esp_timer esp_driver_uart json
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror=return-type)
