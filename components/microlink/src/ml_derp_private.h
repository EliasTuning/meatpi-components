/**
 * @file ml_derp_private.h
 * @brief WiCAN (2026-11-03): the DERP connection primitives shared between
 *        ml_derp.c (the I/O task, the home relay) and ml_derp_pool.c (the
 *        connections to the peers' own regions, the reverse routes).
 *
 * Every function here runs on the DERP I/O task, the sole owner of every
 * relay socket and TLS context, except ml_derp_pool_reset (init, before
 * the task exists) and ml_derp_pool_region_of (a read of the peer table
 * from whoever queues a packet).
 */
#ifndef ML_DERP_PRIVATE_H
#define ML_DERP_PRIVATE_H

#include "microlink_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- one connection to one relay region (ml_derp.c) ----------------------- */

/* dial, TLS (port 443) or plain TCP, the HTTP upgrade and the DERP
 * handshake (ServerKey, ClientInfo, ServerInfo); NotePreferred only for the
 * home connection. On failure the socket and the TLS context are freed. */
esp_err_t ml_derp_conn_open(microlink_t *ml, ml_derp_conn_t *c, uint16_t region,
                            bool preferred);
void ml_derp_conn_close(ml_derp_conn_t *c);

/* a SendPacket frame on that connection; < 0 = the connection is dead */
int ml_derp_conn_send(ml_derp_conn_t *c, const uint8_t *dest_key,
                      const uint8_t *data, size_t len);

/* one frame read and dispatched: 1, 0 = nothing waiting, < 0 = dead */
int ml_derp_conn_poll(microlink_t *ml, ml_derp_conn_t *c);

/* ---- the pool (ml_derp_pool.c) --------------------------------------------- */

void ml_derp_pool_reset(microlink_t *ml);

/* the peer's home region from the peer table, 0 when unknown */
uint16_t ml_derp_pool_region_of(microlink_t *ml, const uint8_t *key);

/* a RecvPacket from that key arrived on the connection to that region */
void ml_derp_pool_note_heard(microlink_t *ml, const uint8_t *key, uint16_t region);

/* that region answered PeerGone for the key: the route through it is gone */
void ml_derp_pool_forget_heard(microlink_t *ml, const uint8_t *key, uint16_t region);

/* the connection to send the item on: the peer's own region, a region the
 * peer was heard from, else the peer's region dialled now, else the home
 * relay. Never NULL while the home relay is up. */
ml_derp_conn_t *ml_derp_pool_pick(microlink_t *ml, const ml_derp_tx_item_t *item);

/* read the extra connections, close the idle ones */
void ml_derp_pool_service(microlink_t *ml, uint32_t *frames_rx);

void ml_derp_pool_close_all(microlink_t *ml);

#ifdef __cplusplus
}
#endif

#endif /* ML_DERP_PRIVATE_H */
