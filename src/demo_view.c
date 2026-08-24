/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_view.h"

#include <stddef.h>

#include <zephyr/sys/__assert.h>

void demo_view_init(struct demo_view_model *model)
{
	__ASSERT_NO_MSG(model != NULL);
	model->view = DEMO_VIEW_PROTOCOL;
}

enum demo_show_result demo_view_toggle_show(struct demo_view_model *model, bool image_available)
{
	__ASSERT_NO_MSG(model != NULL);
	if (model->view == DEMO_VIEW_IMAGE) {
		model->view = DEMO_VIEW_PROTOCOL;
		return DEMO_SHOW_PROTOCOL;
	}
	if (!image_available) {
		return DEMO_SHOW_NO_IMAGE;
	}
	model->view = DEMO_VIEW_IMAGE;
	return DEMO_SHOW_IMAGE;
}

bool demo_view_prepare_action(struct demo_view_model *model)
{
	__ASSERT_NO_MSG(model != NULL);
	if (model->view == DEMO_VIEW_IMAGE) {
		model->view = DEMO_VIEW_PROTOCOL;
		return true;
	}
	return false;
}

bool demo_view_enter_link(struct demo_view_model *model)
{
	__ASSERT_NO_MSG(model != NULL);
	if (model->view != DEMO_VIEW_PROTOCOL) {
		return false;
	}
	model->view = DEMO_VIEW_LINK;
	return true;
}

bool demo_view_leave_link(struct demo_view_model *model)
{
	__ASSERT_NO_MSG(model != NULL);
	if (model->view != DEMO_VIEW_LINK) {
		return false;
	}
	model->view = DEMO_VIEW_PROTOCOL;
	return true;
}
