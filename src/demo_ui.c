/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_ui.h"
#include "demo_view.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(eye_ui, CONFIG_LOG_DEFAULT_LEVEL);

#define SCREEN_WIDTH          240
#define TRACK_X               54
#define TRACK_WIDTH           132
#define PACKET_WIDTH          10
#define TRANSFER_ANIMATION_MS 900U
#define TC_ANIMATION_MS       250U

enum demo_state {
	DEMO_BOOT,
	DEMO_PEER_ABSENT_STATE,
	DEMO_IDLE,
	DEMO_TX,
	DEMO_RX,
	DEMO_REQUEST,
	DEMO_CAPTURING_STATE,
	DEMO_BUSY_STATE,
	DEMO_DUPLEX,
	DEMO_VERIFYING_STATE,
	DEMO_COMPLETE,
	DEMO_FAILED,
};

static const lv_color_t color_bg = LV_COLOR_MAKE(5, 9, 14);
static const lv_color_t color_panel = LV_COLOR_MAKE(12, 20, 29);
static const lv_color_t color_text = LV_COLOR_MAKE(225, 233, 240);
static const lv_color_t color_muted = LV_COLOR_MAKE(111, 128, 143);
static const lv_color_t color_cyan = LV_COLOR_MAKE(38, 198, 218);
static const lv_color_t color_amber = LV_COLOR_MAKE(245, 166, 35);
static const lv_color_t color_green = LV_COLOR_MAKE(65, 194, 117);
static const lv_color_t color_red = LV_COLOR_MAKE(226, 82, 76);

static lv_obj_t *status_label;
static lv_obj_t *protocol_screen;
static const struct demo_image_object *displayed_image;
static bool image_redraw_pending;
static struct demo_view_model view_model;
static lv_obj_t *input_label;
static lv_obj_t *tx_track;
static lv_obj_t *rx_track;
static lv_obj_t *tx_packet;
static lv_obj_t *rx_packet;
static lv_obj_t *tx_bar;
static lv_obj_t *rx_bar;
static lv_color_t local_accent;
static lv_color_t peer_accent;
static enum demo_state state;
static uint32_t state_started;
static bool cfdp_tx_active;
static bool cfdp_rx_active;
static bool tc_incoming;
static bool completion_pending;
static enum demo_transfer_direction completion_direction;

static lv_obj_t *solid_obj(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color)
{
	lv_obj_t *obj = lv_obj_create(parent);

	lv_obj_remove_style_all(obj);
	lv_obj_set_pos(obj, x, y);
	lv_obj_set_size(obj, width, height);
	lv_obj_set_style_bg_color(obj, color, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(obj, 1, 0);
	return obj;
}

static void create_spacecraft(lv_obj_t *parent, int x, int y, lv_color_t accent)
{
	(void)solid_obj(parent, x, y + 7, 11, 10, accent);
	(void)solid_obj(parent, x + 13, y + 3, 17, 18, color_text);
	(void)solid_obj(parent, x + 32, y + 7, 11, 10, accent);
	(void)solid_obj(parent, x + 19, y + 9, 5, 5, color_bg);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
	if (hidden) {
		lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
	}
}

static void set_activity(bool tx, bool rx)
{
	set_hidden(tx_track, !tx);
	set_hidden(tx_packet, !tx);
	set_hidden(tx_bar, !tx);
	set_hidden(rx_track, !rx);
	set_hidden(rx_packet, !rx);
	set_hidden(rx_bar, !rx);
}

static void set_state(enum demo_state next, uint32_t now_ms)
{
	state = next;
	state_started = now_ms;

	switch (state) {
	case DEMO_BOOT:
		set_activity(false, false);
		lv_label_set_text(status_label, "WIFI CONNECTING");
		break;
	case DEMO_PEER_ABSENT_STATE:
		set_activity(false, false);
		lv_label_set_text(status_label, "PEER UNAVAILABLE");
		break;
	case DEMO_IDLE:
		set_activity(false, false);
		lv_label_set_text(status_label, "READY  /  PEER OK");
		break;
	case DEMO_TX:
		set_activity(true, false);
		lv_label_set_text(status_label, "CFDP TX ACTIVE");
		lv_obj_set_style_bg_color(tx_packet, local_accent, 0);
		break;
	case DEMO_RX:
		set_activity(false, true);
		lv_label_set_text(status_label, "CFDP RX ACTIVE");
		lv_obj_set_style_bg_color(rx_packet, peer_accent, 0);
		break;
	case DEMO_REQUEST:
		set_activity(true, false);
		lv_label_set_text(status_label, "TC CAPTURE REQUEST");
		lv_obj_set_style_bg_color(tx_packet, color_amber, 0);
		break;
	case DEMO_CAPTURING_STATE:
		set_activity(false, false);
		lv_label_set_text(status_label, "CAPTURING STILL");
		break;
	case DEMO_BUSY_STATE:
		set_activity(false, false);
		lv_label_set_text(status_label, "BUSY");
		break;
	case DEMO_DUPLEX:
		set_activity(true, true);
		lv_label_set_text(status_label, "CFDP DUPLEX");
		lv_obj_set_style_bg_color(tx_packet, local_accent, 0);
		lv_obj_set_style_bg_color(rx_packet, peer_accent, 0);
		break;
	case DEMO_VERIFYING_STATE:
		set_activity(false, false);
		lv_label_set_text(status_label, "VERIFYING IMAGE");
		break;
	case DEMO_COMPLETE:
		set_activity(false, false);
		lv_label_set_text(status_label, "CHECKSUM OK  /  COMPLETE");
		lv_obj_set_style_text_color(status_label, color_green, 0);
		break;
	case DEMO_FAILED:
		set_activity(false, false);
		lv_label_set_text(status_label, "TRANSFER FAILED");
		lv_obj_set_style_text_color(status_label, color_red, 0);
		break;
	}

	if (state != DEMO_COMPLETE && state != DEMO_FAILED) {
		lv_obj_set_style_text_color(status_label, color_text, 0);
	}
}

static void update_packet(lv_obj_t *packet, lv_obj_t *bar, uint32_t progress, bool reverse)
{
	const int travel = TRACK_WIDTH - PACKET_WIDTH;
	int x = TRACK_X + (int)((progress * (uint32_t)travel) / 100U);

	if (reverse) {
		x = TRACK_X + travel - (x - TRACK_X);
	}

	lv_obj_set_x(packet, x);
	ARG_UNUSED(bar);
}

static uint32_t tc_progress(uint32_t elapsed)
{
	return MIN(elapsed, TC_ANIMATION_MS) * 100U / TC_ANIMATION_MS;
}

static void show_completion(enum demo_transfer_direction direction, uint32_t now_ms)
{
	set_state(DEMO_COMPLETE, now_ms);
	if (direction == DEMO_TRANSFER_TX) {
		lv_label_set_text(status_label, "TX DONE / LOCAL IMAGE");
	} else {
		lv_label_set_text(status_label, "CHECKSUM OK / RX DONE");
	}
}

void demo_ui_init(void)
{
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *label;
	lv_obj_t *divider;

	protocol_screen = screen;
	demo_view_init(&view_model);

	local_accent = IS_ENABLED(CONFIG_EYE_DEMO_ROLE_A) ? color_cyan : color_amber;
	peer_accent = IS_ENABLED(CONFIG_EYE_DEMO_ROLE_A) ? color_amber : color_cyan;

	lv_obj_set_style_bg_color(screen, color_bg, 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(screen, color_text, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	label = lv_label_create(screen);
	lv_label_set_text(label, CONFIG_EYE_DEMO_CALLSIGN);
	lv_obj_set_style_text_color(label, local_accent, 0);
	lv_obj_set_pos(label, 8, 8);

	label = lv_label_create(screen);
	lv_label_set_text(label, "CFDP");
	lv_obj_set_style_text_color(label, color_muted, 0);
	lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

	label = lv_label_create(screen);
	lv_label_set_text(label, CONFIG_EYE_DEMO_PEER_CALLSIGN);
	lv_obj_set_style_text_color(label, peer_accent, 0);
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -8, 8);

	divider = solid_obj(screen, 0, 30, SCREEN_WIDTH, 1, color_panel);
	ARG_UNUSED(divider);

	create_spacecraft(screen, 10, 48, local_accent);
	create_spacecraft(screen, 187, 48, peer_accent);

	label = lv_label_create(screen);
	lv_label_set_text_fmt(label, "ID %d", CONFIG_EYE_DEMO_LOCAL_ENTITY_ID);
	lv_obj_set_style_text_color(label, color_muted, 0);
	lv_obj_set_pos(label, 13, 72);

	label = lv_label_create(screen);
	lv_label_set_text_fmt(label, "ID %d", CONFIG_EYE_DEMO_PEER_ENTITY_ID);
	lv_obj_set_style_text_color(label, color_muted, 0);
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -13, 72);

	tx_track = solid_obj(screen, TRACK_X, 51, TRACK_WIDTH, 2, color_muted);
	rx_track = solid_obj(screen, TRACK_X, 76, TRACK_WIDTH, 2, color_muted);
	tx_packet = solid_obj(screen, TRACK_X, 47, PACKET_WIDTH, 9, color_cyan);
	rx_packet = solid_obj(screen, TRACK_X + TRACK_WIDTH - PACKET_WIDTH, 72, PACKET_WIDTH, 9,
			      color_cyan);

	status_label = lv_label_create(screen);
	lv_label_set_text_fmt(status_label, "%s  /  WIFI --", CONFIG_EYE_DEMO_LOCAL_IPV4);
	lv_obj_set_width(status_label, 224);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(status_label, 8, 102);

	tx_bar = lv_bar_create(screen);
	lv_obj_set_pos(tx_bar, 35, 128);
	lv_obj_set_size(tx_bar, 170, 7);
	lv_bar_set_range(tx_bar, 0, 100);
	lv_bar_set_value(tx_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(tx_bar, color_panel, LV_PART_MAIN);
	lv_obj_set_style_bg_color(tx_bar, color_cyan, LV_PART_INDICATOR);
	lv_obj_set_style_radius(tx_bar, 1, LV_PART_MAIN);
	lv_obj_set_style_radius(tx_bar, 1, LV_PART_INDICATOR);

	rx_bar = lv_bar_create(screen);
	lv_obj_set_pos(rx_bar, 35, 141);
	lv_obj_set_size(rx_bar, 170, 7);
	lv_bar_set_range(rx_bar, 0, 100);
	lv_bar_set_value(rx_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(rx_bar, color_panel, LV_PART_MAIN);
	lv_obj_set_style_bg_color(rx_bar, color_cyan, LV_PART_INDICATOR);
	lv_obj_set_style_radius(rx_bar, 1, LV_PART_MAIN);
	lv_obj_set_style_radius(rx_bar, 1, LV_PART_INDICATOR);

	input_label = lv_label_create(screen);
	lv_label_set_text(input_label, "INPUT: waiting");
	lv_obj_set_width(input_label, 224);
	lv_obj_set_style_text_align(input_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(input_label, color_muted, 0);
	lv_obj_set_pos(input_label, 8, 164);

	(void)solid_obj(screen, 0, 190, SCREEN_WIDTH, 1, color_panel);

	label = lv_label_create(screen);
	lv_label_set_text(label, "< SEND");
	lv_obj_set_pos(label, 4, 191);

	label = lv_label_create(screen);
	lv_label_set_text(label, "< REQUEST");
	lv_obj_set_pos(label, 4, 206);

	label = lv_label_create(screen);
	lv_label_set_text(label, "SHOW >");
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -4, 196);

	label = lv_label_create(screen);
	lv_label_set_text(label, CONFIG_EYE_DEMO_LOCAL_IPV4);
	lv_obj_set_style_text_color(label, color_muted, 0);
	lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -4);

	set_state(DEMO_BOOT, k_uptime_get_32());
}

static void leave_image_view(void)
{
	if (displayed_image == NULL) {
		LOG_WRN("SHOW leave requested without retained image");
		return;
	}
	demo_view_init(&view_model);
	image_redraw_pending = false;
	demo_service_release_display_image(displayed_image);
	displayed_image = NULL;
	lv_obj_invalidate(protocol_screen);
}

bool demo_ui_image_active(void)
{
	return displayed_image != NULL;
}

int demo_ui_render_image(const struct device *display)
{
	struct display_buffer_descriptor descriptor = {
		.buf_size = DEMO_IMAGE_PAYLOAD_SIZE,
		.width = DEMO_IMAGE_WIDTH,
		.pitch = DEMO_IMAGE_WIDTH,
		.height = DEMO_IMAGE_HEIGHT,
	};
	int rc;

	if (displayed_image == NULL || !image_redraw_pending) {
		return 0;
	}

	rc = display_write(display, 0, 0, &descriptor, displayed_image->pixels);
	if (rc == 0) {
		image_redraw_pending = false;
	}
	return rc;
}

void demo_ui_toggle_show(void)
{
	const struct demo_image_object *object = NULL;
	int rc;

	if (displayed_image != NULL) {
		leave_image_view();
		return;
	}
	rc = demo_service_acquire_display_image(&object);
	if (rc != 0) {
		(void)demo_view_toggle_show(&view_model, false);
		if (rc == -ENOENT) {
			lv_label_set_text(status_label, "NO LOCAL IMAGE");
		} else if (rc == -EBUSY) {
			lv_label_set_text(status_label, "IMAGE BUSY");
		} else {
			lv_label_set_text_fmt(status_label, "IMAGE ERROR %d", rc);
		}
		return;
	}
	(void)demo_view_toggle_show(&view_model, true);
	displayed_image = object;
	image_redraw_pending = true;
}

void demo_ui_prepare_action(void)
{
	if (displayed_image != NULL) {
		leave_image_view();
	} else {
		(void)demo_view_prepare_action(&view_model);
	}
}

void demo_ui_report_busy(void)
{
	set_state(DEMO_BUSY_STATE, k_uptime_get_32());
}

void demo_ui_handle_key(uint16_t code, int32_t value)
{
	const char *edge = value != 0 ? "DOWN" : "UP";

	lv_label_set_text_fmt(input_label, "INPUT: code %u  %s", code, edge);

	ARG_UNUSED(code);
}

void demo_ui_handle_event(const struct demo_ui_event *event)
{
	uint32_t now = k_uptime_get_32();

	switch (event->type) {
	case DEMO_UI_NETWORK_CONNECTING:
		set_state(DEMO_BOOT, now);
		break;
	case DEMO_UI_NETWORK_READY:
	case DEMO_UI_PEER_ABSENT:
		set_state(DEMO_PEER_ABSENT_STATE, now);
		break;
	case DEMO_UI_NETWORK_FAILED:
		set_state(DEMO_FAILED, now);
		lv_label_set_text(status_label, "WIFI / IP FAILED");
		break;
	case DEMO_UI_PEER_READY:
		set_state(DEMO_IDLE, now);
		break;
	case DEMO_UI_PEER_INVALID:
		set_state(DEMO_FAILED, now);
		lv_label_set_text_fmt(status_label, "PEER CONFIG ERROR %d", event->detail);
		break;
	case DEMO_UI_TC_TX:
		tc_incoming = false;
		set_state(DEMO_REQUEST, now);
		break;
	case DEMO_UI_TC_RX:
		tc_incoming = true;
		set_state(DEMO_REQUEST, now);
		lv_label_set_text(status_label, "TC REQUEST RECEIVED");
		break;
	case DEMO_UI_COMMAND_RESULT:
		if (event->detail == DEMO_COMMAND_ACCEPTED) {
			lv_label_set_text(status_label, "TC ACCEPTED / WAIT CFDP");
		} else if (event->detail == DEMO_COMMAND_BUSY) {
			lv_label_set_text(status_label, "TC PEER BUSY");
		} else if (event->detail == DEMO_COMMAND_DUPLICATE) {
			lv_label_set_text(status_label, "TC DUPLICATE");
		} else if (event->detail == DEMO_COMMAND_INVALID) {
			lv_label_set_text(status_label, "TC INVALID");
		} else {
			lv_label_set_text(status_label, "TC TIMED OUT");
		}
		break;
	case DEMO_UI_CAPTURING:
		set_state(DEMO_CAPTURING_STATE, now);
		break;
	case DEMO_UI_BUSY:
		set_state(DEMO_BUSY_STATE, now);
		break;
	case DEMO_UI_CFDP_TX:
		completion_pending = false;
		cfdp_tx_active = true;
		lv_bar_set_value(tx_bar, 0, LV_ANIM_OFF);
		update_packet(tx_packet, tx_bar, 0u, false);
		set_state(cfdp_rx_active ? DEMO_DUPLEX : DEMO_TX, now);
		break;
	case DEMO_UI_CFDP_RX:
		completion_pending = false;
		cfdp_rx_active = true;
		lv_bar_set_value(rx_bar, 0, LV_ANIM_OFF);
		update_packet(rx_packet, rx_bar, 0u, true);
		set_state(cfdp_tx_active ? DEMO_DUPLEX : DEMO_RX, now);
		break;
	case DEMO_UI_CFDP_PROGRESS: {
		uint32_t percent =
			demo_transfer_percent(event->bytes_transferred, event->file_size);
		if (event->detail == DEMO_TRANSFER_TX) {
			lv_bar_set_value(tx_bar, (int32_t)percent, LV_ANIM_OFF);
			update_packet(tx_packet, tx_bar, percent, false);
		} else {
			lv_bar_set_value(rx_bar, (int32_t)percent, LV_ANIM_OFF);
			update_packet(rx_packet, rx_bar, percent, true);
		}
		if (event->recovery_activity) {
			lv_label_set_text(status_label, event->detail == DEMO_TRANSFER_TX
								? "CFDP TX RECOVERY"
								: "CFDP RX RECOVERY");
		}
		break;
	}
	case DEMO_UI_VERIFYING:
		if (cfdp_rx_active) {
			lv_label_set_text(status_label, "VERIFYING RX OBJECT");
		} else {
			set_state(DEMO_VERIFYING_STATE, now);
		}
		break;
	case DEMO_UI_COMPLETE:
		if (event->detail == DEMO_TRANSFER_TX) {
			cfdp_tx_active = false;
		} else {
			cfdp_rx_active = false;
		}
		if (cfdp_tx_active || cfdp_rx_active) {
			set_state(cfdp_tx_active ? DEMO_TX : DEMO_RX, now);
		} else if (now - state_started < TRANSFER_ANIMATION_MS) {
			completion_pending = true;
			completion_direction = event->detail;
			lv_label_set_text(status_label, event->detail == DEMO_TRANSFER_TX
								? "TX ACKNOWLEDGED"
								: "RX VERIFIED");
		} else {
			show_completion(event->detail, now);
		}
		break;
	case DEMO_UI_FAILED:
		completion_pending = false;
		if (event->detail == DEMO_TRANSFER_TX) {
			cfdp_tx_active = false;
		} else if (event->detail == DEMO_TRANSFER_RX) {
			cfdp_rx_active = false;
		} else {
			cfdp_tx_active = false;
			cfdp_rx_active = false;
		}
		set_state(DEMO_FAILED, now);
		break;
	}
}

void demo_ui_tick(uint32_t now_ms)
{
	const uint32_t elapsed = now_ms - state_started;
	uint32_t progress;

	if (completion_pending && elapsed >= TRANSFER_ANIMATION_MS) {
		completion_pending = false;
		show_completion(completion_direction, now_ms);
		return;
	}

	switch (state) {
	case DEMO_TX:
	case DEMO_RX:
	case DEMO_DUPLEX:
		break;
	case DEMO_REQUEST:
		progress = tc_progress(elapsed);
		update_packet(tx_packet, tx_bar, progress, tc_incoming);
		break;
	case DEMO_CAPTURING_STATE:
	case DEMO_BUSY_STATE:
		break;
	case DEMO_COMPLETE:
	case DEMO_FAILED:
	case DEMO_IDLE:
	case DEMO_BOOT:
	case DEMO_PEER_ABSENT_STATE:
	case DEMO_VERIFYING_STATE:
		break;
	}
}
