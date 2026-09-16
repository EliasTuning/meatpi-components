/**
 * @file test_main.c
 * @brief Unit tests for the pure obd_gate core (og_core_try/force/release):
 *        grant/deny, same-owner extension, hold expiry reaping, wrong-owner
 *        release, force (steal) semantics, and the counters.
 */
#include "unity.h"

#include "obd_gate_private.h"

static const int CHIP;  /* distinct owner identities (addresses only) */
static const int ELM0;
static const int ELM1;

#define HOLD 2000

void test_free_gate_grants_and_holds(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 1000, HOLD));
    TEST_ASSERT_EQUAL_PTR(&CHIP, g.owner);
    TEST_ASSERT_EQUAL(1000 + HOLD, (int)g.deadline_ms);
    TEST_ASSERT_EQUAL_UINT32(1, g.acquires);

    /* someone else is refused while the hold is live */
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM0, 1500, HOLD));
    TEST_ASSERT_EQUAL_PTR(&CHIP, g.owner);
    TEST_ASSERT_EQUAL_UINT32(1, g.acquires);
}

void test_same_owner_reacquire_extends(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &ELM0, 0, HOLD));
    TEST_ASSERT_TRUE(og_core_try(&g, &ELM0, 500, HOLD));
    TEST_ASSERT_EQUAL(500 + HOLD, (int)g.deadline_ms);
    TEST_ASSERT_EQUAL_UINT32(2, g.acquires);
    TEST_ASSERT_EQUAL_UINT32(0, g.expiries);
}

void test_release_frees_for_next_owner(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));
    og_core_release(&g, &CHIP);
    TEST_ASSERT_NULL(g.owner);
    TEST_ASSERT_TRUE(og_core_try(&g, &ELM0, 10, HOLD));
    TEST_ASSERT_EQUAL_PTR(&ELM0, g.owner);
}

void test_wrong_owner_release_is_ignored(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));
    og_core_release(&g, &ELM0);               /* not the holder */
    TEST_ASSERT_EQUAL_PTR(&CHIP, g.owner);
    og_core_release(&g, NULL);                /* nor is NULL    */
    TEST_ASSERT_EQUAL_PTR(&CHIP, g.owner);
}

void test_expired_hold_is_reaped(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));

    /* just before the deadline: still held */
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM0, HOLD - 1, HOLD));

    /* at the deadline: stale — reaped and granted */
    TEST_ASSERT_TRUE(og_core_try(&g, &ELM0, HOLD, HOLD));
    TEST_ASSERT_EQUAL_PTR(&ELM0, g.owner);
    TEST_ASSERT_EQUAL_UINT32(1, g.expiries);
    TEST_ASSERT_EQUAL(HOLD + HOLD, (int)g.deadline_ms);
}

void test_force_steals_regardless(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));
    og_core_force(&g, &ELM1, 100, HOLD);
    TEST_ASSERT_EQUAL_PTR(&ELM1, g.owner);
    TEST_ASSERT_EQUAL(100 + HOLD, (int)g.deadline_ms);
    TEST_ASSERT_EQUAL_UINT32(1, g.steals);

    /* the robbed owner's late release must not free the thief's hold */
    og_core_release(&g, &CHIP);
    TEST_ASSERT_EQUAL_PTR(&ELM1, g.owner);
}

void test_null_args_rejected(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_FALSE(og_core_try(NULL, &CHIP, 0, HOLD));
    TEST_ASSERT_FALSE(og_core_try(&g, NULL, 0, HOLD));
    TEST_ASSERT_NULL(g.owner);
    og_core_force(&g, NULL, 0, HOLD); /* no crash, no take */
    TEST_ASSERT_NULL(g.owner);
}

void test_waiter_owns_next_turn(void)
{
    og_core_t g = { 0 };

    /* the live starvation case: a tight requester loop must not re-win
       the gate ahead of a blocked waiter */
    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM0, 10, HOLD));  /* -> waiter */
    og_core_release(&g, &CHIP);

    /* chip's next command arrives FIRST — refused, the turn is reserved */
    TEST_ASSERT_FALSE(og_core_try(&g, &CHIP, 12, HOLD));
    TEST_ASSERT_TRUE(og_core_try(&g, &ELM0, 20, HOLD));
    TEST_ASSERT_EQUAL_PTR(&ELM0, g.owner);

    /* and now the roles swap: chip becomes the waiter */
    TEST_ASSERT_FALSE(og_core_try(&g, &CHIP, 30, HOLD));
    og_core_release(&g, &ELM0);
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM0, 40, HOLD));
    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 50, HOLD));
}

void test_reservation_expires_when_waiter_gives_up(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM0, 10, HOLD)); /* waiter, ttl 510 */
    og_core_release(&g, &CHIP);

    /* the waiter stopped polling: after OG_RESERVE_MS its claim lapses */
    TEST_ASSERT_FALSE(og_core_try(&g, &CHIP, 400, HOLD));
    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 511, HOLD));
}

void test_expired_hold_still_honors_waiter(void)
{
    og_core_t g = { 0 };

    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM0, HOLD - 10, HOLD)); /* waiter */

    /* chip's hold expires; a third owner reaps it but must NOT jump the
       waiter's reserved turn */
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM1, HOLD + 5, HOLD));
    TEST_ASSERT_EQUAL_UINT32(1, g.expiries);
    TEST_ASSERT_TRUE(og_core_try(&g, &ELM0, HOLD + 10, HOLD));
}

void test_three_owners_serialize_pairwise(void)
{
    og_core_t g = { 0 };

    /* chip -> elm0 -> elm1 in strict turn-taking via release */
    TEST_ASSERT_TRUE(og_core_try(&g, &CHIP, 0, HOLD));
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM0, 1, HOLD));
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM1, 2, HOLD));
    og_core_release(&g, &CHIP);

    TEST_ASSERT_TRUE(og_core_try(&g, &ELM0, 3, HOLD));
    TEST_ASSERT_FALSE(og_core_try(&g, &ELM1, 4, HOLD));
    og_core_release(&g, &ELM0);

    TEST_ASSERT_TRUE(og_core_try(&g, &ELM1, 5, HOLD));
    TEST_ASSERT_EQUAL_UINT32(3, g.acquires);
}

/* ---- the diagnostics hold (og_diag_*) --------------------------------- */

void test_diag_hold_refcounts_by_owner(void)
{
    og_diag_t d;

    og_diag_init(&d);
    TEST_ASSERT_FALSE(og_diag_held(&d));
    TEST_ASSERT_TRUE(og_diag_set(&d, &CHIP, true));   /* -> held        */
    TEST_ASSERT_TRUE(og_diag_held(&d));
    TEST_ASSERT_FALSE(og_diag_set(&d, &ELM0, true));  /* still held     */
    TEST_ASSERT_FALSE(og_diag_set(&d, &CHIP, true));  /* idempotent     */
    TEST_ASSERT_EQUAL(2, d.n_holders);
    TEST_ASSERT_FALSE(og_diag_set(&d, &CHIP, false)); /* one remains    */
    TEST_ASSERT_TRUE(og_diag_held(&d));
    TEST_ASSERT_TRUE(og_diag_set(&d, &ELM0, false));  /* -> released    */
    TEST_ASSERT_FALSE(og_diag_held(&d));
    TEST_ASSERT_FALSE(og_diag_set(&d, &ELM1, false)); /* non-holder     */
    TEST_ASSERT_EQUAL_UINT32(2, d.holds);
    TEST_ASSERT_EQUAL_UINT32(2, d.releases);
}

void test_diag_ack_resets_when_last_holder_leaves(void)
{
    og_diag_t d;

    og_diag_init(&d);
    og_diag_set(&d, &CHIP, true);
    og_diag_set(&d, &ELM0, true);
    d.acked = true;
    og_diag_set(&d, &CHIP, false);
    TEST_ASSERT_TRUE(d.acked);   /* someone still holds: ack stands  */
    og_diag_set(&d, &ELM0, false);
    TEST_ASSERT_FALSE(d.acked);  /* nobody: the next hold re-waits  */
}

void test_diag_fifth_holder_refused(void)
{
    static const int O[OG_DIAG_MAX_HOLDERS + 1];
    og_diag_t d;

    og_diag_init(&d);

    for (int i = 0; i < OG_DIAG_MAX_HOLDERS; i++)
    {
        og_diag_set(&d, &O[i], true);
    }

    TEST_ASSERT_FALSE(og_diag_set(&d, &O[OG_DIAG_MAX_HOLDERS], true));
    TEST_ASSERT_EQUAL_UINT32(1, d.overflow);
    TEST_ASSERT_EQUAL(OG_DIAG_MAX_HOLDERS, d.n_holders);
    TEST_ASSERT_TRUE(og_diag_held(&d));
}

void app_main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_free_gate_grants_and_holds);
    RUN_TEST(test_same_owner_reacquire_extends);
    RUN_TEST(test_release_frees_for_next_owner);
    RUN_TEST(test_wrong_owner_release_is_ignored);
    RUN_TEST(test_expired_hold_is_reaped);
    RUN_TEST(test_force_steals_regardless);
    RUN_TEST(test_null_args_rejected);
    RUN_TEST(test_waiter_owns_next_turn);
    RUN_TEST(test_reservation_expires_when_waiter_gives_up);
    RUN_TEST(test_expired_hold_still_honors_waiter);
    RUN_TEST(test_three_owners_serialize_pairwise);
    RUN_TEST(test_diag_hold_refcounts_by_owner);
    RUN_TEST(test_diag_ack_resets_when_last_holder_leaves);
    RUN_TEST(test_diag_fifth_holder_refused);

    UNITY_END();
}
