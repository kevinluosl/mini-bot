# Wi-Fi transport and common network interfaces; no cellular modem or AT UART.
# Upstream license retained in LICENSE.
idf_component_register(
    SRCS
        src/network_error.cc
        src/esp/esp_network.cc
        src/esp/esp_ssl.cc
        src/esp/esp_tcp.cc
        src/esp/esp_mqtt.cc
        src/esp/esp_udp.cc
        src/web_socket.cc
        src/http_client.cc
    INCLUDE_DIRS include
    PRIV_REQUIRES esp-tls pthread mqtt
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
