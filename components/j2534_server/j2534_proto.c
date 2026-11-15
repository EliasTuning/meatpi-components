/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file j2534_proto.c
 * @brief Pure J2534 wire codec + filter match (see j2534_proto.h).
 *        Little-endian on the wire, independent of host endianness.
 */
#include "j2534_proto.h"

#include <string.h>

/* ---- little-endian helpers -------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- header ----------------------------------------------------------- */

size_t j2534_hdr_encode(uint8_t *buf, uint8_t type, uint16_t seq,
                        uint16_t channel, uint32_t length)
{
    put_u16(buf + 0, J2534_MAGIC);
    buf[2] = J2534_WIRE_VERSION;
    buf[3] = type;
    put_u16(buf + 4, seq);
    put_u16(buf + 6, channel);
    put_u32(buf + 8, length);
    return J2534_HDR_SIZE;
}

bool j2534_hdr_decode(const uint8_t *buf, size_t len, j2534_hdr_t *out)
{
    if (buf == NULL || out == NULL || len < J2534_HDR_SIZE)
    {
        return false;
    }

    if (get_u16(buf + 0) != J2534_MAGIC || buf[2] != J2534_WIRE_VERSION)
    {
        return false;
    }

    out->magic   = J2534_MAGIC;
    out->version = buf[2];
    out->type    = buf[3];
    out->seq     = get_u16(buf + 4);
    out->channel = get_u16(buf + 6);
    out->length  = get_u32(buf + 8);
    return true;
}

/* ---- PASSTHRU_MSG ----------------------------------------------------- */

#define MSG_FIXED 24 /* 6 × u32 before the data bytes */

size_t j2534_msg_encode(const j2534_msg_t *msg, uint8_t *buf, size_t cap)
{
    if (msg == NULL || buf == NULL || msg->data_size > J2534_MAX_DATA)
    {
        return 0;
    }

    size_t total = MSG_FIXED + msg->data_size;

    if (cap < total)
    {
        return 0;
    }

    put_u32(buf + 0,  msg->protocol_id);
    put_u32(buf + 4,  msg->rx_status);
    put_u32(buf + 8,  msg->tx_flags);
    put_u32(buf + 12, msg->timestamp);
    put_u32(buf + 16, msg->extra_data_index);
    put_u32(buf + 20, msg->data_size);
    memcpy(buf + MSG_FIXED, msg->data, msg->data_size);
    return total;
}

bool j2534_msg_decode(const uint8_t *buf, size_t len, j2534_msg_t *out)
{
    if (buf == NULL || out == NULL || len < MSG_FIXED)
    {
        return false;
    }

    uint32_t ds = get_u32(buf + 20);

    if (ds > J2534_MAX_DATA || ds > len - MSG_FIXED)
    {
        return false;
    }

    out->protocol_id      = get_u32(buf + 0);
    out->rx_status        = get_u32(buf + 4);
    out->tx_flags         = get_u32(buf + 8);
    out->timestamp        = get_u32(buf + 12);
    out->extra_data_index = get_u32(buf + 16);
    out->data_size        = ds;
    memcpy(out->data, buf + MSG_FIXED, ds);
    return true;
}

/* ---- filter match ----------------------------------------------------- */

bool j2534_filter_match(const uint8_t *mask, size_t mask_len,
                        const uint8_t *pattern, size_t pattern_len,
                        const uint8_t *msg, size_t msg_len)
{
    if (mask_len == 0)
    {
        return true; /* PASS-all */
    }

    if (mask == NULL || pattern == NULL || msg == NULL)
    {
        return false;
    }

    size_t n = mask_len < msg_len ? mask_len : msg_len;

    for (size_t i = 0; i < n; i++)
    {
        uint8_t pat = (i < pattern_len) ? pattern[i] : 0;

        if (((msg[i] ^ pat) & mask[i]) != 0)
        {
            return false;
        }
    }

    return true;
}

/* ---- names ------------------------------------------------------------ */

const char *j2534_status_name(uint32_t status)
{
    switch (status)
    {
        case J2534_STATUS_NOERROR:           return "NOERROR";
        case J2534_ERR_NOT_SUPPORTED:        return "ERR_NOT_SUPPORTED";
        case J2534_ERR_INVALID_CHANNEL_ID:   return "ERR_INVALID_CHANNEL_ID";
        case J2534_ERR_INVALID_PROTOCOL_ID:  return "ERR_INVALID_PROTOCOL_ID";
        case J2534_ERR_NULL_PARAMETER:       return "ERR_NULL_PARAMETER";
        case J2534_ERR_INVALID_IOCTL_VALUE:  return "ERR_INVALID_IOCTL_VALUE";
        case J2534_ERR_INVALID_FLAGS:        return "ERR_INVALID_FLAGS";
        case J2534_ERR_FAILED:               return "ERR_FAILED";
        case J2534_ERR_DEVICE_NOT_CONNECTED: return "ERR_DEVICE_NOT_CONNECTED";
        case J2534_ERR_TIMEOUT:              return "ERR_TIMEOUT";
        case J2534_ERR_INVALID_MSG:          return "ERR_INVALID_MSG";
        case J2534_ERR_EXCEEDED_LIMIT:       return "ERR_EXCEEDED_LIMIT";
        case J2534_ERR_BUFFER_EMPTY:         return "ERR_BUFFER_EMPTY";
        case J2534_ERR_BUFFER_FULL:          return "ERR_BUFFER_FULL";
        case J2534_ERR_BUFFER_OVERFLOW:      return "ERR_BUFFER_OVERFLOW";
        case J2534_ERR_INVALID_FILTER_ID:    return "ERR_INVALID_FILTER_ID";
        case J2534_ERR_NO_FLOW_CONTROL:      return "ERR_NO_FLOW_CONTROL";
        case J2534_ERR_NOT_UNIQUE:           return "ERR_NOT_UNIQUE";
        case J2534_ERR_INVALID_BAUDRATE:     return "ERR_INVALID_BAUDRATE";
        case J2534_ERR_INVALID_DEVICE_ID:    return "ERR_INVALID_DEVICE_ID";
        case J2534_ERR_DEVICE_IN_USE:        return "ERR_DEVICE_IN_USE";
        case J2534_ERR_NO_CONNECTION_ESTABLISHED:
            return "ERR_NO_CONNECTION_ESTABLISHED";
        default:                             return "?";
    }
}

/* ---- transport-agnostic framer ---------------------------------------- */

/* read exactly n bytes into buf (or discard them when buf == NULL) */
static j2534_frame_rc_t read_exact(j2534_read_fn rd, void *ctx,
                                   uint32_t per_read_ms,
                                   uint32_t max_idle_reads,
                                   volatile const bool *run, uint8_t *buf,
                                   size_t n)
{
    uint8_t scratch[32];
    size_t got = 0;
    uint32_t idle = 0;

    while (got < n)
    {
        if (run != NULL && !*run)
        {
            return J2534_FR_STOPPED;
        }

        uint8_t *dst = (buf != NULL) ? buf + got : scratch;
        size_t want = n - got;

        if (buf == NULL && want > sizeof(scratch))
        {
            want = sizeof(scratch);
        }

        int r = rd(ctx, dst, want, per_read_ms);

        if (r < 0)
        {
            return J2534_FR_LINK_DOWN;
        }

        if (r == 0)
        {
            if (max_idle_reads != 0 && ++idle >= max_idle_reads)
            {
                return J2534_FR_TIMEOUT;
            }

            continue;
        }

        idle = 0;
        got += (size_t)r;
    }

    return J2534_FR_OK;
}

j2534_frame_rc_t j2534_frame_read(j2534_read_fn rd, void *ctx,
                                  uint32_t per_read_ms,
                                  uint32_t max_idle_reads,
                                  volatile const bool *run, j2534_hdr_t *h,
                                  uint8_t *payload, size_t payload_cap)
{
    uint8_t hdr[J2534_HDR_SIZE];
    j2534_frame_rc_t rc = read_exact(rd, ctx, per_read_ms, max_idle_reads,
                                     run, hdr, sizeof(hdr));

    if (rc != J2534_FR_OK)
    {
        return rc;
    }

    if (!j2534_hdr_decode(hdr, sizeof(hdr), h))
    {
        return J2534_FR_BAD_HDR;
    }

    if (h->length > payload_cap)
    {
        return J2534_FR_TOO_BIG;
    }

    if (h->length == 0)
    {
        return J2534_FR_OK;
    }

    return read_exact(rd, ctx, per_read_ms, max_idle_reads, run, payload,
                      h->length);
}

size_t j2534_ack_encode(uint8_t *buf, size_t cap, uint16_t seq,
                        uint16_t channel, uint32_t status,
                        const uint32_t *result)
{
    uint32_t len = (result != NULL) ? 8 : 4;

    if (buf == NULL || cap < J2534_HDR_SIZE + len)
    {
        return 0;
    }

    j2534_hdr_encode(buf, J2534_MT_ACK, seq, channel, len);
    put_u32(buf + J2534_HDR_SIZE, status);

    if (result != NULL)
    {
        put_u32(buf + J2534_HDR_SIZE + 4, *result);
    }

    return J2534_HDR_SIZE + len;
}

bool j2534_tcp_gate_allowed(bool allow_lan, bool loopback, bool on_ap,
                            bool on_usb)
{
    return allow_lan || loopback || on_ap || on_usb;
}

uint32_t j2534_connect_baud_check(uint32_t asked_bps, uint32_t bus_kbps)
{
    if (bus_kbps == 0)
    {
        return J2534_STATUS_NOERROR;
    }

    /* whole kbit/s, as the settings count: 83333 is "83" */
    return (asked_bps / 1000u == bus_kbps) ? J2534_STATUS_NOERROR
                                           : J2534_ERR_INVALID_BAUDRATE;
}

uint8_t j2534_tp20_encode_ms(uint32_t ms)
{
    if (ms == 0)
    {
        return 0;
    }

    /* 0.1 ms units: 1..6 ms fits in the 6-bit scale (40 ms does not). */
    if (ms <= 6)
    {
        return (uint8_t)(ms * 10u);
    }

    if (ms <= 63)
    {
        return (uint8_t)(0x40u | ms);
    }

    if (ms <= 630)
    {
        uint32_t scale = (ms + 9u) / 10u;

        if (scale > 63u)
        {
            scale = 63u;
        }

        return (uint8_t)(0x80u | scale);
    }

    {
        uint32_t scale = (ms + 99u) / 100u;

        if (scale == 0)
        {
            scale = 1u;
        }

        if (scale > 63u)
        {
            scale = 63u;
        }

        return (uint8_t)(0xC0u | scale);
    }
}

uint32_t j2534_tp20_decode_us(uint8_t wire)
{
    static const uint32_t unit_us[4] = { 100u, 1000u, 10000u, 100000u };

    return unit_us[wire >> 6] * (uint32_t)(wire & 0x3Fu);
}

uint32_t j2534_tp20_round_gap_us(uint32_t us)
{
    if (us == 0)
    {
        return 0;
    }

    return ((us + 499u) / 500u) * 500u;
}

/* ---- ISO15765 message data (SAE J2534-1 layout) ------------------------ */

static void put_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

bool j2534_iso_split(const j2534_msg_t *msg, uint32_t *id,
                     const uint8_t **payload, size_t *len)
{
    if (msg == NULL || id == NULL || payload == NULL || len == NULL)
    {
        return false;
    }

    if (msg->data_size < 4 || msg->data_size > J2534_MAX_DATA ||
        (msg->tx_flags & J2534_TX_ISO15765_ADDR_TYPE) != 0)
    {
        return false;
    }

    *id = get_u32_be(msg->data);
    *payload = msg->data + 4;
    *len = msg->data_size - 4;
    return true;
}

bool j2534_iso_join(j2534_msg_t *out, uint32_t id, bool ext,
                    const uint8_t *payload, size_t len, uint32_t ts_us)
{
    if (out == NULL || (payload == NULL && len > 0) ||
        len > (size_t)J2534_MAX_DATA - 4)
    {
        return false;
    }

    out->protocol_id = J2534_PROT_ISO15765;
    out->rx_status = ext ? J2534_RX_CAN_29BIT_ID : 0;
    out->tx_flags = 0;
    out->timestamp = ts_us;
    out->extra_data_index = (uint32_t)(4 + len);
    out->data_size = (uint32_t)(4 + len);
    put_u32_be(out->data, id);

    if (len > 0)
    {
        memcpy(out->data + 4, payload, len);
    }

    return true;
}

void j2534_iso_indication(j2534_msg_t *out, uint32_t id, bool ext,
                          uint32_t rx_status, uint32_t ts_us)
{
    if (out == NULL)
    {
        return;
    }

    out->protocol_id = J2534_PROT_ISO15765;
    out->rx_status = rx_status | (ext ? J2534_RX_CAN_29BIT_ID : 0);
    out->tx_flags = 0;
    out->timestamp = ts_us;
    out->extra_data_index = 4;
    out->data_size = 4;
    put_u32_be(out->data, id);
}

uint8_t j2534_iso_single_frame(const uint8_t *payload, size_t len, bool pad,
                               uint8_t frame[8])
{
    if (payload == NULL || frame == NULL || len == 0 || len > 7)
    {
        return 0;
    }

    frame[0] = (uint8_t)len;
    memcpy(frame + 1, payload, len);

    uint8_t dlc = (uint8_t)(1 + len);

    if (pad && dlc < 8)
    {
        memset(frame + dlc, 0xAA, (size_t)(8 - dlc));
        dlc = 8;
    }

    return dlc;
}

bool j2534_iso_parse_single_frame(const uint8_t *frame, uint8_t dlc,
                                  size_t *len)
{
    if (frame == NULL || len == NULL || dlc < 2 || dlc > 8)
    {
        return false;
    }

    if ((frame[0] >> 4) != 0)
    {
        return false;
    }

    uint8_t n = (uint8_t)(frame[0] & 0x0F);

    if (n == 0 || n > 7 || n > (uint8_t)(dlc - 1))
    {
        return false;
    }

    *len = n;
    return true;
}

bool j2534_iso_is_first_frame(const uint8_t *frame, uint8_t dlc)
{
    return frame != NULL && dlc >= 2 && (frame[0] >> 4) == 1;
}

/* ---- J1850 (legacy, OBD-chip-backed) message helpers ------------------- */

static char hex_digit(uint8_t nibble)
{
    return "0123456789ABCDEF"[nibble & 0x0F];
}

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

static uint8_t hex_value(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return (uint8_t)(c - 'A' + 10);
}

bool j2534_legacy_split(const j2534_msg_t *msg, const uint8_t **hdr,
                        const uint8_t **payload, size_t *len)
{
    if (msg == NULL || hdr == NULL || payload == NULL || len == NULL)
    {
        return false;
    }

    if (msg->data_size < (uint32_t)(J2534_J1850_HDR + 1) ||
        msg->data_size > J2534_MAX_DATA)
    {
        return false;
    }

    *hdr = msg->data;
    *payload = msg->data + J2534_J1850_HDR;
    *len = msg->data_size - J2534_J1850_HDR;
    return true;
}

size_t j2534_legacy_header_cmd(const uint8_t hdr[3], char *out, size_t cap)
{
    /* "ATSH" + 6 hex + NUL = 11 bytes */
    if (hdr == NULL || out == NULL || cap < 11)
    {
        return 0;
    }

    out[0] = 'A'; out[1] = 'T'; out[2] = 'S'; out[3] = 'H';

    for (int i = 0; i < J2534_J1850_HDR; i++)
    {
        out[4 + i * 2] = hex_digit((uint8_t)(hdr[i] >> 4));
        out[5 + i * 2] = hex_digit(hdr[i]);
    }

    out[10] = '\0';
    return 10;
}

uint16_t j2534_legacy_checksum(const uint8_t *payload, size_t len)
{
    uint16_t sum = 0;

    for (size_t i = 0; payload != NULL && i < len; i++)
    {
        sum = (uint16_t)(sum + payload[i]);
    }

    return sum;
}

/* append the hex of @p payload at out[o]; returns the new offset */
static size_t append_hex(char *out, size_t o, const uint8_t *payload,
                         size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        out[o++] = hex_digit((uint8_t)(payload[i] >> 4));
        out[o++] = hex_digit(payload[i]);
    }

    return o;
}

size_t j2534_legacy_build_request(const uint8_t *payload, size_t len,
                                  size_t sf_max, char *out, size_t cap)
{
    if (payload == NULL || out == NULL || len == 0 ||
        len > J2534_J1850_VT_MAX)
    {
        return 0;
    }

    /* short payload: a bare hex line, the chip sends it as one J1850 frame */
    if (len <= sf_max)
    {
        if (cap < len * 2 + 1)
        {
            return 0;
        }

        size_t o = append_hex(out, 0, payload, len);

        out[o] = '\0';
        return o;
    }

    /* longer: VTFullyRequestCk<LLLL><hex><CCCC>, the chip does the USDT */
    static const char PFX[] = "VTFullyRequestCk";
    size_t pfx = sizeof(PFX) - 1;

    if (cap < pfx + 4 + len * 2 + 4 + 1)
    {
        return 0;
    }

    uint16_t cks = j2534_legacy_checksum(payload, len);
    size_t o = 0;

    memcpy(out, PFX, pfx);
    o += pfx;
    out[o++] = hex_digit((uint8_t)(len >> 12));
    out[o++] = hex_digit((uint8_t)(len >> 8));
    out[o++] = hex_digit((uint8_t)(len >> 4));
    out[o++] = hex_digit((uint8_t)len);
    o = append_hex(out, o, payload, len);
    out[o++] = hex_digit((uint8_t)(cks >> 12));
    out[o++] = hex_digit((uint8_t)(cks >> 8));
    out[o++] = hex_digit((uint8_t)(cks >> 4));
    out[o++] = hex_digit((uint8_t)cks);
    out[o] = '\0';
    return o;
}

size_t j2534_legacy_decode_line(const char *line, size_t line_len,
                                uint8_t *out, size_t cap)
{
    size_t n = 0;
    size_t i = 0;

    if (line == NULL || out == NULL)
    {
        return 0;
    }

    while (i < line_len && n < cap)
    {
        if (!is_hex_digit(line[i]))
        {
            i++;
            continue;
        }

        if (i + 1 >= line_len || !is_hex_digit(line[i + 1]))
        {
            break; /* a lone nibble: stop (not a byte) */
        }

        out[n++] = (uint8_t)((hex_value(line[i]) << 4) | hex_value(line[i + 1]));
        i += 2;
    }

    return n;
}

bool j2534_legacy_line_is_status(const char *line, size_t line_len)
{
    if (line == NULL)
    {
        return true;
    }

    for (size_t i = 0; i < line_len; i++)
    {
        char c = line[i];

        if (c == '?')
        {
            return true;
        }

        /* a letter that is not a hex digit (G-Z / g-z) marks a word like
           NO DATA, ERROR, BUS INIT, SEARCHING, STOPPED, UNABLE, OK */
        char u = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;

        if (u >= 'G' && u <= 'Z')
        {
            return true;
        }
    }

    return false;
}

bool j2534_legacy_is_pending(const uint8_t *msg, size_t len)
{
    return msg != NULL && len >= (size_t)(J2534_J1850_HDR + 3) &&
           msg[J2534_J1850_HDR] == 0x7F && msg[J2534_J1850_HDR + 2] == 0x78;
}
