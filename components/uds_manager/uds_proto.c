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
 * @file uds_proto.c
 * @brief Pure UDS helpers (see uds_proto.h). No I/O — host-tested.
 */
#include "uds_proto.h"

bool uds_is_negative(const uint8_t *resp, size_t len)
{
    return resp != NULL && len >= 3 && resp[0] == UDS_NR_SID;
}

bool uds_is_pending(const uint8_t *resp, size_t len)
{
    return uds_is_negative(resp, len) && resp[2] == UDS_NRC_PENDING;
}

bool uds_is_positive_for(uint8_t req_sid, const uint8_t *resp, size_t len)
{
    return resp != NULL && len >= 1 &&
           resp[0] == (uint8_t)(req_sid + UDS_POS_OFFSET);
}

bool uds_response_matches(uint8_t req_sid, const uint8_t *resp, size_t len)
{
    return uds_is_positive_for(req_sid, resp, len) ||
           (uds_is_negative(resp, len) && resp[1] == req_sid);
}

uint8_t uds_nrc_of(const uint8_t *resp, size_t len)
{
    return uds_is_negative(resp, len) ? resp[2] : 0u;
}

const char *uds_nrc_name(uint8_t nrc)
{
    switch (nrc)
    {
    case 0x10: return "generalReject";
    case 0x11: return "serviceNotSupported";
    case 0x12: return "subFunctionNotSupported";
    case 0x13: return "incorrectMessageLengthOrInvalidFormat";
    case 0x14: return "responseTooLong";
    case 0x21: return "busyRepeatRequest";
    case 0x22: return "conditionsNotCorrect";
    case 0x24: return "requestSequenceError";
    case 0x25: return "noResponseFromSubnetComponent";
    case 0x26: return "failurePreventsExecutionOfRequestedAction";
    case 0x31: return "requestOutOfRange";
    case 0x33: return "securityAccessDenied";
    case 0x35: return "invalidKey";
    case 0x36: return "exceedNumberOfAttempts";
    case 0x37: return "requiredTimeDelayNotExpired";
    case 0x70: return "uploadDownloadNotAccepted";
    case 0x71: return "transferDataSuspended";
    case 0x72: return "generalProgrammingFailure";
    case 0x73: return "wrongBlockSequenceCounter";
    case 0x78: return "requestCorrectlyReceived-ResponsePending";
    case 0x7E: return "subFunctionNotSupportedInActiveSession";
    case 0x7F: return "serviceNotSupportedInActiveSession";
    case 0x81: return "rpmTooHigh";
    case 0x82: return "rpmTooLow";
    case 0x83: return "engineIsRunning";
    case 0x84: return "engineIsNotRunning";
    case 0x85: return "engineRunTimeTooLow";
    case 0x86: return "temperatureTooHigh";
    case 0x87: return "temperatureTooLow";
    case 0x88: return "vehicleSpeedTooHigh";
    case 0x89: return "vehicleSpeedTooLow";
    case 0x8A: return "throttle/PedalTooHigh";
    case 0x8B: return "throttle/PedalTooLow";
    case 0x8C: return "transmissionRangeNotInNeutral";
    case 0x8D: return "transmissionRangeNotInGear";
    case 0x8F: return "brakeSwitch(es)NotClosed";
    case 0x90: return "shifterLeverNotInPark";
    case 0x91: return "torqueConverterClutchLocked";
    case 0x92: return "voltageTooHigh";
    case 0x93: return "voltageTooLow";
    default:   return "unknown";
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool uds_hex_to_bytes(const char *str, uint8_t *out, size_t cap,
                      size_t *out_len)
{
    if (str == NULL || out == NULL || out_len == NULL)
    {
        return false;
    }

    size_t n = 0;
    int hi = -1; /* pending high nibble, -1 = none */

    for (const char *p = str; *p != '\0'; p++)
    {
        char c = *p;

        if (c == ' ' || c == ':' || c == '-' || c == '\t' ||
            c == '\r' || c == '\n' || c == ',')
        {
            continue; /* separators ignored */
        }

        /* skip a 0x / 0X prefix at a byte boundary */
        if (hi < 0 && c == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            p++;
            continue;
        }

        int v = hex_nibble(c);

        if (v < 0)
        {
            return false; /* junk char */
        }

        if (hi < 0)
        {
            hi = v;
        }
        else
        {
            if (n >= cap)
            {
                return false; /* overflow */
            }

            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }

    if (hi >= 0)
    {
        return false; /* dangling nibble */
    }

    *out_len = n;
    return true;
}

size_t uds_bytes_to_hex(const uint8_t *bytes, size_t len, char *out,
                        size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return 0;
    }

    if (bytes == NULL || len == 0)
    {
        out[0] = '\0';
        return 0;
    }

    if (len * 3 > cap) /* "XX " per byte, last space becomes NUL */
    {
        out[0] = '\0';
        return 0;
    }

    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;

    for (size_t i = 0; i < len; i++)
    {
        out[o++] = HEX[bytes[i] >> 4];
        out[o++] = HEX[bytes[i] & 0x0F];

        if (i + 1 < len)
        {
            out[o++] = ' ';
        }
    }

    out[o] = '\0';
    return o;
}
