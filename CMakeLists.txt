# Select transport backend based on Kconfig
if(CONFIG_TBFT_TRANSPORT_ESPNOW)
    set(TBFT_TRANSPORT_SRC "src/tbft_transport_espnow.c")
else()
    set(TBFT_TRANSPORT_SRC "src/tbft_transport_udp.c")
endif()

idf_component_register(
    SRCS
        "src/tbft_message.c"
        "src/tbft_principal.c"
        "src/tbft_itimer.c"
        "src/tbft_node.c"
        "${TBFT_TRANSPORT_SRC}"
        "src/tbft_certificate.c"
        "src/tbft_prepared_cert.c"
        "src/tbft_partition.c"
        "src/tbft_state.c"
        "src/tbft_view_info.c"
        "src/tbft_agreement_region.c"
        "src/tbft_checkpoint_region.c"
        "src/tbft_special_region.c"
        "src/tbft_replica.c"
        "src/tbft_libbyz.c"
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        mbedtls
        esp_timer
        lwip
        freertos
        vfs
        spiffs
        esp_wifi
        efuse
        nvs_flash
)

# C23: the sources use nullptr, constexpr, static_assert, [[attributes]] and
# <stdckdint.h>.  ESP-IDF >= 6.0 already compiles C as -std=gnu23
# (docs/en/api-guides/c.rst, "C Version"); pinning it the way that page
# describes keeps this component on C23 even if a project changes the default.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu23)
