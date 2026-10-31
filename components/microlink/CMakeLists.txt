set(SRCS
    "src/microlink.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/x25519.c"
    "src/ml_zerocopy.c"
    "src/ml_cellular.c"
    "src/ml_at_socket.c"
    "src/ml_net_switch.c"
    "src/ml_x509_wrap.c"
)

set(REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    lwip
    mbedtls
    espressif__cjson
    esp_driver_uart
    esp_driver_gpio
    esp_http_server
)

# WiCAN/IDF v6: mbedtls 4 (TF-PSA-Crypto) moved entropy/ctr_drbg/
# chachapoly to mbedtls/private/. Add the builtin private include dir so
# microlink's classic-mbedtls TLS/crypto (byte-unchanged) resolves them.
idf_build_get_property(idf_path IDF_PATH)
set(_ml_mbedtls_priv
    "${idf_path}/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/include")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
        "${_ml_mbedtls_priv}"
    REQUIRES ${REQUIRES}
    # WiCAN (2026-11-01): ip4_routes carries the subnet routes a peer
    # advertises into lwIP's route lookup (ml_wg_mgr.c routes_apply)
    PRIV_REQUIRES
        wireguard_lwip
        ip4_routes
)

# WiCAN (2026-10-31, issue 1024): the TLS chain parser's tolerance for a
# certificate it cannot parse (src/ml_x509_wrap.c). Every public Tailscale
# DERP relay ends its chain with an Ed25519-signed meta certificate;
# mbedTLS 4 parses it to the bare UNKNOWN_OID, which its chain parsers
# tolerate, but the TLS 1.3 parser returns that tolerated code from its
# exit path when it belongs to the last certificate, so every relay
# handshake failed. The wrap answers 0 for a skipped certificate behind a
# parsed one; the -u beside each wrap is the ip4_routes idiom (the linker
# takes the wrapper out of this static library at its first scan).
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=mbedtls_x509_crt_parse_der"
                      "-u __wrap_mbedtls_x509_crt_parse_der"
                      "-Wl,--wrap=mbedtls_x509_crt_parse_der_nocopy"
                      "-u __wrap_mbedtls_x509_crt_parse_der_nocopy")
