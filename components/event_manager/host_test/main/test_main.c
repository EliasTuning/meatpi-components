/**
 * @file test_main.c
 * @brief Host suite for event_manager's pure modules: rule
 *        parse/validate, match/when evaluation (every op incl.
 *        `changed`), cooldown arithmetic (fake µs clock), template
 *        substitution.
 */
#include <string.h>

#include "unity.h"

#include "event_manager_private.h"

static em_rule_t s_rules[EM_RULES_MAX];
static int s_count;
static char s_err[96];

static esp_err_t parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    esp_err_t rc = em_rules_parse(root, s_rules, EM_RULES_MAX, &s_count,
                                  s_err, sizeof(s_err));

    cJSON_Delete(root);
    return rc;
}

static em_event_t ev_param(const char *param, double value)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "autopid");
    snprintf(ev.name, sizeof(ev.name), "param");
    ev.ts_us = 123456789;
    ev.kv[0] = (em_kv_t){ .key = "param", .type = EM_VAL_STR };
    snprintf(ev.kv[0].v.str, EM_STR_MAX, "%s", param);
    ev.kv[1] = (em_kv_t){ .key = "value", .type = EM_VAL_F64 };
    ev.kv[1].v.f64 = value;
    ev.n = 2;
    return ev;
}

/* ---- parsing ------------------------------------------------------------------- */

/* ---- live-value conditions + undo (2026-09-17) ---------------------- */
static const char *s_soc = "25";
static const char *s_ssid = "HomeAP";

static esp_err_t stub_resolve(const char *name, char *out, size_t len)
{
    if (strcmp(name, "autopid.SOC") == 0)
    {
        snprintf(out, len, "%s", s_soc);
        return ESP_OK;
    }

    if (strcmp(name, "wifi.ssid") == 0)
    {
        snprintf(out, len, "%s", s_ssid);
        return ESP_OK;
    }

    return ESP_FAIL;
}

#define CHARGE_OBJ \
    "{\"name\":\"charge_poll\",\"on\":\"autopid.param\"," \
    "\"match\":{\"param\":\"CHARGING\"}," \
    "\"when\":[{\"key\":\"value\",\"op\":\"==\",\"val\":1}," \
    "{\"value\":\"${autopid.SOC}\",\"op\":\">\",\"val\":20}]," \
    "\"do\":\"autopid.group\"," \
    "\"with\":{\"group\":\"charging\",\"enabled\":true},\"undo\":true}"
#define CHARGE_RULE "[" CHARGE_OBJ "]"

void test_parse_live_value_and_undo(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));
    TEST_ASSERT_EQUAL(2, s_rules[0].n_when);
    TEST_ASSERT_EQUAL_STRING("value", s_rules[0].when[0].key);
    TEST_ASSERT_EQUAL_STRING("", s_rules[0].when[0].value);
    TEST_ASSERT_EQUAL_STRING("", s_rules[0].when[1].key);
    TEST_ASSERT_EQUAL_STRING("${autopid.SOC}", s_rules[0].when[1].value);
    TEST_ASSERT_EQUAL(EM_OP_GT, s_rules[0].when[1].op);
    TEST_ASSERT_TRUE(s_rules[0].undo);
    TEST_ASSERT_TRUE(em_rule_has_live(&s_rules[0]));
    /* neither key nor value / both / a value without a template */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"op\":\"==\",\"val\":1}]}]"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"key\":\"v\",\"value\":\"${a}\",\"op\":\"==\","
        "\"val\":1}]}]"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"value\":\"autopid.SOC\",\"op\":\"==\",\"val\":1}]}]"));
    /* undo defaults to false */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\"}]"));
    TEST_ASSERT_FALSE(s_rules[0].undo);
    TEST_ASSERT_FALSE(em_rule_has_live(&s_rules[0]));
}

void test_live_value_conditions(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));

    em_rule_state_t st;
    em_event_t on = ev_param("CHARGING", 1);
    em_event_t off = ev_param("CHARGING", 0);

    memset(&st, 0, sizeof(st));
    s_soc = "25";
    TEST_ASSERT_TRUE(em_rule_when_ex(&s_rules[0], &on, &st, stub_resolve));
    s_soc = "15";
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &on, &st, stub_resolve));
    s_soc = "25";
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &off, &st, stub_resolve));
    /* no resolver: a live value never holds (the plain wrapper) */
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &on, &st));

    /* strings compare as strings */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"home\",\"on\":\"wifi.sta\","
        "\"when\":[{\"value\":\"${wifi.ssid}\",\"op\":\"==\",\"val\":\"HomeAP\"}],"
        "\"do\":\"log.note\"}]"));

    em_event_t w = { 0 };

    snprintf(w.source, sizeof(w.source), "wifi");
    snprintf(w.name, sizeof(w.name), "sta");
    s_ssid = "HomeAP";
    TEST_ASSERT_TRUE(em_rule_when_ex(&s_rules[0], &w, &st, stub_resolve));
    s_ssid = "Cafe";
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &w, &st, stub_resolve));
}

void test_live_recheck(void)
{
    em_rule_state_t st;

    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));
    st.last_ev = ev_param("CHARGING", 1);
    s_soc = "25";
    TEST_ASSERT_TRUE(em_rule_live_holds(&s_rules[0], &st, stub_resolve));
    s_soc = "10";
    TEST_ASSERT_FALSE(em_rule_live_holds(&s_rules[0], &st, stub_resolve));
    /* an unresolvable value fails the re-check */
    TEST_ASSERT_FALSE(em_rule_live_holds(&s_rules[0], &st, NULL));
    /* only live conditions are re-checked: trigger fields and `changed`
       move with events, so a rule without live values always holds */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"x\",\"on\":\"autopid.param\","
        "\"when\":[{\"key\":\"value\",\"op\":\">\",\"val\":1}],\"do\":\"log.note\"}]"));
    TEST_ASSERT_TRUE(em_rule_live_holds(&s_rules[0], &st, stub_resolve));
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"x\",\"on\":\"autopid.param\","
        "\"when\":[{\"value\":\"${autopid.SOC}\",\"op\":\"changed\"}],\"do\":\"log.note\"}]"));
    TEST_ASSERT_TRUE(em_rule_live_holds(&s_rules[0], &st, stub_resolve));
}

static bool undoable_group(const char *a) { return strcmp(a, "autopid.group") == 0; }
static bool undoable_none(const char *a) { (void)a; return false; }
static const char *s_live = "";

static esp_err_t stub_live(const char *name, char *out, size_t len)
{
    if (strcmp(name, "x.y") == 0)
    {
        snprintf(out, len, "%s", s_live);
        return ESP_OK;
    }

    return ESP_FAIL;
}

void test_while_rule_step(void)
{
    em_rule_state_t st;
    em_event_t on = ev_param("CHARGING", 1);
    em_event_t off = ev_param("CHARGING", 0);

    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));
    s_soc = "25";

    bool holds = em_rule_when_ex(&s_rules[0], &on, &st, stub_resolve);

    TEST_ASSERT_TRUE(holds);
    TEST_ASSERT_EQUAL(EM_STEP_RUN, em_rule_step(&s_rules[0], &st, holds, &on));
    TEST_ASSERT_EQUAL(1, st.last_ev.kv[1].v.f64);  /* kept for the re-check */
    st.active = true;                                   /* the caller armed it     */
    TEST_ASSERT_EQUAL(EM_STEP_NONE, em_rule_step(&s_rules[0], &st, true, &on));
    TEST_ASSERT_TRUE(st.active);                        /* still holding: nothing  */
    TEST_ASSERT_EQUAL(EM_STEP_UNDO, em_rule_step(&s_rules[0], &st, false, &off));
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_EQUAL(EM_STEP_NONE, em_rule_step(&s_rules[0], &st, false, &off));
    /* a plain rule runs on every holding event and never undoes */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"x\",\"on\":\"autopid.param\","
        "\"when\":[{\"key\":\"value\",\"op\":\">\",\"val\":0}],\"do\":\"log.note\"}]"));
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(EM_STEP_RUN, em_rule_step(&s_rules[0], &st, true, &on));
    TEST_ASSERT_EQUAL(EM_STEP_RUN, em_rule_step(&s_rules[0], &st, true, &on));
    TEST_ASSERT_EQUAL(EM_STEP_NONE, em_rule_step(&s_rules[0], &st, false, &off));
    TEST_ASSERT_FALSE(st.active);
}

void test_while_rule_recheck(void)
{
    em_rule_state_t st;

    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));
    st.last_ev = ev_param("CHARGING", 1);
    s_soc = "25";
    TEST_ASSERT_FALSE(em_rule_recheck(&s_rules[0], &st, stub_resolve)); /* not active */
    st.active = true;
    TEST_ASSERT_FALSE(em_rule_recheck(&s_rules[0], &st, stub_resolve)); /* still holds */
    TEST_ASSERT_TRUE(st.active);
    s_soc = "10";
    TEST_ASSERT_TRUE(em_rule_recheck(&s_rules[0], &st, stub_resolve));  /* undo due */
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_FALSE(em_rule_recheck(&s_rules[0], &st, stub_resolve)); /* only once */
    /* a disabled rule never re-checks even while armed */
    st.active = true;
    s_rules[0].enabled = false;
    TEST_ASSERT_FALSE(em_rule_recheck(&s_rules[0], &st, stub_resolve));
    TEST_ASSERT_TRUE(st.active);
    /* no live conditions: nothing to re-check between events */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"x\",\"on\":\"wifi.sta\","
        "\"when\":[{\"key\":\"connected\",\"op\":\"==\",\"val\":true}],"
        "\"do\":\"autopid.group\",\"with\":{\"group\":\"default\",\"enabled\":true},"
        "\"undo\":true}]"));
    st.active = true;
    TEST_ASSERT_FALSE(em_rule_recheck(&s_rules[0], &st, stub_resolve));
    TEST_ASSERT_TRUE(st.active);
}

void test_validate_undo(void)
{
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));
    TEST_ASSERT_EQUAL(ESP_OK, em_rules_validate_undo(s_rules, s_count, undoable_group,
                                                     err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      em_rules_validate_undo(s_rules, s_count, undoable_none, err,
                                             sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "charge_poll"));
    TEST_ASSERT_NOT_NULL(strstr(err, "autopid.group"));
    TEST_ASSERT_NOT_NULL(strstr(err, "cannot undo"));
    /* no undo: any action passes */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\"}]"));
    TEST_ASSERT_EQUAL(ESP_OK, em_rules_validate_undo(s_rules, s_count, undoable_none,
                                                     err, sizeof(err)));
}

void test_live_value_kinds(void)
{
    em_rule_state_t st;
    em_event_t ev = ev_param("P", 1);

    memset(&st, 0, sizeof(st));
    /* booleans render as true/false and compare against JSON booleans */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"b\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"value\":\"${x.y}\",\"op\":\"==\",\"val\":true}]}]"));
    s_live = "true";
    TEST_ASSERT_TRUE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
    s_live = "false";
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
    /* decimals compare numerically; text uses contains; empty/null never holds */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"n\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"value\":\"${x.y}\",\"op\":\">=\",\"val\":12.5}]}]"));
    s_live = "12.5";
    TEST_ASSERT_TRUE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
    s_live = "12.49";
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
    s_live = "";
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"s\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"value\":\"${x.y}\",\"op\":\"contains\",\"val\":\"AP\"}]}]"));
    s_live = "HomeAP";
    TEST_ASSERT_TRUE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
    s_live = "null";
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
    /* an unknown name renders `null` (unresolved): never holds */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"u\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"value\":\"${no.such}\",\"op\":\"!=\",\"val\":0}]}]"));
    TEST_ASSERT_FALSE(em_rule_when_ex(&s_rules[0], &ev, &st, stub_live));
}

/* ---- scenario harness (2026-09-17): the engine's per-event decision replayed
   over scripted event sequences and live-value drifts, tallying what each
   rule would have done — rule COMBINATIONS, not single calls ------------ */
static em_rule_state_t s_scn_st[EM_RULES_MAX];
static int s_runs[EM_RULES_MAX];
static int s_undos[EM_RULES_MAX];
static int s_supp;
static const char *s_epoch = "1000";
static const char *s_speed = "0";

static esp_err_t scn_resolve(const char *name, char *out, size_t len)
{
    const char *v = NULL;

    if (strcmp(name, "autopid.SOC") == 0)
    {
        v = s_soc;
    }
    else if (strcmp(name, "autopid.SPEED") == 0)
    {
        v = s_speed;
    }
    else if (strcmp(name, "wifi.ssid") == 0)
    {
        v = s_ssid;
    }
    else if (strcmp(name, "time.epoch") == 0)
    {
        v = s_epoch;
    }

    if (v == NULL)
    {
        return ESP_FAIL;
    }

    snprintf(out, len, "%s", v);
    return ESP_OK;
}

static void scn_reset(void)
{
    memset(s_scn_st, 0, sizeof(s_scn_st));
    memset(s_runs, 0, sizeof(s_runs));
    memset(s_undos, 0, sizeof(s_undos));
    s_supp = 0;
    s_soc = "25";
    s_ssid = "HomeAP";
    s_epoch = "1000";
    s_speed = "0";
}

/** One event through every rule, like dispatch_one(). */
static void feed(const em_event_t *ev, int64_t now_us)
{
    char sel[EM_SEL_LEN];

    snprintf(sel, sizeof(sel), "%s.%s", ev->source, ev->name);

    for (int r = 0; r < s_count; r++)
    {
        bool sup = false;

        switch (em_rule_decide(&s_rules[r], &s_scn_st[r], ev, sel, now_us,
                               scn_resolve, &sup))
        {
            case EM_STEP_RUN:
                s_runs[r]++;
                em_rule_applied(&s_rules[r], &s_scn_st[r]);
                break;
            case EM_STEP_UNDO:
                s_undos[r]++;
                break;
            default:
                break;
        }

        if (sup)
        {
            s_supp++;
        }
    }
}

/** The dispatcher's 1 s re-check pass. */
static void tick(void)
{
    for (int r = 0; r < s_count; r++)
    {
        if (em_rule_recheck(&s_rules[r], &s_scn_st[r], scn_resolve))
        {
            s_undos[r]++;
        }
    }
}

static em_event_t ev_wifi(bool connected, const char *ssid)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "wifi");
    snprintf(ev.name, sizeof(ev.name), "sta");
    ev.kv[0] = (em_kv_t){ .key = "connected", .type = EM_VAL_BOOL };
    ev.kv[0].v.b = connected;
    ev.kv[1] = (em_kv_t){ .key = "ssid", .type = EM_VAL_STR };
    snprintf(ev.kv[1].v.str, EM_STR_MAX, "%s", ssid);
    ev.n = 2;
    return ev;
}

static em_event_t ev_tick(const char *timer)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "timer");
    snprintf(ev.name, sizeof(ev.name), "tick");
    ev.kv[0] = (em_kv_t){ .key = "timer", .type = EM_VAL_STR };
    snprintf(ev.kv[0].v.str, EM_STR_MAX, "%s", timer);
    ev.n = 1;
    return ev;
}

#define WIFI_RULE \
    "{\"name\":\"home_slow\",\"on\":\"wifi.sta\"," \
    "\"when\":[{\"key\":\"connected\",\"op\":\"==\",\"val\":true}," \
    "{\"key\":\"ssid\",\"op\":\"==\",\"val\":\"HomeAP\"}]," \
    "\"do\":\"autopid.group\",\"with\":{\"group\":\"default\",\"enabled\":true," \
    "\"period_ms\":10000},\"undo\":true}"

void test_scn_wifi_while_rule(void)
{
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse("[" WIFI_RULE "]"));

    em_event_t up = ev_wifi(true, "HomeAP");
    em_event_t cafe = ev_wifi(true, "Cafe");
    em_event_t down = ev_wifi(false, "");

    feed(&up, 1);                       /* connect at home: slow down     */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    TEST_ASSERT_TRUE(s_scn_st[0].active);
    feed(&up, 2);                       /* a repeat connect: nothing      */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&down, 3);                     /* drop: back to normal           */
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    TEST_ASSERT_FALSE(s_scn_st[0].active);
    feed(&down, 4);                     /* a second drop: nothing         */
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    feed(&cafe, 5);                     /* another network: not home      */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&up, 6);                       /* home again: slow down again    */
    TEST_ASSERT_EQUAL(2, s_runs[0]);
    TEST_ASSERT_EQUAL(0, s_supp);
}

void test_scn_live_value_drift(void)
{
    /* CHARGING is 1 and SOC above 20 enables a group; SOC drifting down
       while CHARGING never changes must still undo (the 1 s re-check) */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));

    em_event_t on = ev_param("CHARGING", 1);

    tick();                             /* nothing armed yet              */
    TEST_ASSERT_EQUAL(0, s_undos[0]);
    feed(&on, 1);
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    tick();                             /* SOC 25: still holds            */
    TEST_ASSERT_EQUAL(0, s_undos[0]);
    s_soc = "10";
    tick();                             /* drifted below: undo            */
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    TEST_ASSERT_FALSE(s_scn_st[0].active);
    tick();                             /* only once                      */
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    s_soc = "30";
    tick();                             /* back above: NO re-arm without  */
    TEST_ASSERT_EQUAL(1, s_runs[0]);    /* a trigger event                */
    feed(&on, 2);                       /* the next CHARGING event does   */
    TEST_ASSERT_EQUAL(2, s_runs[0]);
}

void test_scn_plain_and_while_share_a_trigger(void)
{
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[" WIFI_RULE ","
        "{\"name\":\"log_wifi\",\"on\":\"wifi.sta\",\"do\":\"log.note\","
        "\"with\":{\"message\":\"wifi ${connected} ${ssid}\"}}]"));

    em_event_t up = ev_wifi(true, "HomeAP");
    em_event_t down = ev_wifi(false, "");

    feed(&up, 1);
    feed(&up, 2);
    feed(&down, 3);
    feed(&down, 4);
    TEST_ASSERT_EQUAL(1, s_runs[0]);    /* while-rule: transitions only   */
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    TEST_ASSERT_EQUAL(4, s_runs[1]);    /* plain rule: every event        */
    TEST_ASSERT_EQUAL(0, s_undos[1]);
}

void test_scn_match_isolates_other_parameters(void)
{
    /* match {param: CHARGING}: SPEED events never touch the rule, so they
       can neither arm nor undo it */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(CHARGE_RULE));

    em_event_t on = ev_param("CHARGING", 1);
    em_event_t speed = ev_param("SPEED", 0);

    feed(&speed, 1);
    TEST_ASSERT_EQUAL(0, s_runs[0]);
    feed(&on, 2);
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&speed, 3);                    /* value 0 would fail `it is 1`   */
    TEST_ASSERT_EQUAL(0, s_undos[0]);   /* but SPEED is not our param     */
    TEST_ASSERT_TRUE(s_scn_st[0].active);
}

void test_scn_state_in_match_never_undoes(void)
{
    /* the documented trap: a while-rule whose trigger STATE sits in
       `match` never sees the opposite event, so it never undoes — the
       builder writes states into `when` for exactly this reason */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"trap\",\"on\":\"wifi.sta\",\"match\":{\"connected\":true},"
        "\"do\":\"autopid.group\",\"with\":{\"group\":\"default\",\"enabled\":true},"
        "\"undo\":true}]"));

    em_event_t up = ev_wifi(true, "HomeAP");
    em_event_t down = ev_wifi(false, "");

    feed(&up, 1);
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&down, 2);
    TEST_ASSERT_EQUAL(0, s_undos[0]);   /* skipped by match: stays active */
    TEST_ASSERT_TRUE(s_scn_st[0].active);
}

void test_scn_cooldown_on_a_while_rule(void)
{
    /* cooldown gates the ARMING only; the undo is never held back */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"cd\",\"on\":\"wifi.sta\","
        "\"when\":[{\"key\":\"connected\",\"op\":\"==\",\"val\":true}],"
        "\"do\":\"autopid.group\",\"with\":{\"group\":\"default\",\"enabled\":true},"
        "\"undo\":true,\"cooldown_ms\":60000}]"));

    em_event_t up = ev_wifi(true, "HomeAP");
    em_event_t down = ev_wifi(false, "");
    int64_t s = 1000000;

    feed(&up, 1 * s);
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&down, 2 * s);
    TEST_ASSERT_EQUAL(1, s_undos[0]);   /* undo 1 s later: not suppressed */
    feed(&up, 3 * s);                   /* re-arm inside the cooldown     */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    TEST_ASSERT_EQUAL(1, s_supp);
    TEST_ASSERT_FALSE(s_scn_st[0].active);
    feed(&down, 4 * s);                 /* not active: nothing to undo    */
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    feed(&up, 62 * s);                  /* cooldown over                  */
    TEST_ASSERT_EQUAL(2, s_runs[0]);
    TEST_ASSERT_TRUE(s_scn_st[0].active);
}

void test_scn_two_while_rules_independent(void)
{
    /* OR is two rules: each arms and undoes on its own conditions */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[" CHARGE_OBJ ","
        "{\"name\":\"fast\",\"on\":\"autopid.param\",\"match\":{\"param\":\"SPEED\"},"
        "\"when\":[{\"key\":\"value\",\"op\":\">\",\"val\":50}],"
        "\"do\":\"autopid.group\",\"with\":{\"group\":\"driving\",\"enabled\":true},"
        "\"undo\":true}]"));

    em_event_t charge_on = ev_param("CHARGING", 1);
    em_event_t charge_off = ev_param("CHARGING", 0);
    em_event_t fast = ev_param("SPEED", 80);
    em_event_t slow = ev_param("SPEED", 10);

    feed(&charge_on, 1);
    feed(&fast, 2);
    TEST_ASSERT_TRUE(s_scn_st[0].active);
    TEST_ASSERT_TRUE(s_scn_st[1].active);
    feed(&slow, 3);                     /* only the speed rule undoes     */
    TEST_ASSERT_EQUAL(1, s_undos[1]);
    TEST_ASSERT_EQUAL(0, s_undos[0]);
    TEST_ASSERT_TRUE(s_scn_st[0].active);
    feed(&charge_off, 4);
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    TEST_ASSERT_EQUAL(1, s_runs[1]);
}

void test_scn_disabled_rule_is_inert(void)
{
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse("[" WIFI_RULE "]"));
    s_rules[0].enabled = false;

    em_event_t up = ev_wifi(true, "HomeAP");

    feed(&up, 1);
    tick();
    TEST_ASSERT_EQUAL(0, s_runs[0]);
    TEST_ASSERT_EQUAL(0, s_undos[0]);
    TEST_ASSERT_FALSE(s_scn_st[0].active);
}

void test_scn_timer_with_epoch_deadline(void)
{
    /* "poll faster until 12:00": ticks arm it while the live epoch is
       below the deadline; the re-check undoes it once the clock passes */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"until\",\"on\":\"timer.tick\",\"match\":{\"timer\":\"t\"},"
        "\"when\":[{\"value\":\"${time.epoch}\",\"op\":\"<\",\"val\":2000}],"
        "\"do\":\"autopid.group\",\"with\":{\"group\":\"default\",\"enabled\":true,"
        "\"period_ms\":500},\"undo\":true}]"));

    em_event_t t = ev_tick("t");
    em_event_t other = ev_tick("other");

    s_epoch = "1500";
    feed(&other, 1);                    /* another timer: not ours        */
    TEST_ASSERT_EQUAL(0, s_runs[0]);
    feed(&t, 2);
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&t, 3);                        /* still holding: nothing         */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    s_epoch = "2000";
    tick();                             /* deadline reached: undo         */
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    feed(&t, 4);                        /* later ticks: condition false   */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    TEST_ASSERT_EQUAL(1, s_undos[0]);
    TEST_ASSERT_FALSE(s_scn_st[0].active);
}

void test_scn_changed_with_cooldown(void)
{
    /* a plain "on change" alert with a 60 s cooldown: fires on the first
       change, holds the next ones, fires again after the window */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"alert\",\"on\":\"autopid.param\",\"match\":{\"param\":\"SOC\"},"
        "\"when\":[{\"key\":\"value\",\"op\":\"changed\"}],\"do\":\"mqtt.publish\","
        "\"with\":{\"topic\":\"~/alerts\",\"payload\":\"${value}\"},"
        "\"cooldown_ms\":60000}]"));

    em_event_t a = ev_param("SOC", 50);
    em_event_t b = ev_param("SOC", 49);
    em_event_t c = ev_param("SOC", 48);
    int64_t s = 1000000;

    feed(&a, 1 * s);                    /* first value counts as changed  */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&a, 2 * s);                    /* same value: not changed        */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    TEST_ASSERT_EQUAL(0, s_supp);
    feed(&b, 3 * s);                    /* changed, inside the cooldown   */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    TEST_ASSERT_EQUAL(1, s_supp);
    feed(&c, 62 * s);                   /* changed, cooldown over         */
    TEST_ASSERT_EQUAL(2, s_runs[0]);
}

void test_scn_mixed_conditions(void)
{
    /* trigger field + two live values + a string op in one AND list */
    scn_reset();
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"mix\",\"on\":\"autopid.param\",\"match\":{\"param\":\"SPEED\"},"
        "\"when\":[{\"key\":\"value\",\"op\":\">=\",\"val\":30},"
        "{\"value\":\"${autopid.SOC}\",\"op\":\"!=\",\"val\":0},"
        "{\"value\":\"${wifi.ssid}\",\"op\":\"contains\",\"val\":\"Home\"}],"
        "\"do\":\"log.note\"}]"));

    em_event_t fast = ev_param("SPEED", 30);
    em_event_t slow = ev_param("SPEED", 29);

    feed(&fast, 1);
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    feed(&slow, 2);                     /* value below                    */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    s_soc = "0";
    feed(&fast, 3);                     /* SOC is 0                       */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    s_soc = "25";
    s_ssid = "Cafe";
    feed(&fast, 4);                     /* not at home                    */
    TEST_ASSERT_EQUAL(1, s_runs[0]);
    s_ssid = "HomeAP";
    feed(&fast, 5);
    TEST_ASSERT_EQUAL(2, s_runs[0]);
}

void test_scn_action_template_reads_live_values(void)
{
    /* what the fired action gets: the trigger's fields and live values
       rendered into `with` (the dispatcher's render_with over each leaf) */
    char out[128];
    em_event_t ev = ev_param("CHARGING", 1);

    scn_reset();
    s_soc = "25";
    s_ssid = "HomeAP";
    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "{\"${param}\":${value},\"soc\":${autopid.SOC},\"net\":\"${wifi.ssid}\"}",
        &ev, scn_resolve, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"CHARGING\":1,\"soc\":25,\"net\":\"HomeAP\"}", out);
    TEST_ASSERT_EQUAL(ESP_OK, em_template_render("${no.such}", &ev, scn_resolve,
                                                 out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("null", out);
}

void test_rules_parse_happy(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"high_rpm\",\"on\":\"autopid.param\","
        "\"match\":{\"param\":\"rpm\"},"
        "\"when\":[{\"key\":\"value\",\"op\":\">\",\"val\":3000}],"
        "\"do\":\"log.note\",\"with\":{\"message\":\"rpm ${value}\"},"
        "\"cooldown_ms\":60000}]"));
    TEST_ASSERT_EQUAL(1, s_count);
    TEST_ASSERT_TRUE(s_rules[0].enabled);
    TEST_ASSERT_EQUAL(1, s_rules[0].n_match);
    TEST_ASSERT_EQUAL(1, s_rules[0].n_when);
    TEST_ASSERT_EQUAL(EM_OP_GT, s_rules[0].when[0].op);
    TEST_ASSERT_EQUAL(60000, s_rules[0].cooldown_ms);
    TEST_ASSERT_TRUE(strstr(s_rules[0].with_json, "${value}") != NULL);
}

void test_rules_parse_rejects(void)
{
    /* missing dot in selector */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"autopid\",\"do\":\"log.note\"}]"));
    /* unknown op */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"key\":\"v\",\"op\":\"~=\",\"val\":1}]}]"));
    /* ordered op with a string operand */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"when\":[{\"key\":\"v\",\"op\":\">\",\"val\":\"hi\"}]}]"));
    /* duplicate names */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\"},"
        "{\"name\":\"x\",\"on\":\"a.b\",\"do\":\"log.note\"}]"));
    /* empty array + null are fine */
    TEST_ASSERT_EQUAL(ESP_OK, parse("[]"));
    TEST_ASSERT_EQUAL(0, s_count);
}

void test_rules_parse_script_sugar(void)
{
    /* {"script":"name"} rewrites to script.run + {"name":...} */
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"diag\",\"on\":\"uds.response\","
        "\"script\":\"vin_check\"}]"));
    TEST_ASSERT_EQUAL(1, s_count);
    TEST_ASSERT_EQUAL_STRING("script.run", s_rules[0].action);
    TEST_ASSERT_EQUAL_STRING("{\"name\":\"vin_check\"}",
                             s_rules[0].with_json);

    /* script + do are exclusive */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"script\":\"s\","
        "\"do\":\"log.note\"}]"));
    /* script takes no with */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\",\"script\":\"s\","
        "\"with\":{\"k\":1}}]"));
    /* quote/backslash injection rejected */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\","
        "\"script\":\"a\\\"b\"}]"));
    /* neither script nor do */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(
        "[{\"name\":\"x\",\"on\":\"a.b\"}]"));
}

/* ---- match + when --------------------------------------------------------------- */

void test_match_and_ops(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"autopid.param\","
        "\"match\":{\"param\":\"rpm\"},"
        "\"when\":[{\"key\":\"value\",\"op\":\">\",\"val\":3000}],"
        "\"do\":\"log.note\"}]"));

    em_rule_state_t st = { 0 };
    em_event_t hi = ev_param("rpm", 4000);
    em_event_t lo = ev_param("rpm", 1000);
    em_event_t other = ev_param("speed", 9000);

    TEST_ASSERT_TRUE(em_rule_match(&s_rules[0], &hi));
    TEST_ASSERT_FALSE(em_rule_match(&s_rules[0], &other));
    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &hi, &st));
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &lo, &st));

    /* every comparison op over the same event */
    struct { const char *op; double val; bool expect; } cases[] =
    {
        { "==", 4000, true },  { "==", 1, false },
        { "!=", 1, true },     { "!=", 4000, false },
        { ">=", 4000, true },  { "<", 4000, false },
        { "<=", 4000, true },  { ">", 4000, false },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char json[220];

        snprintf(json, sizeof(json),
                 "[{\"name\":\"r\",\"on\":\"autopid.param\","
                 "\"when\":[{\"key\":\"value\",\"op\":\"%s\","
                 "\"val\":%g}],\"do\":\"log.note\"}]",
                 cases[i].op, cases[i].val);
        TEST_ASSERT_EQUAL(ESP_OK, parse(json));

        em_rule_state_t s2 = { 0 };

        TEST_ASSERT_EQUAL_MESSAGE(cases[i].expect,
                                  em_rule_when(&s_rules[0], &hi, &s2),
                                  cases[i].op);
    }
}

void test_contains_and_string_eq(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"mqtt.rx\","
        "\"when\":[{\"key\":\"payload\",\"op\":\"contains\","
        "\"val\":\"launch\"}],\"do\":\"log.note\"}]"));

    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "mqtt");
    snprintf(ev.name, sizeof(ev.name), "rx");
    ev.kv[0] = (em_kv_t){ .key = "payload", .type = EM_VAL_STR };
    snprintf(ev.kv[0].v.str, EM_STR_MAX, "please launch now");
    ev.n = 1;

    em_rule_state_t st = { 0 };

    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &ev, &st));
    snprintf(ev.kv[0].v.str, EM_STR_MAX, "nothing here");
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &ev, &st));
}

void test_changed_semantics(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"autopid.param\","
        "\"when\":[{\"key\":\"value\",\"op\":\"changed\"}],"
        "\"do\":\"log.note\"}]"));

    em_rule_state_t st = { 0 };
    em_event_t a = ev_param("rpm", 100);
    em_event_t b = ev_param("rpm", 200);

    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &a, &st));  /* first    */
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &a, &st)); /* same     */
    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &b, &st));  /* changed  */
    TEST_ASSERT_FALSE(em_rule_when(&s_rules[0], &b, &st));
    TEST_ASSERT_TRUE(em_rule_when(&s_rules[0], &a, &st));  /* back     */
}

void test_cooldown_arithmetic(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse(
        "[{\"name\":\"r\",\"on\":\"a.b\",\"do\":\"log.note\","
        "\"cooldown_ms\":1000}]"));

    em_rule_state_t st = { 0 };

    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &st, 5000000));
    TEST_ASSERT_FALSE(em_rule_cooldown_ok(&s_rules[0], &st, 5500000));
    TEST_ASSERT_FALSE(em_rule_cooldown_ok(&s_rules[0], &st, 5999999));
    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &st, 6000001));

    /* no cooldown = always allowed */
    s_rules[0].cooldown_ms = 0;

    em_rule_state_t s2 = { 0 };

    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &s2, 1));
    TEST_ASSERT_TRUE(em_rule_cooldown_ok(&s_rules[0], &s2, 2));
}

/* ---- templates -------------------------------------------------------------------- */

static esp_err_t fake_resolver(const char *name, char *out, size_t len)
{
    if (strcmp(name, "battery.voltage") == 0)
    {
        snprintf(out, len, "12.43");
        return ESP_OK;
    }

    if (strcmp(name, "autopid.data") == 0)
    {
        snprintf(out, len, "{\"rpm\":5324}");
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

void test_template_render(void)
{
    em_event_t ev = ev_param("rpm", 4000.5);
    char out[256];

    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "{\"p\":\"${param}\",\"v\":${value},\"at\":${ts}}", &ev,
        fake_resolver, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "{\"p\":\"rpm\",\"v\":4000.5,\"at\":123456789}", out);

    /* pull values + missing key -> null */
    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "batt=${battery.voltage} data=${autopid.data} x=${nope}", &ev,
        fake_resolver, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(
        "batt=12.43 data={\"rpm\":5324} x=null", out);

    /* no templates = verbatim; ${ without } = literal */
    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "plain ${unclosed", &ev, fake_resolver, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("plain ${unclosed", out);

    /* overflow detected */
    char tiny[8];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, em_template_render(
        "0123456789", &ev, NULL, tiny, sizeof(tiny)));
}

void test_template_types(void)
{
    em_event_t ev = { 0 };

    snprintf(ev.source, sizeof(ev.source), "s");
    snprintf(ev.name, sizeof(ev.name), "n");
    ev.kv[0] = (em_kv_t){ .key = "b", .type = EM_VAL_BOOL };
    ev.kv[0].v.b = true;
    ev.kv[1] = (em_kv_t){ .key = "i", .type = EM_VAL_I64 };
    ev.kv[1].v.i64 = -42;
    ev.n = 2;

    char out[64];

    TEST_ASSERT_EQUAL(ESP_OK, em_template_render(
        "${b}/${i}/${source}.${name}", &ev, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("true/-42/s.n", out);
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_rules_parse_happy);
    RUN_TEST(test_rules_parse_rejects);
    RUN_TEST(test_rules_parse_script_sugar);
    RUN_TEST(test_match_and_ops);
    RUN_TEST(test_contains_and_string_eq);
    RUN_TEST(test_changed_semantics);
    RUN_TEST(test_cooldown_arithmetic);
    RUN_TEST(test_template_render);
    RUN_TEST(test_template_types);
    RUN_TEST(test_parse_live_value_and_undo);
    RUN_TEST(test_live_value_conditions);
    RUN_TEST(test_live_recheck);
    RUN_TEST(test_while_rule_step);
    RUN_TEST(test_while_rule_recheck);
    RUN_TEST(test_validate_undo);
    RUN_TEST(test_live_value_kinds);
    RUN_TEST(test_scn_wifi_while_rule);
    RUN_TEST(test_scn_live_value_drift);
    RUN_TEST(test_scn_plain_and_while_share_a_trigger);
    RUN_TEST(test_scn_match_isolates_other_parameters);
    RUN_TEST(test_scn_state_in_match_never_undoes);
    RUN_TEST(test_scn_cooldown_on_a_while_rule);
    RUN_TEST(test_scn_two_while_rules_independent);
    RUN_TEST(test_scn_disabled_rule_is_inert);
    RUN_TEST(test_scn_timer_with_epoch_deadline);
    RUN_TEST(test_scn_changed_with_cooldown);
    RUN_TEST(test_scn_mixed_conditions);
    RUN_TEST(test_scn_action_template_reads_live_values);

    UNITY_END();
}
