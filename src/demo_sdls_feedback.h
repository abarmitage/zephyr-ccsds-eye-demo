/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_SDLS_FEEDBACK_H
#define CCSDS_EYE_DEMO_SDLS_FEEDBACK_H

#include <stdbool.h>
#include <stdint.h>

#define DEMO_SDLS_CLCWS_PER_FSR 3u

struct demo_sdls_feedback_policy {
	uint8_t clcw_streak;
	uint8_t last_report;
	bool report_valid;
	bool previous_fsr;
};

/** Return true when the current OCF may carry an FSR instead of @p clcw. */
bool demo_sdls_feedback_select_fsr(struct demo_sdls_feedback_policy *policy,
				   const uint8_t clcw[4], bool cop_urgent);

#endif /* CCSDS_EYE_DEMO_SDLS_FEEDBACK_H */
