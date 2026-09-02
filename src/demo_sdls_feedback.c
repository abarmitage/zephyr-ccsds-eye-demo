/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_sdls_feedback.h"

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

void demo_sdls_feedback_latch_acceptance(
	struct demo_sdls_feedback_policy *policy,
	const uint8_t fsr[DEMO_SDLS_OCF_LEN])
{
	memcpy(policy->acceptance_fsr, fsr, sizeof(policy->acceptance_fsr));
	policy->acceptance_pending = true;
}

void demo_sdls_feedback_confirm_acceptance(
	struct demo_sdls_feedback_policy *policy)
{
	memset(policy->acceptance_fsr, 0, sizeof(policy->acceptance_fsr));
	policy->acceptance_pending = false;
}

bool demo_sdls_feedback_select_fsr(struct demo_sdls_feedback_policy *policy,
				   const uint8_t clcw[DEMO_SDLS_OCF_LEN],
				   const uint8_t current_fsr[DEMO_SDLS_OCF_LEN],
				   bool cop_urgent,
				   uint8_t selected[DEMO_SDLS_OCF_LEN])
{
	uint32_t word = sys_get_be32(clcw);
	uint8_t report = (uint8_t)word;
	bool flags_urgent = (word & (BIT(13) | BIT(12) | BIT(11))) != 0u;
	bool report_changed = !policy->report_valid || report != policy->last_report;

	policy->last_report = report;
	policy->report_valid = true;
	if (policy->acceptance_pending) {
		if (policy->previous_fsr) {
			policy->previous_fsr = false;
			memcpy(selected, clcw, DEMO_SDLS_OCF_LEN);
			return false;
		}
		policy->previous_fsr = true;
		memcpy(selected, policy->acceptance_fsr,
		       sizeof(policy->acceptance_fsr));
		return true;
	}
	if (policy->previous_fsr || cop_urgent || flags_urgent || report_changed) {
		policy->previous_fsr = false;
		if (policy->clcw_streak < DEMO_SDLS_CLCWS_PER_FSR) {
			policy->clcw_streak++;
		}
		memcpy(selected, clcw, DEMO_SDLS_OCF_LEN);
		return false;
	}
	if (policy->clcw_streak < DEMO_SDLS_CLCWS_PER_FSR) {
		policy->clcw_streak++;
		memcpy(selected, clcw, DEMO_SDLS_OCF_LEN);
		return false;
	}
	policy->clcw_streak = 0u;
	policy->previous_fsr = true;
	memcpy(selected, current_fsr, DEMO_SDLS_OCF_LEN);
	return true;
}
