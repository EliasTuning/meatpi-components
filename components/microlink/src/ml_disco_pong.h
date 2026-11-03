#pragma once

#include "microlink_internal.h"

/* Private to microlink; addresses and ports are in host byte order. */
int ml_disco_udp_sendto(microlink_t *ml, const uint8_t *data, size_t len,
                       uint32_t dest_ip, uint16_t dest_port);

esp_err_t ml_disco_reply_ping(microlink_t *ml, const ml_peer_t *peer,
                             const ml_rx_packet_t *pkt, const uint8_t txid[12]);
