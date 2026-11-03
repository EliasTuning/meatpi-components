#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    uint8_t disco_public_key[32];
    uint8_t disco_private_key[32];
} microlink_t;

typedef struct
{
    uint8_t public_key[32];
    uint8_t disco_key[32];
    char hostname[64];
} ml_peer_t;

typedef struct
{
    bool via_derp;
    uint32_t src_ip;
    uint16_t src_port;
} ml_rx_packet_t;

esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *key,
                            const uint8_t *data, size_t len);
