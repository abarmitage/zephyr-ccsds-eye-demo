/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "demo_ui.h"
#include "demo_service.h"

LOG_MODULE_REGISTER(eye_demo, CONFIG_LOG_DEFAULT_LEVEL);

struct demo_key_event {
	uint16_t code;
	int32_t value;
};

K_MSGQ_DEFINE(key_events, sizeof(struct demo_key_event), 16, 4);

static void input_event(struct input_event *event, void *user_data)
{
	const struct demo_key_event key = {
		.code = event->code,
		.value = event->value,
	};

	ARG_UNUSED(user_data);

	if (event->type == INPUT_EV_KEY) {
		(void)k_msgq_put(&key_events, &key, K_NO_WAIT);
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_event, NULL);

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	struct demo_key_event key;

	if (!device_is_ready(display)) {
		LOG_ERR("display is not ready");
		return 0;
	}
	demo_ui_init();
	(void)lv_timer_handler();
	(void)display_blanking_off(display);
	if (demo_service_start() != 0) {
		LOG_ERR("protocol service failed to start");
	}

	while (true) {
		struct demo_ui_event ui_event;

		while (k_msgq_get(&key_events, &key, K_NO_WAIT) == 0) {
			LOG_INF("input code=%u value=%d", key.code, key.value);
			demo_ui_handle_key(key.code, key.value);
			if (key.value != 0 && demo_ui_link_active()) {
				enum demo_link_action action = 0;
				bool has_action = true;

				if (key.code == INPUT_KEY_DOWN) {
					demo_ui_leave_link();
					has_action = false;
				} else if (key.code == INPUT_KEY_MENU) {
					action = DEMO_LINK_UNLOCK;
				} else if (key.code == INPUT_KEY_PLAY) {
					action = DEMO_LINK_SET_VR;
				} else if (key.code == INPUT_KEY_UP) {
					action = DEMO_LINK_SYNC_TX;
				} else {
					has_action = false;
				}
				if (has_action) {
					if (demo_service_queue_link_action(action)) {
						demo_ui_report_link_queued(action);
					} else {
						LOG_WRN("link action queue full");
					}
				}
			} else if (key.value != 0 && key.code == INPUT_KEY_MENU) {
				demo_ui_prepare_action();
				if (!demo_service_queue_local_send()) {
					demo_ui_report_busy();
				}
			} else if (key.value != 0 && key.code == INPUT_KEY_PLAY) {
				demo_ui_prepare_action();
				if (!demo_service_queue_remote_request()) {
					demo_ui_report_busy();
				}
			} else if (key.value != 0 && key.code == INPUT_KEY_UP) {
				demo_ui_toggle_show();
			} else if (key.value != 0 && key.code == INPUT_KEY_DOWN) {
				demo_ui_enter_link();
			}
		}
		while (demo_service_get_ui_event(&ui_event)) {
			demo_ui_handle_event(&ui_event);
		}

		if (demo_ui_image_active()) {
			int rc = demo_ui_render_image(display);

			if (rc != 0) {
				LOG_ERR("image display failed: %d", rc);
			}
		} else {
			demo_ui_tick(k_uptime_get_32());
			(void)lv_timer_handler();
		}
		k_sleep(K_MSEC(10));
	}

	return 0;
}
