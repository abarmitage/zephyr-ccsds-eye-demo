/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "demo_ui.h"

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

	while (true) {
		while (k_msgq_get(&key_events, &key, K_NO_WAIT) == 0) {
			LOG_INF("input code=%u value=%d", key.code, key.value);
			demo_ui_handle_key(key.code, key.value);
		}

		demo_ui_tick(k_uptime_get_32());
		(void)lv_timer_handler();
		k_sleep(K_MSEC(10));
	}

	return 0;
}

