file(GLOB UPNP_SRCS "src/*.cpp")

include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
# Conditional sources: esp-idf/conditional/<name>/ is compiled only when <name>
# is staged. The on-device UPnP settings pane lives in conditional/spangap-lcd/
# and registers via the when:-gated upnpLcdRegister hook — no #if anywhere.
idf_component_register(
    SRCS ${UPNP_SRCS} ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        esp_event
        esp_netif
        lwip
        esp_http_client
)
