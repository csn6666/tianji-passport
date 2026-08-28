idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "chat_provision.c"
         "app_net.c"
         "demo_fortune.c"
         "llm_client.c"
         "bazi_engine.c"
         "bazi_tables.c"
         "font_cn_16.c"
         "font_mystic_28.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_wifi esp_event esp_netif nvs_flash json esp_ringbuf esp_http_server esp_http_client esp-tls mbedtls
)

# 没配 chat_config.h 就别让它编到一半再报一堆看不懂的错
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/chat_config.h")
    message(FATAL_ERROR
        "缺少 main/chat_config.h。先复制模板再填自己的服务器地址:\n"
        "    cp main/chat_config.h.example main/chat_config.h")
endif()
