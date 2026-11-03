#include "ml_disco_pong.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "nacl_box.h"

static const char *TAG = "ml_wg_mgr";
static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };

esp_err_t ml_disco_reply_ping(microlink_t *ml, const ml_peer_t *peer,
                             const ml_rx_packet_t *pkt, const uint8_t txid[12])
{
    if (!pkt->via_derp && (pkt->src_ip == 0 || pkt->src_port == 0))
    {
        ESP_LOGW(TAG, "DISCO UDP ping has no return address for %s", peer->hostname);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t plaintext[32] = { 0x02, 0 };
    memcpy(plaintext + 2, txid, 12);
    plaintext[24] = 0xff;
    plaintext[25] = 0xff;
    plaintext[26] = (pkt->src_ip >> 24) & 0xff;
    plaintext[27] = (pkt->src_ip >> 16) & 0xff;
    plaintext[28] = (pkt->src_ip >> 8) & 0xff;
    plaintext[29] = pkt->src_ip & 0xff;
    plaintext[30] = (pkt->src_port >> 8) & 0xff;
    plaintext[31] = pkt->src_port & 0xff;

    uint8_t pong[6 + 32 + NACL_BOX_NONCEBYTES + NACL_BOX_MACBYTES + sizeof(plaintext)];
    memcpy(pong, DISCO_MAGIC, sizeof(DISCO_MAGIC));
    memcpy(pong + 6, ml->disco_public_key, 32);
    uint8_t *nonce = pong + 38;
    esp_fill_random(nonce, NACL_BOX_NONCEBYTES);
    if (nacl_box(nonce + NACL_BOX_NONCEBYTES, plaintext, sizeof(plaintext), nonce,
                 peer->disco_key, ml->disco_private_key) != 0)
    {
        ESP_LOGW(TAG, "DISCO pong encryption failed for %s", peer->hostname);
        return ESP_FAIL;
    }

    /* A pong proves only its ping's path. A UDP reply to a DERP ping can make
     * Tailscale promote its synthetic relay address to a direct endpoint. */
    if (pkt->via_derp)
    {
        esp_err_t err = ml_derp_queue_send(ml, peer->public_key, pong, sizeof(pong));
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "DISCO relay pong dropped for %s", peer->hostname);
        }
        return err;
    }

    int sent = ml_disco_udp_sendto(ml, pong, sizeof(pong), pkt->src_ip, pkt->src_port);
    if (sent != (int)sizeof(pong))
    {
        ESP_LOGD(TAG, "DISCO UDP pong send failed for %s", peer->hostname);
        return ESP_FAIL;
    }
    return ESP_OK;
}
