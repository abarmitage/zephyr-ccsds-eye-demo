/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_VIEW_H
#define CCSDS_EYE_DEMO_VIEW_H

#include <stdbool.h>

enum demo_view {
	DEMO_VIEW_PROTOCOL,
	DEMO_VIEW_IMAGE,
	DEMO_VIEW_LINK,
};

enum demo_show_result {
	DEMO_SHOW_IMAGE,
	DEMO_SHOW_PROTOCOL,
	DEMO_SHOW_NO_IMAGE,
};

struct demo_view_model {
	enum demo_view view;
};

void demo_view_init(struct demo_view_model *model);
enum demo_show_result demo_view_toggle_show(struct demo_view_model *model, bool image_available);
bool demo_view_prepare_action(struct demo_view_model *model);
bool demo_view_enter_link(struct demo_view_model *model);
bool demo_view_leave_link(struct demo_view_model *model);

#endif /* CCSDS_EYE_DEMO_VIEW_H */
