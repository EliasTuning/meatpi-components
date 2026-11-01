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
 * @file obd_chip_fw_version.c
 * @brief The chip firmware version policy, pure and host-tested (issue
 *        #1023; split out of obd_chip_parse.c on 2026-11-01, the 700-line
 *        rule): the versions the start-up leaves alone (the v4.52p accept
 *        list), the version numbers parsed out of a VTVERS answer, the
 *        packaged constant or a file's path, their order, the version as a
 *        token, and `is_outdated` (the chip's version below the image's):
 *        the one comparison every update follows since the forced rewrite
 *        went (Ali, 2026-11-01).
 */
#include "obd_chip_private.h"

#include <stdio.h>
#include <string.h>

/* ---- packaged firmware policy (issue #1023, 2026-10-28) ----------------------
 * The chip firmware versions the packaged image leaves alone. Legacy v4.52p
 * (main fab54ad, 2026-10-12) bundles V2.3.24 and treats a chip at V2.3.22
 * or V2.3.24 as up to date; v6 bundled V2.3.22 alone and DOWNGRADED a chip
 * that v4.52p had taken to V2.3.24, a 3557-record write on the first v6
 * boot. Keep in step with OBD_FW_BUILTIN_VERSION in obd_chip_fw.c: the
 * packaged version must be in this list (host-tested). */
static const char *const OBD_FW_CURRENT[] = { "V2.3.22", "V2.3.24" };

bool obd_fw_version_is_current(const char *vtvers)
{
    if (vtvers == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < sizeof(OBD_FW_CURRENT) / sizeof(OBD_FW_CURRENT[0]);
         i++)
    {
        if (strstr(vtvers, OBD_FW_CURRENT[i]) != NULL)
        {
            return true;
        }
    }

    return false;
}

const char *obd_fw_current_versions(void)
{
    return "V2.3.22, V2.3.24";
}

/* one decimal field of a version, bounded; advances @p p past the digits */
static bool version_field(const char **p, unsigned *out)
{
    const char *s = *p;
    unsigned n = 0;

    if (*s < '0' || *s > '9')
    {
        return false;
    }

    while (*s >= '0' && *s <= '9')
    {
        n = n * 10 + (unsigned)(*s - '0');

        if (n > 99999)
        {
            return false;
        }

        s++;
    }

    *out = n;
    *p = s;
    return true;
}

bool obd_fw_version_parse(const char *s, unsigned out[3])
{
    /* the first "V<major>.<minor>.<patch>" in the text: the chip's VTVERS
       answer ("MIC3624 V2.3.24\r\r>"), the packaged constant, a file's
       path ("/data/obd_fw/V2.3.24.txt"); a 'V' not followed by three
       numbers (VTVERS, VERS) is skipped */
    if (s == NULL)
    {
        return false;
    }

    for (const char *v = s; (v = strchr(v, 'V')) != NULL; v++)
    {
        const char *p = v + 1;
        unsigned n[3];
        size_t k;

        for (k = 0; k < 3; k++)
        {
            if (k > 0)
            {
                if (*p != '.')
                {
                    break;
                }

                p++;
            }

            if (!version_field(&p, &n[k]))
            {
                break;
            }
        }

        if (k == 3)
        {
            out[0] = n[0];
            out[1] = n[1];
            out[2] = n[2];
            return true;
        }
    }

    return false;
}

int obd_fw_version_cmp(const unsigned a[3], const unsigned b[3])
{
    for (size_t i = 0; i < 3; i++)
    {
        if (a[i] != b[i])
        {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }

    return 0;
}

size_t obd_fw_version_token(const char *s, char *out, size_t out_len)
{
    unsigned v[3];

    if (out == NULL || out_len == 0)
    {
        return 0;
    }

    out[0] = '\0';

    if (!obd_fw_version_parse(s, v))
    {
        return 0;
    }

    int n = snprintf(out, out_len, "V%u.%u.%u", v[0], v[1], v[2]);

    if (n <= 0 || (size_t)n >= out_len)
    {
        out[0] = '\0';
        return 0;
    }

    return (size_t)n;
}

bool obd_fw_version_is_outdated(const char *vtvers, const char *image)
{
    /* The chip answered with a version and the image's is above it,
       compared field by field as numbers (V2.3.9 is below V2.3.24). A chip
       at the image's version or above it is not outdated: no path writes
       such a chip (2026-11-01, Ali: no forced rewrite; a chip left on
       V2.3.22 under the packaged V2.3.24 is flagged and the user's update
       lifts it, 2026-10-28). Silence, '?' and an image without a version
       are not "outdated" either: a chip down or in its bootloader is
       reported on its own. */
    unsigned chip[3];
    unsigned img[3];

    if (!obd_fw_version_parse(vtvers, chip) || !obd_fw_version_parse(image, img))
    {
        return false;
    }

    return obd_fw_version_cmp(chip, img) < 0;
}
