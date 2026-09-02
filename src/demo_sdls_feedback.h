/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_SDLS_FEEDBACK_H
#define CCSDS_EYE_DEMO_SDLS_FEEDBACK_H

#include <stdbool.h>
#include <stdint.h>

#define DEMO_SDLS_CLCWS_PER_FSR 3u
#define DEMO_SDLS_OCF_LEN        4u

struct demo_sdls_feedback_policy {
	uint8_t acceptance_fsr[DEMO_SDLS_OCF_LEN];
	uint8_t clcw_streak;
	uint8_t last_report;
	bool report_valid;
	bool previous_fsr;
	bool acceptance_pending;
};

/** Retain an OTAR-acceptance FSR until new-key traffic confirms the cutover. */
void demo_sdls_feedback_latch_acceptance(
	struct demo_sdls_feedback_policy *policy,
	const uint8_t fsr[DEMO_SDLS_OCF_LEN]);

/** Stop repeating a retained OTAR-acceptance FSR. */
void demo_sdls_feedback_confirm_acceptance(
	struct demo_sdls_feedback_policy *policy);

/** Select one complete OCF, returning true when @p selected contains an FSR. */
bool demo_sdls_feedback_select_fsr(struct demo_sdls_feedback_policy *policy,
				   const uint8_t clcw[DEMO_SDLS_OCF_LEN],
				   const uint8_t current_fsr[DEMO_SDLS_OCF_LEN],
				   bool cop_urgent,
				   uint8_t selected[DEMO_SDLS_OCF_LEN]);

#endif /* CCSDS_EYE_DEMO_SDLS_FEEDBACK_H */
