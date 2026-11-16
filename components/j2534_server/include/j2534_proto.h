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
 * @file j2534_proto.h
 * @brief SAE J2534 constants + the WiCAN PassThru wire-protocol codec.
 *        PURE (no IDF types): host-tested. This is the contract the
 *        companion Windows DLL and the device server both implement.
 *        See TASK_j2534_server.md §4.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- J2534 protocol IDs (SAE J2534-1) --------------------------------- *
 * WiCAN serves CAN + ISO15765 on the native controller, and J1850 VPW on
 * the OBD chip (ELM/STN dialect). PWM / ISO9141 / ISO14230 are chip-wired
 * on the hardware too but not built yet (they return ERR_NOT_SUPPORTED). */
enum {
    J2534_PROT_J1850VPW  = 1,   /* supported (OBD chip, GM Class 2) */
    J2534_PROT_J1850PWM  = 2,
    J2534_PROT_ISO9141   = 3,
    J2534_PROT_ISO14230  = 4,
    J2534_PROT_CAN       = 5,   /* supported */
    J2534_PROT_ISO15765  = 6,   /* supported (ISO-TP) */
    J2534_PROT_SCI_A_ENGINE = 7,
    /* SAE J2534-2 TP2.0. Its own channel: raw CAN (protocol 5) does not
     * parse TP2.0 opcodes. */
    J2534_PROT_TP2_0     = 0x800E,
};

/* ---- J2534 return/status codes ---------------------------------------- */
enum {
    J2534_STATUS_NOERROR              = 0x00,
    J2534_ERR_NOT_SUPPORTED           = 0x01,
    J2534_ERR_INVALID_CHANNEL_ID      = 0x02,
    J2534_ERR_INVALID_PROTOCOL_ID     = 0x03,
    J2534_ERR_NULL_PARAMETER          = 0x04,
    J2534_ERR_INVALID_IOCTL_VALUE     = 0x05,
    J2534_ERR_INVALID_FLAGS           = 0x06,
    J2534_ERR_FAILED                  = 0x07,
    J2534_ERR_DEVICE_NOT_CONNECTED    = 0x08,
    J2534_ERR_TIMEOUT                 = 0x09,
    J2534_ERR_INVALID_MSG             = 0x0A,
    J2534_ERR_EXCEEDED_LIMIT          = 0x0C, /* filter / session table full */
    J2534_ERR_BUFFER_EMPTY            = 0x10,
    J2534_ERR_BUFFER_FULL             = 0x11,
    J2534_ERR_BUFFER_OVERFLOW         = 0x12,
    J2534_ERR_MSG_PROTOCOL_ID         = 0x14,
    J2534_ERR_INVALID_FILTER_ID       = 0x15,
    J2534_ERR_NO_FLOW_CONTROL         = 0x16,
    J2534_ERR_NOT_UNIQUE              = 0x17,
    J2534_ERR_INVALID_BAUDRATE        = 0x18,
    J2534_ERR_INVALID_DEVICE_ID       = 0x19,
    J2534_ERR_DEVICE_IN_USE           = 0x1A, /* WiCAN: second tester */
    /* SAE J2534-2 table 123: its own return values start at 0x10000. */
    J2534_ERR_NO_CONNECTION_ESTABLISHED = 0x00010001,
};

/* ---- filter types ----------------------------------------------------- */
enum {
    J2534_PASS_FILTER          = 1,
    J2534_BLOCK_FILTER         = 2,
    J2534_FLOW_CONTROL_FILTER  = 3, /* ISO15765 reassembly */
};

/* ---- TxFlags / RxStatus bits ------------------------------------------ */
enum {
    J2534_TX_ISO15765_FRAME_PAD = 0x00000040,
    J2534_TX_ISO15765_ADDR_TYPE = 0x00000080, /* extended addressing: not supported */
    J2534_TX_CAN_29BIT_ID       = 0x00000100,
    J2534_TX_WAIT_P3_MIN_ONLY   = 0x00000200,
    J2534_TX_TP2_0_BROADCAST    = 0x00010000, /* TP2_0_BROADCAST_MSG */
};
enum {
    J2534_RX_TX_MSG_TYPE           = 0x00000001, /* a transmit, not a receive */
    J2534_RX_START_OF_MESSAGE      = 0x00000002, /* ISO15765 FF indication */
    J2534_RX_TX_INDICATION         = 0x00000008, /* TxDone (with TX_MSG_TYPE) */
    J2534_RX_ISO15765_PADDING_ERR  = 0x00000010,
    J2534_RX_ISO15765_ADDR_TYPE    = 0x00000080,
    J2534_RX_CAN_29BIT_ID          = 0x00000100,
    J2534_RX_CONNECTION_ESTABLISHED = 0x00010000, /* bit 16 */
    J2534_RX_CONNECTION_LOST        = 0x00020000, /* bit 17 */
};

/* ---- Ioctl IDs + SET_CONFIG parameter IDs ----------------------------- */
enum {
    J2534_IOCTL_GET_CONFIG               = 0x01,
    J2534_IOCTL_SET_CONFIG               = 0x02,
    J2534_IOCTL_READ_VBATT               = 0x03,
    J2534_IOCTL_CLEAR_TX_BUFFER          = 0x07,
    J2534_IOCTL_CLEAR_RX_BUFFER          = 0x08,
    J2534_IOCTL_CLEAR_PERIODIC_MSGS      = 0x09,
    J2534_IOCTL_CLEAR_MSG_FILTERS        = 0x0A,
    J2534_IOCTL_REQUEST_CONNECTION       = 0x800A,
    J2534_IOCTL_TEARDOWN_CONNECTION      = 0x800B,
    J2534_IOCTL_GET_DEVICE_INFO          = 0x800C,
};
enum {
    J2534_CFG_DATA_RATE      = 0x01,
    J2534_CFG_LOOPBACK       = 0x03,
    J2534_CFG_BIT_SAMPLE_POINT = 0x17,
    J2534_CFG_SYNC_JUMP_WIDTH  = 0x18,
    J2534_CFG_ISO15765_BS    = 0x1E,
    J2534_CFG_ISO15765_STMIN = 0x1F,
    J2534_CFG_BS_TX          = 0x22,
    J2534_CFG_STMIN_TX       = 0x23,
    J2534_CFG_J1962_PINS     = 0x8001, /* this hardware: 0x060E only */
    /* SAE J2534-2 table 77, in order. */
    J2534_CFG_TP2_0_T_BR_INT = 0x8044,
    J2534_CFG_TP2_0_T_E      = 0x8045,
    J2534_CFG_TP2_0_MNTC     = 0x8046,
    J2534_CFG_TP2_0_T_CTA    = 0x8047,
    J2534_CFG_TP2_0_MNCT     = 0x8048,
    J2534_CFG_TP2_0_MNTB     = 0x8049,
    J2534_CFG_TP2_0_MNT      = 0x804A,
    J2534_CFG_TP2_0_T_WAIT   = 0x804B,
    J2534_CFG_TP2_0_T1       = 0x804C,
    J2534_CFG_TP2_0_T3       = 0x804D,
    J2534_CFG_TP2_0_IDENTIFIER = 0x804E,
    J2534_CFG_TP2_0_RXIDPASSIVE = 0x804F,
};
/* GET_DEVICE_INFO parameters (SAE J2534-2 table 121), the ones answered. */
enum {
    J2534_DEV_J1850VPW_SUPPORTED    = 0x03,
    J2534_DEV_CAN_SUPPORTED         = 0x06,
    J2534_DEV_ISO15765_SUPPORTED    = 0x07,
    J2534_DEV_TP2_0_SUPPORTED       = 0x13,
    J2534_DEV_TP2_0_PS_J1962        = 0x28,
    J2534_DEV_J1850VPW_SIMULTANEOUS = 0x36,
    J2534_DEV_CAN_SIMULTANEOUS      = 0x39,
    J2534_DEV_ISO15765_SIMULTANEOUS = 0x3A,
    J2534_DEV_TP2_0_SIMULTANEOUS    = 0x46,
};

/** Most parameters one GET_DEVICE_INFO request carries (the ACK's size). */
#define J2534_DEV_INFO_MAX 16

/* ---- wire framing ----------------------------------------------------- */
#define J2534_MAGIC        0x4A35u   /* "J5" */
#define J2534_WIRE_VERSION 1
#define J2534_HDR_SIZE     12
#define J2534_MAX_DATA     4128       /* SAE PASSTHRU_MSG data cap */

/* message types (TASK §4) */
enum {
    J2534_MT_HELLO          = 0x01,
    J2534_MT_OPEN           = 0x02,
    J2534_MT_CLOSE          = 0x03,
    J2534_MT_CONNECT        = 0x04,
    J2534_MT_DISCONNECT     = 0x05,
    J2534_MT_WRITE_MSGS     = 0x10,
    J2534_MT_START_FILTER   = 0x11,
    J2534_MT_STOP_FILTER    = 0x12,
    J2534_MT_START_PERIODIC = 0x13,
    J2534_MT_STOP_PERIODIC  = 0x14,
    J2534_MT_IOCTL          = 0x15,
    J2534_MT_ACK            = 0x80,
    J2534_MT_RX_MSG         = 0x81,
    J2534_MT_EVENT          = 0x82,
};

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
    uint16_t channel;
    uint32_t length;   /* payload bytes after the header */
} j2534_hdr_t;

/** A PassThru message, device-side representation. */
typedef struct {
    uint32_t protocol_id;
    uint32_t rx_status;
    uint32_t tx_flags;
    uint32_t timestamp;      /* µs */
    uint32_t extra_data_index;
    uint32_t data_size;
    uint8_t  data[J2534_MAX_DATA];
} j2534_msg_t;

/**
 * Encode a header into @p buf (must be >= J2534_HDR_SIZE). Always writes
 * magic/version. Returns J2534_HDR_SIZE.
 */
size_t j2534_hdr_encode(uint8_t *buf, uint8_t type, uint16_t seq,
                        uint16_t channel, uint32_t length);

/**
 * Decode a header from @p buf of @p len bytes. Returns true and fills
 * @p out only when len >= J2534_HDR_SIZE, magic matches, and version is
 * accepted. Does NOT require the payload to be present.
 */
bool j2534_hdr_decode(const uint8_t *buf, size_t len, j2534_hdr_t *out);

/**
 * Serialize @p msg to @p buf (cap @p cap). Layout: protocol_id,
 * rx_status, tx_flags, timestamp, extra_data_index, data_size, then
 * data_size bytes. Returns the byte count, or 0 if it doesn't fit / is
 * malformed (data_size > J2534_MAX_DATA).
 */
size_t j2534_msg_encode(const j2534_msg_t *msg, uint8_t *buf, size_t cap);

/**
 * Parse a PASSTHRU_MSG from @p buf/@p len. Returns true on a complete,
 * in-bounds message (data_size <= min(len-24, J2534_MAX_DATA)).
 */
bool j2534_msg_decode(const uint8_t *buf, size_t len, j2534_msg_t *out);

/**
 * J2534 message-filter match: a candidate matches when, over the first
 * @p n bytes (n = min(mask_len, msg_len)), every masked bit is equal:
 *   ((msg[i] ^ pattern[i]) & mask[i]) == 0 for all i in [0, n).
 * A zero-length mask matches everything (PASS-all). Used for PASS/BLOCK
 * filters; FLOW_CONTROL uses the pattern as the ISO-TP address.
 */
bool j2534_filter_match(const uint8_t *mask, size_t mask_len,
                        const uint8_t *pattern, size_t pattern_len,
                        const uint8_t *msg, size_t msg_len);

/** Human name for a status code (static string; "?" if unknown). */
const char *j2534_status_name(uint32_t status);

/* ---- ISO15765 message data, the SAE J2534-1 layout (2026-10-19) --------
 * data = [4-byte big-endian CAN id][UDS payload], the same way round in
 * both directions; extended addressing (ISO15765_ADDR_TYPE, a fifth
 * address byte) is not supported. Before this date the device took the
 * bare payload, which no standard tool sends (udsoncan, the OEM suites). */

/**
 * Split a tester's ISO15765 message into the id (not masked to 11 or 29
 * bits) and the payload after it. False on a short message (< 4 bytes)
 * or when ISO15765_ADDR_TYPE is set. A 4-byte message yields len 0.
 */
bool j2534_iso_split(const j2534_msg_t *msg, uint32_t *id,
                     const uint8_t **payload, size_t *len);

/**
 * Build a device-to-tester ISO15765 message: data = [id][payload],
 * rx_status = CAN_29BIT_ID when @p ext, timestamp @p ts_us. False when
 * the payload does not fit (len > J2534_MAX_DATA - 4), @p out untouched.
 */
bool j2534_iso_join(j2534_msg_t *out, uint32_t id, bool ext,
                    const uint8_t *payload, size_t len, uint32_t ts_us);

/**
 * An ISO15765 indication: TxDone (TX_MSG_TYPE | TX_INDICATION after a
 * transmit) or START_OF_MESSAGE (a first frame arrived). data = the id
 * only (4 bytes); CAN_29BIT_ID is added to @p rx_status when @p ext.
 */
void j2534_iso_indication(j2534_msg_t *out, uint32_t id, bool ext,
                          uint32_t rx_status, uint32_t ts_us);

/**
 * A single frame for a request without a flow-control session
 * (functional addressing): [0n][payload], padded to 8 bytes with 0xAA
 * when @p pad. Returns the DLC, 0 when len is 0 or above 7.
 */
uint8_t j2534_iso_single_frame(const uint8_t *payload, size_t len, bool pad,
                               uint8_t frame[8]);

/**
 * A received single frame: PCI nibble 0, length 1..7 within the DLC.
 * Sets @p len (the payload starts at frame + 1). False for anything else.
 */
bool j2534_iso_parse_single_frame(const uint8_t *frame, uint8_t dlc,
                                  size_t *len);

/** True for an ISO-TP first frame (PCI nibble 1, DLC >= 2). */
bool j2534_iso_is_first_frame(const uint8_t *frame, uint8_t dlc);

/* ---- J1850 (legacy, OBD-chip-backed) message helpers (2026-10-19) -------
 * A J1850 message is [3-byte header][payload], carried whole in a
 * PASSTHRU_MSG's data both ways, with NO 4-byte CAN-id prefix (the way the
 * nightjoker7 fork's DLL and every J2534 app treat J1850). These pure
 * helpers build the OBD chip's ELM/STN commands and classify its reply
 * lines; the chip itself does the USDT (ISO 15765-2 style) segmentation on
 * VPW (VTFullyRequestCk), so no software SF/FF/CF is needed. */

/** The J1850 header is three bytes: priority, target, source. */
#define J2534_J1850_HDR     3
/** Largest payload sent as one bare hex line; longer goes through
 *  VTFullyRequestCk so the chip segments it. */
#define J2534_J1850_SF_MAX  7
/** VTFullyRequestCk's 4-hex length field caps the payload at 0x0FFF. */
#define J2534_J1850_VT_MAX  0x0FFF

/**
 * Split a J1850 message into its 3-byte header and the payload after it.
 * False when @p msg is malformed or data_size < 4 (a header plus at least
 * one payload byte). @p hdr/@p payload point into msg->data.
 */
bool j2534_legacy_split(const j2534_msg_t *msg, const uint8_t **hdr,
                        const uint8_t **payload, size_t *len);

/**
 * Write "ATSH%02X%02X%02X" for a 3-byte header into @p out (NUL-terminated,
 * no CR). Returns the string length (10), or 0 when @p cap is too small.
 */
size_t j2534_legacy_header_cmd(const uint8_t hdr[3], char *out, size_t cap);

/** The 16-bit additive checksum (sum of the bytes) VTFullyRequestCk wants. */
uint16_t j2534_legacy_checksum(const uint8_t *payload, size_t len);

/**
 * Build the OBD-chip request line for @p payload (the header is set
 * separately with j2534_legacy_header_cmd): a bare hex string when
 * len <= @p sf_max, else "VTFullyRequestCk<LLLL><hex><CCCC>" for the chip
 * to segment and reassemble. NUL-terminated, no CR (obd_chip_request adds
 * it). Returns the length, or 0 when len is 0, over J2534_J1850_VT_MAX, or
 * @p out does not fit.
 */
size_t j2534_legacy_build_request(const uint8_t *payload, size_t len,
                                  size_t sf_max, char *out, size_t cap);

/**
 * Decode one chip reply line into bytes: hex digit pairs, spaces ignored.
 * Returns the byte count (0 for an empty or non-hex line), capped at @p cap.
 */
size_t j2534_legacy_decode_line(const char *line, size_t line_len,
                                uint8_t *out, size_t cap);

/**
 * True when a reply line is an ELM/chip status or error word (NO DATA,
 * ERROR, BUS..., SEARCHING, STOPPED, UNABLE, '?', OK, SEARCHING) rather
 * than a hex message: it carries a letter that is not a hex digit.
 */
bool j2534_legacy_line_is_status(const char *line, size_t line_len);

/**
 * True when a decoded J1850 message is a UDS responsePending
 * ([h0 h1 h2] 7F <sid> 78): an interim to ride out, not the answer.
 */
bool j2534_legacy_is_pending(const uint8_t *msg, size_t len);

/* ---- transport-agnostic framer (pure: fed by any read function) --------- */

/** One whole wire frame: header + the largest PASSTHRU_MSG (RX_MSG). */
#define J2534_MAX_FRAME (J2534_HDR_SIZE + 24 + J2534_MAX_DATA)  /* 4164 */
/** The largest request payload the device accepts (WRITE_MSGS with one
 *  maximal message + slack). */
#define J2534_RX_CAP    (J2534_MAX_DATA + 256)

/** Read up to n bytes, blocking up to timeout_ms: >0 bytes, 0 timeout,
 *  <0 link down. The j2534_transport_t.read contract. */
typedef int (*j2534_read_fn)(void *ctx, uint8_t *buf, size_t n,
                             uint32_t timeout_ms);

typedef enum {
    J2534_FR_OK = 0,
    J2534_FR_TIMEOUT,    /* max_idle_reads consecutive empty reads      */
    J2534_FR_LINK_DOWN,  /* read returned <0                            */
    J2534_FR_BAD_HDR,    /* magic / version mismatch (stream desync)    */
    J2534_FR_TOO_BIG,    /* header announces more than payload_cap      */
    J2534_FR_STOPPED,    /* *run went false                             */
} j2534_frame_rc_t;

/**
 * Read one frame: the 12-byte header, then `length` payload bytes into
 * @p payload (cap @p payload_cap). payload == NULL = discard mode (the
 * bytes are consumed but not kept; payload_cap still bounds `length`).
 * @p per_read_ms is one read() poll; @p max_idle_reads consecutive empty
 * polls give up (0 = wait forever); @p run (nullable) aborts when false.
 * Bytes are consumed exactly: a partial chunk from the transport never
 * desyncs the stream.
 */
j2534_frame_rc_t j2534_frame_read(j2534_read_fn rd, void *ctx,
                                  uint32_t per_read_ms,
                                  uint32_t max_idle_reads,
                                  volatile const bool *run, j2534_hdr_t *h,
                                  uint8_t *payload, size_t payload_cap);

/**
 * Encode a complete ACK frame (header + status u32 [+ result u32]) into
 * @p buf. Returns 16 or 20 bytes, 0 when cap is too small.
 */
size_t j2534_ack_encode(uint8_t *buf, size_t cap, uint16_t seq,
                        uint16_t channel, uint32_t status,
                        const uint32_t *result);

/**
 * The TCP interface-exposure predicate: with allow_lan off, only
 * connections that landed on loopback, WiCAN's own SoftAP or the
 * USB-device (NCM) netif are accepted. Serial and BLE transports never
 * consult it (paired / cabled local links).
 */
bool j2534_tcp_gate_allowed(bool allow_lan, bool loopback, bool on_ap,
                            bool on_usb);

/**
 * CONNECT's baud against the bus the device sits on. The native CAN node
 * runs at the device's setting (or at what its listen-before-talk policy
 * read off the bus), not at the tester's wish: a channel opened "at 250000"
 * on a 500 kbit/s node would read and write at 500. @p bus_kbps is the
 * node's bitrate in kbit/s as the settings count it (33 = 33.333, 83 =
 * 83.333, 95 = 95.238), 0 = not known (CAN down, or automatic with nothing
 * detected yet: accepted, nothing can be sent until it is).
 * @return J2534_STATUS_NOERROR or J2534_ERR_INVALID_BAUDRATE.
 */
uint32_t j2534_connect_baud_check(uint32_t asked_bps, uint32_t bus_kbps);

/**
 * TP2.0 timing byte (high 2 bits unit, low 6 bits scale; units 0.1 / 1 /
 * 10 / 100 ms). @p ms 40 encodes as 0x68. 0 stays 0. Values above 6 ms
 * use a coarser unit, rounded up so the wire byte is not shorter than
 * the request.
 */
uint8_t j2534_tp20_encode_ms(uint32_t ms);

/** The same byte, in microseconds. */
uint32_t j2534_tp20_decode_us(uint8_t wire);

/**
 * Round a gap up to the device's 500 µs resolution (J2534-2 19.2).
 * 0 stays 0. 500 stays 500.
 */
uint32_t j2534_tp20_round_gap_us(uint32_t us);

/**
 * The key byte of telegram number @p n (from 0) of one broadcast request.
 * Both key bytes of a telegram carry it: 0x55 0x55 first, then 0xAA 0xAA,
 * changing with every telegram sent (SAE J2819 5.1.2).
 */
uint8_t j2534_tp20_broadcast_key(uint32_t n);

/**
 * Where a block is sent again from (SAE J2819 5.2: an acknowledge whose
 * sequence number is not the last telegram's plus one names the telegram
 * to go back to). The telegram that asked for the acknowledge had the
 * sequence number @p sent_seq and was telegram number @p in_block (from 0)
 * of its block; the acknowledge carries @p ack_seq.
 * @return the number of the telegram in the block to send again from
 * (0 .. in_block), or -1 when the acknowledge names no telegram of this
 * block (also for sent_seq + 1, which is the plain acknowledge).
 */
int j2534_tp20_resend_from(uint8_t sent_seq, unsigned in_block, uint8_t ack_seq);

/**
 * One GET_DEVICE_INFO parameter (SAE J2534-2 section 25, table 111).
 * @p value is the caller's Value: an output for the *_SUPPORTED and
 * *_SIMULTANEOUS parameters (0xPPQQRRSS), an input for TP2_0_PS_J1962
 * (the pin bitmap asked about: CAN-H in the high word, CAN-L in the low
 * word, bit 0 = pin 1) and left alone there.
 * @return what goes into Supported: true when the device has the
 * parameter, or for TP2_0_PS_J1962 when it has those pins. A parameter
 * the device does not know returns false with @p value untouched; the
 * request as a whole is still STATUS_NOERROR.
 */
bool j2534_device_info(uint32_t param, uint32_t *value);

#ifdef __cplusplus
}
#endif
