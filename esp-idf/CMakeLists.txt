file(GLOB UPNP_SRCS "src/*.cpp")

idf_component_register(
    SRCS ${UPNP_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-net
        spangap-lcd
        esp_event
        esp_netif
        lwip
        esp_http_client
)
