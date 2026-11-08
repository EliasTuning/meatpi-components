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
 * @file test_doc.c
 * @brief The scripting reference + example gallery (script_engine_doc.c,
 *        script_engine_examples.c): the tables the web UI renders must be
 *        complete and consistent, every binding documented in a known
 *        group with a signature that names it, every example a valid
 *        script name under the inline run cap, and the JSON shapes the
 *        Scripts page reads.
 */
#include <string.h>

#include "unity.h"

#include "script_engine_doc.h"

bool se_script_name_ok(const char *name);   /* script_engine_name.c */

void test_doc_bindings_table_is_consistent(void)
{
    size_t n = se_bind_doc_count();

    TEST_ASSERT_GREATER_THAN(10, n);

    for (size_t i = 0; i < n; i++)
    {
        const se_bind_doc_t *d = se_bind_doc(i);

        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_TRUE(strlen(d->name) > 0);
        TEST_ASSERT_TRUE_MESSAGE(se_doc_group_ok(d->group), d->name);
        /* the signature starts with the name and an opening bracket */
        TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(d->name, d->sig, strlen(d->name), d->name);
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('(', d->sig[strlen(d->name)], d->name);
        TEST_ASSERT_TRUE_MESSAGE(strlen(d->doc) > 20, d->name);
        TEST_ASSERT_TRUE_MESSAGE(strlen(d->ret) > 0, d->name);
        /* the example mentions the binding */
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(d->ex, d->name), d->name);

        /* names are unique */
        for (size_t j = 0; j < i; j++)
        {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(se_bind_doc(j)->name, d->name) != 0, d->name);
        }
    }

    TEST_ASSERT_NULL(se_bind_doc(n));
    TEST_ASSERT_NOT_NULL(se_bind_doc_find("obd_request"));
    TEST_ASSERT_NULL(se_bind_doc_find("nope"));
    TEST_ASSERT_NULL(se_bind_doc_find(NULL));
    TEST_ASSERT_FALSE(se_doc_group_ok("misc"));
    TEST_ASSERT_FALSE(se_doc_group_ok(NULL));
}

void test_doc_reference_json_shape(void)
{
    const se_doc_ctx_t ctx =
    {
        .language = "Berry 9.9.9", .enabled = true, .allow_reflash = false,
        .max_runtime_ms = 12345,
    };
    cJSON *o = se_doc_reference(&ctx);

    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("Berry 9.9.9",
                             cJSON_GetObjectItem(o, "language")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(o, "enabled")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(o, "allow_reflash")));

    cJSON *lim = cJSON_GetObjectItem(o, "limits");

    TEST_ASSERT_EQUAL(SE_SRC_MAX, cJSON_GetObjectItem(lim, "src_max")->valueint);
    TEST_ASSERT_EQUAL(SE_OUT_MAX, cJSON_GetObjectItem(lim, "out_max")->valueint);
    TEST_ASSERT_EQUAL(12345, cJSON_GetObjectItem(lim, "max_runtime_ms")->valueint);

    TEST_ASSERT_EQUAL((int)se_bind_doc_count(),
                      cJSON_GetArraySize(cJSON_GetObjectItem(o, "bindings")));

    cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(o, "bindings"), 0);

    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "sig"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "group"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "doc"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "ex"));

    /* every binding's group is one of the listed groups */
    cJSON *groups = cJSON_GetObjectItem(o, "groups");
    cJSON *b;

    cJSON_ArrayForEach(b, cJSON_GetObjectItem(o, "bindings"))
    {
        const char *g = cJSON_GetObjectItem(b, "group")->valuestring;
        bool found = false;
        cJSON *gg;

        cJSON_ArrayForEach(gg, groups)
        {
            if (strcmp(cJSON_GetObjectItem(gg, "id")->valuestring, g) == 0)
            {
                found = true;
            }
        }

        TEST_ASSERT_TRUE_MESSAGE(found, g);
    }

    TEST_ASSERT_GREATER_THAN(5, cJSON_GetArraySize(cJSON_GetObjectItem(o, "globals")));
    TEST_ASSERT_GREATER_THAN(5, cJSON_GetArraySize(cJSON_GetObjectItem(o, "primer")));
    TEST_ASSERT_GREATER_THAN(5, cJSON_GetArraySize(cJSON_GetObjectItem(o, "errors")));
    TEST_ASSERT_EQUAL_STRING("script.run",
        cJSON_GetObjectItem(cJSON_GetObjectItem(o, "rules"), "action")->valuestring);
    cJSON_Delete(o);

    /* a NULL context still yields a document */
    o = se_doc_reference(NULL);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(o, "enabled")));
    cJSON_Delete(o);
}

void test_examples_are_valid_scripts(void)
{
    size_t n = se_example_count();

    TEST_ASSERT_GREATER_THAN(4, n);

    for (size_t i = 0; i < n; i++)
    {
        const se_example_t *x = se_example_get(i);
        char fname[64];

        TEST_ASSERT_NOT_NULL(x);
        /* the id doubles as the suggested file name */
        snprintf(fname, sizeof(fname), "%s.be", x->id);
        TEST_ASSERT_TRUE_MESSAGE(se_script_name_ok(fname), x->id);
        TEST_ASSERT_TRUE_MESSAGE(strlen(x->title) > 0, x->id);
        TEST_ASSERT_TRUE_MESSAGE(strlen(x->desc) > 10, x->id);
        TEST_ASSERT_TRUE_MESSAGE(x->level >= 1 && x->level <= 3, x->id);
        TEST_ASSERT_TRUE_MESSAGE(strcmp(x->needs, "") == 0 || strcmp(x->needs, "vehicle") == 0 ||
                                 strcmp(x->needs, "dtc") == 0 || strcmp(x->needs, "can") == 0, x->id);
        /* runnable inline: under the run body cap with JSON overhead to spare */
        TEST_ASSERT_TRUE_MESSAGE(strlen(x->src) > 100 && strlen(x->src) < SE_SRC_MAX - 512, x->id);
        /* starts with a comment header, ends with a newline */
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('#', x->src[0], x->id);
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('\n', x->src[strlen(x->src) - 1], x->id);
        /* no tabs (the editor indents with two spaces) */
        TEST_ASSERT_NULL_MESSAGE(strchr(x->src, '\t'), x->id);

        for (size_t j = 0; j < i; j++)
        {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(se_example_get(j)->id, x->id) != 0, x->id);
        }
    }

    TEST_ASSERT_NULL(se_example_get(n));
    TEST_ASSERT_NOT_NULL(se_example_find("hello"));
    TEST_ASSERT_NULL(se_example_find("nope"));
    TEST_ASSERT_NULL(se_example_find(NULL));
}

void test_examples_only_use_documented_bindings(void)
{
    /* every "name(" call of a device binding an example makes must be a
       documented binding: a renamed binding shows up here first */
    static const char *const DEVICE_PREFIXES[] = { "uds", "obd_", "dtc_", "can_", "emit", "sleep_ms", "millis", "log(" };

    for (size_t i = 0; i < se_example_count(); i++)
    {
        const char *src = se_example_get(i)->src;

        for (size_t p = 0; p < sizeof(DEVICE_PREFIXES) / sizeof(DEVICE_PREFIXES[0]); p++)
        {
            const char *at = src;

            while ((at = strstr(at, DEVICE_PREFIXES[p])) != NULL)
            {
                /* extract the identifier at `at` */
                char ident[40];
                size_t k = 0;

                while (k < sizeof(ident) - 1 &&
                       ((at[k] >= 'a' && at[k] <= 'z') || at[k] == '_' ||
                        (at[k] >= '0' && at[k] <= '9')))
                {
                    ident[k] = at[k];
                    k++;
                }

                ident[k] = '\0';

                bool is_call = at[k] == '(';
                bool mid_word = at > src && ((at[-1] >= 'a' && at[-1] <= 'z') || at[-1] == '_' || (at[-1] >= 'A' && at[-1] <= 'Z'));

                if (is_call && !mid_word)
                {
                    TEST_ASSERT_NOT_NULL_MESSAGE(se_bind_doc_find(ident), ident);
                }

                at += (k > 0) ? k : 1;
            }
        }
    }
}

void test_examples_json_lists_without_sources(void)
{
    cJSON *o = se_doc_examples();

    TEST_ASSERT_NOT_NULL(o);

    cJSON *arr = cJSON_GetObjectItem(o, "examples");

    TEST_ASSERT_EQUAL((int)se_example_count(), cJSON_GetArraySize(arr));

    cJSON *e = cJSON_GetArrayItem(arr, 0);

    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(e, "id")->valuestring);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(e, "title"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(e, "desc"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(e, "needs"));
    TEST_ASSERT_EQUAL(1, cJSON_GetObjectItem(e, "level")->valueint);
    TEST_ASSERT_EQUAL((int)strlen(se_example_get(0)->src), cJSON_GetObjectItem(e, "size")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(e, "src"));
    cJSON_Delete(o);
}
