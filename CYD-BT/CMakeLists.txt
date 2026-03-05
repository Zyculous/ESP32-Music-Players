if(NOT DEFINED CYD_ENABLE_TOUCH)
    set(CYD_ENABLE_TOUCH ON)
endif()

if(NOT DEFINED CYD_ENABLE_IMAGE_LOADING)
    set(CYD_ENABLE_IMAGE_LOADING ON)
endif()

set(CYD_SRCS
        "main.c"
        "bluetooth_handler.c"
        "display_handler.c"
        "audio_handler.c"
        "ui_handler.c")

set(CYD_REQUIRES
        nvs_flash
        bt
        esp_lcd
        driver
        esp_driver_i2s
        esp_driver_spi
        esp_driver_gpio)

if(CYD_ENABLE_TOUCH)
    list(APPEND CYD_SRCS "touch_handler.c")
    list(APPEND CYD_REQUIRES esp_driver_i2c)
endif()

if(CYD_ENABLE_IMAGE_LOADING)
    list(APPEND CYD_REQUIRES esp_jpeg)
endif()

idf_component_register(SRCS ${CYD_SRCS}
                                            INCLUDE_DIRS "."
                                            REQUIRES ${CYD_REQUIRES})

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CYD_FEATURE_TOUCH=$<BOOL:${CYD_ENABLE_TOUCH}>
    CYD_FEATURE_IMAGE_LOADING=$<BOOL:${CYD_ENABLE_IMAGE_LOADING}>
)