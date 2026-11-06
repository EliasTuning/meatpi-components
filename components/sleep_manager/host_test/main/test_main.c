/**
 * @file test_main.c
 * @brief Host tests for the pure sleep policy ladder: countdown +
 *        recovery, sleep entry, stable-wake reboot, band dips,
 *        periodic check-in gating, ms-clock wrap, the wake hold following
 *        wake_delay_ms; and the settings rules (v3 threshold resolver,
 *        v2 -> v3 and v3 -> v4 migration values).
 *        Plus the critical battery floor tracker (2026-10-01) and the
 *        countdown the surfaces report (2026-10-06): which rule sleeps
 *        the device first, the sleep delay or the floor, and in how
 *        many seconds, through the one word a status read unpacks; and
 *        the hold (same day): ten more minutes on request, on both
 *        deadlines.
 *        Expected output: 23 Tests 0 Failures 0 Ignored.
 */
#include "unity.h"

#include "sleep_manager_private.h"

/* the legacy pair: sleep 13.1 V, wake 13.2 V, 1 s hold, 3 min delay */
static sm_cfg_t s_cfg;
static sm_policy_t s_p;

void setUp(void)
{
    s_cfg.sleep_v = 13.1f;
    s_cfg.wake_v = 13.2f;
    s_cfg.wake_hold_ms = 1000;
    s_cfg.delay_ms = 180000;
    s_cfg.periodic = false;
    s_cfg.interval_ms = 1800000;
    sm_policy_init(&s_p);
}

void tearDown(void)
{
}

static void test_healthy_battery_stays_normal(void)
{
    for (uint32_t t = 0; t < 600000; t += 1000)
    {
        TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                              sm_policy_eval(&s_p, &s_cfg, 14.2f, t));
        TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_NORMAL, s_p.state);
    }
}

static void test_countdown_then_sleep(void)
{
    /* engine off: 12.4 V, countdown starts */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 1000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    /* still counting at +179 s */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 180000));
    /* expiry → sleep */
    TEST_ASSERT_EQUAL_INT(SM_ACT_ENTER_SLEEP,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 181001));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
}

static void test_recovery_cancels_countdown(void)
{
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 1000);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    /* alternator back: above wake_v cancels */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 13.8f, 60000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_NORMAL, s_p.state);
    /* the 13.1..13.2 band does NOT cancel (hysteresis) */
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 61000);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 13.15f, 62000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
}

static void test_stable_recovery_reboots(void)
{
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
    /* engine started */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_WAKE_PENDING, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_WAKE_REBOOT,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 201500));
}

static void test_wake_dip_goes_back_to_sleep(void)
{
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000); /* WAKE_PENDING */
    /* headlights flash / spike gone: dip before the hold expires */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.5f, 200500));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
}

static void test_periodic_wake_gated_by_critical(void)
{
    s_cfg.periodic = true;
    s_cfg.interval_ms = 60000;
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001); /* SLEEPING, t+60s */
    /* healthy-ish battery: periodic fires after the interval */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 239000));
    TEST_ASSERT_EQUAL_INT(SM_ACT_PERIODIC_WAKE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f, 241000));
    /* below the critical floor it must NEVER fire (don't kill the
       battery for a check-in) */
    sm_policy_init(&s_p);
    sm_policy_eval(&s_p, &s_cfg, 11.5f, 0);
    sm_policy_eval(&s_p, &s_cfg, 11.5f, 180001);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 11.5f, 900000));
}

static void test_clock_wrap_safe(void)
{
    /* countdown straddling the u32 ms wrap */
    uint32_t t0 = 0xFFFFFFFFu - 60000u;

    sm_policy_eval(&s_p, &s_cfg, 12.4f, t0);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f,
                                         t0 + 100000u)); /* wrapped */
    TEST_ASSERT_EQUAL_INT(SM_ACT_ENTER_SLEEP,
                          sm_policy_eval(&s_p, &s_cfg, 12.4f,
                                         t0 + 181000u));
}

/* v3: the stored pair resolves as given when the band is >= 0.1 V and is
   pulled up to sleep + 0.1 V (reported) when it is not */
static void test_resolve_thresholds(void)
{
    float s, w;

    TEST_ASSERT_FALSE(sm_resolve_thresholds(13100, 13300, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.1f, s);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.3f, w);
    /* exactly the minimum band is fine (the migrated default pair) */
    TEST_ASSERT_FALSE(sm_resolve_thresholds(13100, 13200, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.2f, w);
    /* equal: clamped */
    TEST_ASSERT_TRUE(sm_resolve_thresholds(12900, 12900, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 12.9f, s);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.0f, w);
    /* below sleep: clamped, sleep untouched */
    TEST_ASSERT_TRUE(sm_resolve_thresholds(14000, 12100, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 14.0f, s);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 14.1f, w);
    /* a wide band is the user's choice */
    TEST_ASSERT_FALSE(sm_resolve_thresholds(12500, 13800, &s, &w));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.8f, w);
}

/* v4: the wake hold follows the setting: a 5 s hold waits the full 5 s
   (and dips still cancel), a 100 ms hold wakes almost at once */
static void test_wake_hold_follows_setting(void)
{
    s_cfg.wake_hold_ms = 5000;
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000));
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_WAKE_PENDING, s_p.state);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 204900));
    TEST_ASSERT_EQUAL_INT(SM_ACT_WAKE_REBOOT,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 205000));

    s_cfg.wake_hold_ms = 100;
    sm_policy_init(&s_p);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 180001);
    sm_policy_eval(&s_p, &s_cfg, 14.1f, 200000);
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200050));
    TEST_ASSERT_EQUAL_INT(SM_ACT_WAKE_REBOOT,
                          sm_policy_eval(&s_p, &s_cfg, 14.1f, 200100));
}

/* v3 -> v4: a document without wake_delay_ms gets the default; one that
   has it, or a current document, is left alone */
static void test_migrated_wake_delay_ms(void)
{
    TEST_ASSERT_EQUAL_INT(500, sm_migrated_wake_delay_ms(3, false));
    TEST_ASSERT_EQUAL_INT(500, sm_migrated_wake_delay_ms(1, false));
    TEST_ASSERT_EQUAL_INT(0, sm_migrated_wake_delay_ms(3, true));
    TEST_ASSERT_EQUAL_INT(0, sm_migrated_wake_delay_ms(4, false));
}

/* v2 -> v3: every historical document gets the band it had (sleep + 100),
   kept inside the new field's range */
static void test_migrated_wake_mv(void)
{
    TEST_ASSERT_EQUAL_INT(13200, sm_migrated_wake_mv(13100, 12100, 15000));
    TEST_ASSERT_EQUAL_INT(12100, sm_migrated_wake_mv(12000, 12100, 15000));
    TEST_ASSERT_EQUAL_INT(14100, sm_migrated_wake_mv(14000, 12100, 15000));
    /* a v1 document is clamped to the v2 ranges first; the rule still
       stays inside the field's range if it were not */
    TEST_ASSERT_EQUAL_INT(12100, sm_migrated_wake_mv(11000, 12100, 15000));
    TEST_ASSERT_EQUAL_INT(15000, sm_migrated_wake_mv(15500, 12100, 15000));
}

/* the critical floor: 5 min under 11.90 V trips once, then re-arms. The
   delay is pinned here: it was 2 min until 2026-10-06 (Ali made it the
   default sleep delay), and a trip at 120 s would be the old rule back */
static void test_critical_floor_trips_after_delay(void)
{
    sm_critical_t c;

    TEST_ASSERT_EQUAL_UINT32(300000u, SM_CRITICAL_DELAY_MS);
    sm_critical_init(&c);
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 0));         /* arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 60000));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 120000));    /* not at 2 min */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 299000));
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.7f, 300000));     /* trip */
    /* one trip per arming; the next reading under the floor re-arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 301000));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 600000));
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.7f, 601000));
    /* a healthy battery never arms it */
    sm_critical_init(&c);
    for (uint32_t t = 0; t < 900000; t += 1000)
    {
        TEST_ASSERT_FALSE(sm_critical_eval(&c, 12.4f, t));
    }
}

/* recovery above the hysteresis band resets the timer; the band itself
   neither arms nor resets it */
static void test_critical_floor_resets_on_recovery(void)
{
    sm_critical_t c;

    sm_critical_init(&c);
    sm_critical_eval(&c, 11.7f, 0);                            /* arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 150000));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 12.0f, 151000));    /* reset */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 152000));    /* re-arm */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.7f, 451000));    /* 299 s */
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.7f, 452000));     /* 300 s */
    /* 11.92 V sits in the band: keeps an armed timer, arms nothing */
    sm_critical_init(&c);
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.92f, 0));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.92f, 400000));   /* never armed */
    sm_critical_eval(&c, 11.8f, 500000);                       /* arms */
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.92f, 700000));   /* holds */
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.92f, 800000));    /* trips */
}

/* ---- the countdown (2026-10-06): which rule sleeps the device first ---- */

/* the whole path of a status read: the task evaluates and publishes one
   word at `now`, a reader unpacks it at the same instant */
static void expect_pending(sleep_manager_pending_t cause, uint32_t secs,
                           bool ladder_on, const sm_critical_t *c,
                           uint32_t now)
{
    uint32_t deadline = now;
    uint32_t got = 12345;
    sleep_manager_pending_t evald =
        sm_pending_eval(&s_p, ladder_on, c, now, &deadline);

    TEST_ASSERT_EQUAL_INT(cause, evald);
    TEST_ASSERT_EQUAL_INT(cause,
                          sm_pending_unpack(sm_pending_pack(evald, deadline),
                                            now, &got));
    TEST_ASSERT_EQUAL_UINT32(secs, got);
}

/* nothing counts on a healthy battery, nor once the device is asleep */
static void test_pending_none_when_healthy_or_asleep(void)
{
    sm_critical_t c;

    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 14.2f, 1000);
    sm_critical_eval(&c, 14.2f, 1000);
    expect_pending(SLEEP_MANAGER_PENDING_NONE, 0, true, &c, 1000);

    sm_policy_eval(&s_p, &s_cfg, 11.5f, 2000);  /* LOW_VOLTAGE */
    sm_critical_eval(&c, 11.5f, 2000);          /* armed       */
    TEST_ASSERT_EQUAL_INT(SM_ACT_ENTER_SLEEP,
                          sm_policy_eval(&s_p, &s_cfg, 11.5f, 182000));
    expect_pending(SLEEP_MANAGER_PENDING_NONE, 0, true, &c, 182000);
    sm_policy_eval(&s_p, &s_cfg, 14.1f, 190000); /* WAKE_PENDING */
    expect_pending(SLEEP_MANAGER_PENDING_NONE, 0, true, &c, 190000);
}

/* above the floor the countdown is the sleep delay, in whole seconds
   rounded up, never negative, gone when the battery recovers and absent
   while the ladder does not run (sleep disabled) */
static void test_pending_counts_the_sleep_delay(void)
{
    sm_critical_t c;

    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 1000);  /* deadline 181000 */
    sm_critical_eval(&c, 12.4f, 1000);          /* not armed       */
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 180, true, &c, 1000);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 180, true, &c, 1001);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 120, true, &c, 61000);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 1, true, &c, 180500);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 0, true, &c, 181000);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 0, true, &c, 185000);
    expect_pending(SLEEP_MANAGER_PENDING_NONE, 0, false, &c, 61000);

    sm_policy_init(&s_p);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);
    sm_policy_eval(&s_p, &s_cfg, 13.8f, 60000); /* alternator back */
    expect_pending(SLEEP_MANAGER_PENDING_NONE, 0, true, &c, 60000);
}

/* a sleep delay longer than the floor's 5 min (10 min here): the floor
   comes first and the countdown names it; when the battery climbs back
   over the floor but stays under the sleep voltage, the sleep delay is
   what is left, counted from its own start */
static void test_pending_floor_overtakes_a_longer_delay(void)
{
    sm_critical_t c;

    s_cfg.delay_ms = 600000;
    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 11.55f, 0);
    sm_critical_eval(&c, 11.55f, 0);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 300, true, &c, 0);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 60, true, &c, 240000);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 0, true, &c, 300000);

    sm_policy_eval(&s_p, &s_cfg, 12.4f, 250000);
    sm_critical_eval(&c, 12.4f, 250000);        /* over the floor: reset */
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 350, true, &c, 250000);
}

/* a sleep delay shorter than the floor's comes first. The report of
   2026-10-06 was a DEFAULT device (5 min delay) on an 11.6 V supply
   asleep after the floor's 2 min of the time; with the floor at 5 min it
   has its 5 minutes: the two deadlines tie, and a tie goes to the floor
   (the task evaluates it first). A supply that passes 12.5 V on its way
   down arms the ladder a sample earlier: the delay is the earlier one */
static void test_pending_shorter_delay_beats_the_floor(void)
{
    sm_critical_t c;

    s_cfg.delay_ms = 60000;
    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 11.55f, 0);
    sm_critical_eval(&c, 11.55f, 0);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 60, true, &c, 0);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 15, true, &c, 45000);

    s_cfg.delay_ms = 300000;
    sm_policy_init(&s_p);
    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 11.55f, 0);
    sm_critical_eval(&c, 11.55f, 0);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 300, true, &c, 0);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 180, true, &c, 120000);

    sm_policy_init(&s_p);
    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 12.5f, 0);     /* on the way down */
    sm_critical_eval(&c, 12.5f, 0);
    sm_policy_eval(&s_p, &s_cfg, 11.55f, 3000);
    sm_critical_eval(&c, 11.55f, 3000);
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 297, true, &c, 3000);
}

/* sleep disabled: the ladder never leaves NORMAL and the floor is the
   only countdown; the hysteresis band holds it, recovery clears it */
static void test_pending_floor_alone_with_sleep_disabled(void)
{
    sm_critical_t c;

    sm_critical_init(&c);
    expect_pending(SLEEP_MANAGER_PENDING_NONE, 0, false, &c, 4000);
    sm_critical_eval(&c, 11.7f, 5000);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 300, false, &c, 5000);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 1, false, &c, 304001);
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.92f, 60000));    /* holds */
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 245, false, &c, 60000);
    sm_critical_eval(&c, 12.0f, 61000);                        /* reset */
    expect_pending(SLEEP_MANAGER_PENDING_NONE, 0, false, &c, 61000);
}

/* both deadlines straddling the u32 ms wrap (a 10 min delay, so that the
   floor is the earlier one) */
static void test_pending_clock_wrap_safe(void)
{
    sm_critical_t c;
    uint32_t t0 = 0xFFFFFFFFu - 60000u;

    s_cfg.delay_ms = 600000;
    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 11.7f, t0);
    sm_critical_eval(&c, 11.7f, t0);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 300, true, &c, t0);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 200, true, &c,
                   t0 + 100000u);               /* wrapped */
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 0, true, &c,
                   t0 + 300000u);
    sm_critical_init(&c);                       /* the ladder alone */
    expect_pending(SLEEP_MANAGER_PENDING_DELAY, 500, true, &c,
                   t0 + 100000u);
}

/* ---- the hold (Ali, 2026-10-06): ten more minutes on request ------------ */

/* a hold moves both deadlines out to now + hold where that is later: a
   default device at 11.6 V (both at 5 min) held at 4:00 left sleeps 10 min
   after the press; the countdown follows; the states do not change */
static void test_hold_moves_both_deadlines(void)
{
    sm_critical_t c;

    s_cfg.delay_ms = 300000;
    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 11.55f, 0);
    sm_critical_eval(&c, 11.55f, 0);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 240, true, &c, 60000);

    TEST_ASSERT_TRUE(sm_hold_apply(&s_p, true, &c, 60000, 600000));
    TEST_ASSERT_EQUAL_UINT32(660000, s_p.t_low);
    TEST_ASSERT_EQUAL_UINT32(660000, c.t_trip);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    TEST_ASSERT_TRUE(c.armed);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 600, true, &c, 60000);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 1, true, &c, 659001);

    /* the ladder keeps its rules: nothing fires before the new deadline,
       the entry comes at it, a recovery still cancels */
    TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                          sm_policy_eval(&s_p, &s_cfg, 11.55f, 300000));
    TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.55f, 300000));
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.55f, 660000));
    TEST_ASSERT_EQUAL_INT(SM_ACT_ENTER_SLEEP,
                          sm_policy_eval(&s_p, &s_cfg, 11.55f, 660000));
}

/* a hold shorter than what is left changes nothing; the ladder alone when
   the floor is not armed; the floor alone with sleep disabled; nothing to
   hold on a healthy battery or once asleep */
static void test_hold_only_what_counts(void)
{
    sm_critical_t c;

    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 0);      /* delay 180 s, floor idle */
    TEST_ASSERT_TRUE(sm_hold_apply(&s_p, true, &c, 1000, 60000));
    TEST_ASSERT_EQUAL_UINT32(180000, s_p.t_low); /* 61 s < 180 s: unchanged */
    TEST_ASSERT_FALSE(c.armed);
    TEST_ASSERT_TRUE(sm_hold_apply(&s_p, true, &c, 150000, 60000));
    TEST_ASSERT_EQUAL_UINT32(210000, s_p.t_low);

    sm_policy_init(&s_p);                         /* sleep disabled */
    sm_critical_init(&c);
    sm_critical_eval(&c, 11.7f, 0);
    TEST_ASSERT_TRUE(sm_hold_apply(&s_p, false, &c, 100000, 600000));
    TEST_ASSERT_EQUAL_UINT32(700000, c.t_trip);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_NORMAL, s_p.state);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 600, false, &c, 100000);

    sm_policy_init(&s_p);                         /* healthy: nothing */
    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 14.0f, 0);
    TEST_ASSERT_FALSE(sm_hold_apply(&s_p, true, &c, 1000, 600000));

    sm_policy_eval(&s_p, &s_cfg, 12.4f, 2000);    /* asleep: nothing */
    sm_policy_eval(&s_p, &s_cfg, 12.4f, 182000);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_SLEEPING, s_p.state);
    TEST_ASSERT_FALSE(sm_hold_apply(&s_p, true, &c, 183000, 600000));
}

/* across the clock wrap the later deadline is still the later one */
static void test_hold_clock_wrap_safe(void)
{
    sm_critical_t c;
    uint32_t t0 = 0xFFFFFFFFu - 30000u;

    sm_critical_init(&c);
    sm_policy_eval(&s_p, &s_cfg, 11.7f, t0);      /* t_low = t0 + 180 s */
    sm_critical_eval(&c, 11.7f, t0);              /* t_trip = t0 + 300 s */
    TEST_ASSERT_TRUE(sm_hold_apply(&s_p, true, &c, t0 + 20000u, 600000));
    TEST_ASSERT_EQUAL_UINT32(t0 + 620000u, s_p.t_low);
    TEST_ASSERT_EQUAL_UINT32(t0 + 620000u, c.t_trip);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 600, true, &c, t0 + 20000u);
}

/* the published word is read later than it was written: the seconds are
   counted at the read, exact however old the word is; a passed deadline
   reads 0, across the 30-bit field's wrap and the clock's too */
static void test_pending_word_is_exact_at_the_read(void)
{
    uint32_t s = 12345;
    uint32_t w = sm_pending_pack(SLEEP_MANAGER_PENDING_DELAY, 181000);

    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_PENDING_DELAY,
                          sm_pending_unpack(w, 1000, &s));
    TEST_ASSERT_EQUAL_UINT32(180, s);
    sm_pending_unpack(w, 61400, &s);            /* 119.6 s left */
    TEST_ASSERT_EQUAL_UINT32(120, s);
    sm_pending_unpack(w, 180999, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s);
    sm_pending_unpack(w, 181000, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_PENDING_DELAY,
                          sm_pending_unpack(w, 9000000, &s)); /* long past */
    TEST_ASSERT_EQUAL_UINT32(0, s);

    /* the deadline sits just over the 30-bit boundary, the reader under it */
    w = sm_pending_pack(SLEEP_MANAGER_PENDING_CRITICAL, 0x40000000u + 5000u);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_PENDING_CRITICAL,
                          sm_pending_unpack(w, 0x40000000u - 115000u, &s));
    TEST_ASSERT_EQUAL_UINT32(120, s);
    sm_pending_unpack(w, 0x40000000u + 5001u, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s);

    /* and over the clock's own wrap */
    w = sm_pending_pack(SLEEP_MANAGER_PENDING_CRITICAL, 20000u);
    sm_pending_unpack(w, 0xFFFFFFFFu - 9999u, &s);  /* 30 s before */
    TEST_ASSERT_EQUAL_UINT32(30, s);

    /* nothing counts: 0 whatever the low bits hold */
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_PENDING_NONE,
                          sm_pending_unpack(
                              sm_pending_pack(SLEEP_MANAGER_PENDING_NONE,
                                              500000), 1000, &s));
    TEST_ASSERT_EQUAL_UINT32(0, s);
}

/* a log read pushes both counting deadlines out a minute from the read,
   read after read, and never past ten minutes from the first read of the
   countdown; the states do not change */
static void test_activity_holds_within_the_cap(void)
{
    sm_critical_t c;
    sm_activity_t a;

    s_cfg.delay_ms = 300000;
    sm_critical_init(&c);
    sm_activity_init(&a);
    sm_policy_eval(&s_p, &s_cfg, 11.55f, 0);     /* both at 5 min */
    sm_critical_eval(&c, 11.55f, 0);

    /* a read 1 min in: the deadlines are 4 min away, 1 min is not later */
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, true, &c, &a, 60000, 60000,
                                       600000));
    TEST_ASSERT_TRUE(a.active);
    TEST_ASSERT_EQUAL_UINT32(660000, a.cap_until);  /* 1 min + 10 min */
    TEST_ASSERT_EQUAL_UINT32(300000, s_p.t_low);
    TEST_ASSERT_EQUAL_UINT32(300000, c.t_trip);

    /* a read 10 s before the entry: both move out to a minute from it */
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, true, &c, &a, 290000, 60000,
                                       600000));
    TEST_ASSERT_EQUAL_UINT32(350000, s_p.t_low);
    TEST_ASSERT_EQUAL_UINT32(350000, c.t_trip);
    TEST_ASSERT_EQUAL_UINT32(350000, a.until);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_LOW_VOLTAGE, s_p.state);
    TEST_ASSERT_TRUE(c.armed);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 60, true, &c, 290000);

    /* reads every second keep it awake ... */
    for (uint32_t t = 291000; t < 640000; t += 1000)
    {
        TEST_ASSERT_TRUE(sm_activity_apply(&s_p, true, &c, &a, t, 60000,
                                           600000));
        TEST_ASSERT_EQUAL_INT(SM_ACT_NONE,
                              sm_policy_eval(&s_p, &s_cfg, 11.55f, t));
        TEST_ASSERT_FALSE(sm_critical_eval(&c, 11.55f, t));
    }

    /* ... but never past the cap: 10 min after the first read */
    TEST_ASSERT_EQUAL_UINT32(660000, s_p.t_low);
    TEST_ASSERT_EQUAL_UINT32(660000, c.t_trip);
    TEST_ASSERT_EQUAL_UINT32(660000, a.until);
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, true, &c, &a, 659000, 60000,
                                       600000));
    TEST_ASSERT_EQUAL_UINT32(660000, c.t_trip);
    TEST_ASSERT_TRUE(sm_critical_eval(&c, 11.55f, 660000));
    TEST_ASSERT_EQUAL_INT(SM_ACT_ENTER_SLEEP,
                          sm_policy_eval(&s_p, &s_cfg, 11.55f, 660000));
    /* asleep: a read holds nothing and the tracker lets go */
    TEST_ASSERT_FALSE(sm_activity_apply(&s_p, true, &c, &a, 661000, 60000,
                                        600000));
    TEST_ASSERT_FALSE(a.active);
}

/* only what counts moves: the floor alone with sleep disabled, the ladder
   alone while the floor is not armed; nothing on a healthy battery; and a
   fresh countdown gets a fresh cap once the tracker was reset */
static void test_activity_only_what_counts(void)
{
    sm_critical_t c;
    sm_activity_t a;

    sm_critical_init(&c);
    sm_activity_init(&a);
    sm_policy_eval(&s_p, &s_cfg, 14.0f, 0);      /* healthy: nothing */
    TEST_ASSERT_FALSE(sm_activity_apply(&s_p, true, &c, &a, 1000, 60000,
                                        600000));
    TEST_ASSERT_FALSE(a.active);

    sm_policy_eval(&s_p, &s_cfg, 12.4f, 2000);   /* delay 180 s, floor idle */
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, true, &c, &a, 170000, 60000,
                                       600000));
    TEST_ASSERT_EQUAL_UINT32(230000, s_p.t_low);
    TEST_ASSERT_FALSE(c.armed);

    sm_policy_init(&s_p);                         /* sleep disabled */
    sm_critical_init(&c);
    sm_activity_init(&a);
    sm_critical_eval(&c, 11.7f, 0);               /* floor at 300 s */
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, false, &c, &a, 280000, 60000,
                                       600000));
    TEST_ASSERT_EQUAL_UINT32(340000, c.t_trip);
    TEST_ASSERT_EQUAL_INT(SLEEP_MANAGER_NORMAL, s_p.state);
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 60, false, &c, 280000);

    /* the cap belongs to one countdown: reset, the next read sets a new one */
    sm_activity_init(&a);
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, false, &c, &a, 300000, 60000,
                                       600000));
    TEST_ASSERT_EQUAL_UINT32(900000, a.cap_until);
    TEST_ASSERT_EQUAL_UINT32(360000, c.t_trip);
}

/* across the clock wrap the cap and the push still compare right */
static void test_activity_clock_wrap_safe(void)
{
    sm_critical_t c;
    sm_activity_t a;
    uint32_t t0 = 0xFFFFFFFFu - 30000u;

    sm_critical_init(&c);
    sm_activity_init(&a);
    sm_policy_eval(&s_p, &s_cfg, 11.7f, t0);      /* t_low = t0 + 180 s */
    sm_critical_eval(&c, 11.7f, t0);              /* t_trip = t0 + 300 s */
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, true, &c, &a, t0 + 290000u,
                                       60000, 600000));
    TEST_ASSERT_EQUAL_UINT32(t0 + 350000u, c.t_trip);
    TEST_ASSERT_EQUAL_UINT32(t0 + 350000u, s_p.t_low);
    TEST_ASSERT_EQUAL_UINT32(t0 + 890000u, a.cap_until);
    TEST_ASSERT_TRUE(sm_activity_apply(&s_p, true, &c, &a, t0 + 880000u,
                                       60000, 600000));
    TEST_ASSERT_EQUAL_UINT32(t0 + 890000u, c.t_trip);   /* capped */
    expect_pending(SLEEP_MANAGER_PENDING_CRITICAL, 10, true, &c,
                   t0 + 880000u);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_healthy_battery_stays_normal);
    RUN_TEST(test_countdown_then_sleep);
    RUN_TEST(test_recovery_cancels_countdown);
    RUN_TEST(test_stable_recovery_reboots);
    RUN_TEST(test_wake_dip_goes_back_to_sleep);
    RUN_TEST(test_periodic_wake_gated_by_critical);
    RUN_TEST(test_clock_wrap_safe);
    RUN_TEST(test_resolve_thresholds);
    RUN_TEST(test_migrated_wake_mv);
    RUN_TEST(test_wake_hold_follows_setting);
    RUN_TEST(test_migrated_wake_delay_ms);
    RUN_TEST(test_critical_floor_trips_after_delay);
    RUN_TEST(test_critical_floor_resets_on_recovery);
    RUN_TEST(test_pending_none_when_healthy_or_asleep);
    RUN_TEST(test_pending_counts_the_sleep_delay);
    RUN_TEST(test_pending_floor_overtakes_a_longer_delay);
    RUN_TEST(test_pending_shorter_delay_beats_the_floor);
    RUN_TEST(test_pending_floor_alone_with_sleep_disabled);
    RUN_TEST(test_pending_clock_wrap_safe);
    RUN_TEST(test_pending_word_is_exact_at_the_read);
    RUN_TEST(test_hold_moves_both_deadlines);
    RUN_TEST(test_hold_only_what_counts);
    RUN_TEST(test_hold_clock_wrap_safe);
    RUN_TEST(test_activity_holds_within_the_cap);
    RUN_TEST(test_activity_only_what_counts);
    RUN_TEST(test_activity_clock_wrap_safe);
    UNITY_END();
}
