# 仅编译 mini-bot 使用的应用、外设和网络组件。
set(BOARD_DIR "deepbot-s3-cam-n16r8")

set(SOURCES
    "main.cc"
    "application.cc"
    "application_incoming_json.cc"
    "application_ota.cc"
    "assets.cc"
    "device_state_machine.cc"
    "mcp_server.cc"
    "ota.cc"
    "settings.cc"
    "system_info.cc"
    "audio/audio_codec.cc"
    "audio/audio_debugger.cc"
    "audio/audio_service.cc"
    "audio/codecs/no_audio_codec.cc"
    "audio/demuxer/ogg_demuxer.cc"
    "audio/engines/afe_audio_engine.cc"
    "audio/wake_words/custom_wake_word.cc"
    "audio/wake_words/wake_word_audio_cache.cc"
    "notify/notify_player.cc"
    "display/display.cc"
    "display/text_glyph.cc"
    "display/oled_display.cc"
    "display/deepbot_eyes_display.cc"
    "display/deepbot_eyes_music.cc"
    "display/lvgl_display/lvgl_display.cc"
    "display/lvgl_display/emoji_collection.cc"
    "display/lvgl_display/lvgl_theme.cc"
    "display/lvgl_display/lvgl_font.cc"
    "display/lvgl_display/dynamic_glyph_cache.cc"
    "display/lvgl_display/lvgl_image.cc"
    "display/lvgl_display/jpg/image_to_jpeg.cpp"
    "protocols/protocol.cc"
    "protocols/text_glyph_payload.cc"
    "protocols/mqtt_protocol.cc"
    "protocols/websocket_protocol.cc"
    "protocols/custom_server_audio.cc"
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "boards/common/backlight.cc"
    "boards/common/ov_camera_watcher.cc"
)
# 板级目录新增的 .cc 自动参与编译。
file(GLOB BOARD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/*.cc)
list(APPEND SOURCES ${BOARD_SOURCES})

set(INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg"
                 "audio" "audio/demuxer" "protocols" "boards/common")

# 128x64 单色 OLED：14px/1bpp 字体，不带彩色 emoji（眼睛表情由几何图形绘制）。
set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)
set(BUILTIN_ICON_FONT font_material_symbols_14_1)
set(TEXT_FONT_SIZE 14)
set(TEXT_FONT_BPP 1)

# 组件依赖收集阶段（CMAKE_BUILD_EARLY_EXPANSION）里 CMAKE_CURRENT_SOURCE_DIR
# 指向 build 临时目录，config.json 还不可读；守住后用目录名兜底，正式配置阶段再读。
set(BOARD_TYPE "${BOARD_DIR}")
set(BOARD_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/config.json")
if(EXISTS "${BOARD_CONFIG_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BOARD_CONFIG_FILE}")
    file(READ "${BOARD_CONFIG_FILE}" BOARD_CONFIG_JSON)
    string(REGEX MATCH "\"type\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" _type_match "${BOARD_CONFIG_JSON}")
    if(CMAKE_MATCH_1)
        set(BOARD_TYPE "${CMAKE_MATCH_1}")
    endif()
endif()
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()

# 只带中文资源；zh-CN 已覆盖 en-US 的全部字符串和提示音。
set(LANG_DIR "zh-CN")
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES
                        esp_pm
                        esp_psram
                        esp_http_client # OV 网关同步 HTTP；组件依赖在 sdkconfig 条件生效前收集。
                        esp_netif
                        esp_driver_gpio
                        esp_driver_uart
                        esp_driver_i2c
                        esp_driver_i2s
                        esp_driver_ledc
                        esp_app_format
                        app_update
                        spi_flash
                        mbedtls
                        console
                        efuse
                        pthread # MCP 工作线程直接使用 esp_pthread.h。
                        mini_bot_network
                        xiaozhi-fonts
                    )

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
    target_compile_definitions(${COMPONENT_LIB}
        PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
        PRIVATE BOARD_MANUFACTURER=\"\"
        PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
        PRIVATE TEXT_FONT_SIZE=${TEXT_FONT_SIZE} TEXT_FONT_BPP=${TEXT_FONT_BPP})
    # LVGL 内置内存池放在 PSRAM（板子 8MB PSRAM）。
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    if(lvgl_lib AND CONFIG_SPIRAM)
        target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/display")
        target_compile_definitions(${lvgl_lib} PRIVATE "LV_MEM_POOL_INCLUDE=\"lvgl_psram_pool.h\"")
    endif()
endif()

add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND "${PYTHON}" ${PROJECT_DIR}/scripts/gen_lang.py --language "${LANG_DIR}" --output "${LANG_HEADER}"
    DEPENDS ${LANG_JSON} ${LANG_SOUNDS} ${COMMON_SOUNDS} ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
    VERBATIM
)
add_custom_target(lang_header DEPENDS ${LANG_HEADER})
# 并行构建也必须先生成头文件，不能依赖已存在的生成产物。
add_dependencies(${COMPONENT_LIB} lang_header)

idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(COMPONENT ${build_components})
    if(COMPONENT MATCHES "espressif__esp-sr")
        idf_component_get_property(ESP_SR_PATH ${COMPONENT} COMPONENT_DIR)
    elseif(COMPONENT MATCHES "xiaozhi-fonts")
        idf_component_get_property(NOTO_FONTS_PATH ${COMPONENT} COMPONENT_DIR)
    endif()
endforeach()

# assets 分区：唤醒模型 + 字体，由 build_default_assets.py 生成。
partition_table_get_partition_info(assets_size "--partition-name assets" "size")
if(CONFIG_FLASH_DEFAULT_ASSETS AND assets_size)
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND "${PYTHON}" ${PROJECT_DIR}/scripts/build_default_assets.py
                --sdkconfig "${SDKCONFIG}" --output "${GENERATED_ASSETS_BIN}"
                --builtin_text_font "${BUILTIN_TEXT_FONT}"
                --esp_sr_model_path "${ESP_SR_PATH}/model" --noto_fonts_path "${NOTO_FONTS_PATH}"
                --max_size "${assets_size}"
        DEPENDS "${SDKCONFIG}" ${PROJECT_DIR}/scripts/build_default_assets.py
        COMMENT "Building default assets.bin"
        VERBATIM
    )
    add_custom_target(generated_default_assets ALL DEPENDS ${GENERATED_ASSETS_BIN})
    esptool_py_flash_to_partition(flash "assets" "${GENERATED_ASSETS_BIN}")
elseif(CONFIG_FLASH_CUSTOM_ASSETS AND assets_size)
    set(CUSTOM_ASSETS "${PROJECT_DIR}/${CONFIG_CUSTOM_ASSETS_FILE}")
    if(NOT EXISTS "${CUSTOM_ASSETS}")
        message(FATAL_ERROR "Assets file not found: ${CUSTOM_ASSETS}")
    endif()
    esptool_py_flash_to_partition(flash "assets" "${CUSTOM_ASSETS}")
endif()
