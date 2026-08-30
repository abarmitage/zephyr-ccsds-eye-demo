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

	make_clcw(clcw, 7u, 0u);
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, false));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, false));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, false));
	zassert_true(demo_sdls_feedback_select_fsr(&policy, clcw, false));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, false),
		      "an FSR must be followed promptly by a CLCW");
}

ZTEST(demo_sdls_feedback, test_urgent_and_changed_clcw_preempts_fsr)
{
	struct demo_sdls_feedback_policy policy = {
		.clcw_streak = DEMO_SDLS_CLCWS_PER_FSR,
		.last_report = 7u,
		.report_valid = true,
	};
	uint8_t clcw[4];

	make_clcw(clcw, 7u, 0u);
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, true));
	zassert_true(demo_sdls_feedback_select_fsr(&policy, clcw, false));
	policy.previous_fsr = false;
	policy.clcw_streak = DEMO_SDLS_CLCWS_PER_FSR;
	make_clcw(clcw, 8u, 0u);
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, false));
	make_clcw(clcw, 8u, BIT(11));
	zassert_false(demo_sdls_feedback_select_fsr(&policy, clcw, false));
}

ZTEST_SUITE(demo_sdls_feedback, NULL, NULL, NULL, NULL, NULL);
