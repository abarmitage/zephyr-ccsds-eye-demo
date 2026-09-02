/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "demo_sdls_feedback.h"

static void make_clcw(uint8_t out[4], uint8_t report, uint32_t flags)
{
	sys_put_be32(flags | report, out);
}

ZTEST(demo_sdls_feedback, test_fsr_is_bounded_and_clcw_follows)
{
	struct demo_sdls_feedback_policy policy = {0};
	uint8_t clcw[4];
	uint8_t fsr[4] = {0x80u, 1u, 2u, 3u};
	uint8_t selected[4];

	make_clcw(clcw, 7u, 0u);
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						     selected));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						     selected));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						     selected));
	zassert_true(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						    selected));
	zassert_mem_equal(selected, fsr, sizeof(selected));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						     selected),
		      "an FSR must be followed promptly by a CLCW");
	zassert_mem_equal(selected, clcw, sizeof(selected));
}

ZTEST(demo_sdls_feedback, test_urgent_and_changed_clcw_preempts_fsr)
{
	struct demo_sdls_feedback_policy policy = {
		.clcw_streak = DEMO_SDLS_CLCWS_PER_FSR,
		.last_report = 7u,
		.report_valid = true,
	};
	uint8_t clcw[4];
	uint8_t fsr[4] = {0x80u, 1u, 2u, 3u};
	uint8_t selected[4];

	make_clcw(clcw, 7u, 0u);
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, true,
						     selected));
	zassert_true(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						    selected));
	policy.previous_fsr = false;
	policy.clcw_streak = DEMO_SDLS_CLCWS_PER_FSR;
	make_clcw(clcw, 8u, 0u);
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						     selected));
	make_clcw(clcw, 8u, BIT(11));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, fsr, false,
						     selected));
}

ZTEST(demo_sdls_feedback, test_latched_acceptance_survives_loss_and_overwrite)
{
	struct demo_sdls_feedback_policy policy = {0};
	uint8_t clcw[4];
	uint8_t acceptance[4] = {0x80u, 3u, 0x2au, 0u};
	uint8_t overwritten[4] = {0xf0u, 1u, 0x99u, 0xffu};
	uint8_t selected[4];

	make_clcw(clcw, 9u, BIT(11));
	demo_sdls_feedback_latch_acceptance(&policy, acceptance);
	zassert_true(demo_sdls_feedback_select_fsr(
		&policy, clcw, overwritten, true, selected));
	zassert_mem_equal(selected, acceptance, sizeof(selected));

	/* Treat the first selected FSR as lost. A CLCW follows, then the exact
	 * acceptance value is repeated despite the changed live FSR.
	 */
	zassert_false(demo_sdls_feedback_select_fsr(
		&policy, clcw, overwritten, true, selected));
	zassert_mem_equal(selected, clcw, sizeof(selected));
	zassert_true(demo_sdls_feedback_select_fsr(
		&policy, clcw, overwritten, true, selected));
	zassert_mem_equal(selected, acceptance, sizeof(selected));

	demo_sdls_feedback_confirm_acceptance(&policy);
	zassert_false(policy.acceptance_pending);
	zassert_false(demo_sdls_feedback_select_fsr(
		&policy, clcw, overwritten, true, selected));
	zassert_mem_equal(selected, clcw, sizeof(selected));
}

ZTEST_SUITE(demo_sdls_feedback, NULL, NULL, NULL, NULL, NULL);
