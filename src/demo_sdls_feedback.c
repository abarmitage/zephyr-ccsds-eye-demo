/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_sdls_feedback.h"

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

bool demo_sdls_feedback_select_fsr(struct demo_sdls_feedback_policy *policy,
				   const uint8_t clcw[4], bool cop_urgent)
{
	uint32_t word = sys_get_be32(clcw);
	uint8_t report = (uint8_t)word;
	bool flags_urgent = (word & (BIT(13) | BIT(12) | BIT(11))) != 0u;
	bool report_changed = !policy->report_valid || report != policy->last_report;

	policy->last_report = report;
	policy->report_valid = true;
	if (policy->previous_fsr || cop_urgent || flags_urgent || report_changed) {
		policy->previous_fsr = false;
		if (policy->clcw_streak < DEMO_SDLS_CLCWS_PER_FSR) {
			policy->clcw_streak++;
		}
		return false;
	}
	if (policy->clcw_streak < DEMO_SDLS_CLCWS_PER_FSR) {
		policy->clcw_streak++;
		return false;
	}
	policy->clcw_streak = 0u;
	policy->previous_fsr = true;
	return true;
}
