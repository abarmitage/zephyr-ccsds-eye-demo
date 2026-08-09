/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_ui.h"

#include <stdbool.h>
#include <stdio.h>

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#define SCREEN_WIDTH 240
#define TRACK_X 54
#define TRACK_WIDTH 132
#define PACKET_WIDTH 10

enum demo_state {
	DEMO_IDLE,
	DEMO_TX,
	DEMO_REQUEST,
	DEMO_DUPLEX,
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
static lv_obj_t *input_label;
static lv_obj_t *tx_track;
static lv_obj_t *rx_track;
static lv_obj_t *tx_packet;
static lv_obj_t *rx_packet;
static lv_obj_t *tx_bar;
static lv_obj_t *rx_bar;
static lv_obj_t *tx_caption;
static lv_obj_t *rx_caption;
static enum demo_state state;
static uint32_t state_started;

static lv_obj_t *solid_obj(lv_obj_t *parent, int x, int y, int width,
			   int height, lv_color_t color)
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

static void create_spacecraft(lv_obj_t *parent, int x, int y,
			      lv_color_t accent)
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
	set_hidden(tx_caption, !tx);
	set_hidden(rx_track, !rx);
	set_hidden(rx_packet, !rx);
	set_hidden(rx_bar, !rx);
	set_hidden(rx_caption, !rx);
}

static void set_state(enum demo_state next, uint32_t now_ms)
{
	state = next;
	state_started = now_ms;

	switch (state) {
	case DEMO_IDLE:
		set_activity(false, false);
		lv_label_set_text(status_label, "READY  /  PEER --");
		break;
	case DEMO_TX:
		set_activity(true, false);
		lv_label_set_text(status_label, "CAPTURED  /  CFDP TX");
		lv_obj_set_style_bg_color(tx_packet, color_cyan, 0);
		break;
	case DEMO_REQUEST:
		set_activity(true, false);
		lv_label_set_text(status_label, "TC CAPTURE REQUEST");
		lv_obj_set_style_bg_color(tx_packet, color_amber, 0);
		break;
	case DEMO_DUPLEX:
		set_activity(true, true);
		lv_label_set_text(status_label, "CFDP DUPLEX");
		lv_obj_set_style_bg_color(tx_packet, color_cyan, 0);
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

static void update_packet(lv_obj_t *packet, lv_obj_t *bar, uint32_t progress,
			  bool reverse)
{
	const int travel = TRACK_WIDTH - PACKET_WIDTH;
	int x = TRACK_X + (int)((progress * (uint32_t)travel) / 100U);

	if (reverse) {
		x = TRACK_X + travel - (x - TRACK_X);
	}

	lv_obj_set_x(packet, x);
	lv_bar_set_value(bar, (int32_t)progress, LV_ANIM_OFF);
}

void demo_ui_init(void)
{
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *label;
	lv_obj_t *divider;

	lv_obj_set_style_bg_color(screen, color_bg, 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(screen, color_text, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	label = lv_label_create(screen);
	lv_label_set_text(label, CONFIG_EYE_DEMO_CALLSIGN);
	lv_obj_set_pos(label, 8, 8);

	label = lv_label_create(screen);
	lv_label_set_text(label, "CFDP");
	lv_obj_set_style_text_color(label, color_cyan, 0);
	lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

	label = lv_label_create(screen);
	lv_label_set_text(label, CONFIG_EYE_DEMO_PEER_CALLSIGN);
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -8, 8);

	divider = solid_obj(screen, 0, 30, SCREEN_WIDTH, 1, color_panel);
	ARG_UNUSED(divider);

	create_spacecraft(screen, 10, 48,
			  IS_ENABLED(CONFIG_EYE_DEMO_ROLE_A) ? color_cyan : color_amber);
	create_spacecraft(screen, 187, 48,
			  IS_ENABLED(CONFIG_EYE_DEMO_ROLE_A) ? color_amber : color_cyan);

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
	rx_packet = solid_obj(screen, TRACK_X + TRACK_WIDTH - PACKET_WIDTH, 72,
			      PACKET_WIDTH, 9, color_cyan);

	tx_caption = lv_label_create(screen);
	lv_label_set_text(tx_caption, "TX");
	lv_obj_set_style_text_color(tx_caption, color_cyan, 0);
	lv_obj_set_pos(tx_caption, 30, 44);

	rx_caption = lv_label_create(screen);
	lv_label_set_text(rx_caption, "RX");
	lv_obj_set_style_text_color(rx_caption, color_cyan, 0);
	lv_obj_set_pos(rx_caption, 30, 69);

	status_label = lv_label_create(screen);
	lv_label_set_text(status_label, "READY  /  PEER --");
	lv_obj_set_width(status_label, 224);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(status_label, 8, 102);

	tx_bar = lv_bar_create(screen);
	lv_obj_set_pos(tx_bar, 35, 128);
	lv_obj_set_size(tx_bar, 170, 7);
	lv_bar_set_range(tx_bar, 0, 100);
	lv_obj_set_style_bg_color(tx_bar, color_panel, LV_PART_MAIN);
	lv_obj_set_style_bg_color(tx_bar, color_cyan, LV_PART_INDICATOR);
	lv_obj_set_style_radius(tx_bar, 1, LV_PART_MAIN);
	lv_obj_set_style_radius(tx_bar, 1, LV_PART_INDICATOR);

	rx_bar = lv_bar_create(screen);
	lv_obj_set_pos(rx_bar, 35, 141);
	lv_obj_set_size(rx_bar, 170, 7);
	lv_bar_set_range(rx_bar, 0, 100);
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
	lv_label_set_text(label, "PLAY  SEND");
	lv_obj_set_pos(label, 8, 202);

	label = lv_label_create(screen);
	lv_label_set_text(label, "MENU  REQUEST");
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -8, 202);

	label = lv_label_create(screen);
	lv_label_set_text(label, "UP  DUPLEX     DOWN  RESET");
	lv_obj_set_style_text_color(label, color_muted, 0);
	lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -7);

	set_state(DEMO_IDLE, k_uptime_get_32());
}

void demo_ui_handle_key(uint16_t code, int32_t value)
{
	const char *edge = value != 0 ? "DOWN" : "UP";

	lv_label_set_text_fmt(input_label, "INPUT: code %u  %s", code, edge);

	if (value == 0) {
		return;
	}

	switch (code) {
	case INPUT_KEY_PLAY:
		set_state(DEMO_TX, k_uptime_get_32());
		break;
	case INPUT_KEY_MENU:
		set_state(DEMO_REQUEST, k_uptime_get_32());
		break;
	case INPUT_KEY_UP:
		set_state(DEMO_DUPLEX, k_uptime_get_32());
		break;
	case INPUT_KEY_DOWN:
		set_state(DEMO_IDLE, k_uptime_get_32());
		break;
	case INPUT_KEY_0:
		set_state(DEMO_FAILED, k_uptime_get_32());
		break;
	default:
		break;
	}
}

void demo_ui_tick(uint32_t now_ms)
{
	const uint32_t elapsed = now_ms - state_started;
	uint32_t progress;

	switch (state) {
	case DEMO_TX:
		progress = MIN(elapsed / 30U, 100U);
		update_packet(tx_packet, tx_bar, progress, false);
		if (progress == 100U) {
			set_state(DEMO_COMPLETE, now_ms);
		}
		break;
	case DEMO_REQUEST:
		if (elapsed < 700U) {
			progress = MIN(elapsed / 7U, 100U);
			update_packet(tx_packet, tx_bar, progress, false);
		} else if (elapsed < 1100U) {
			set_activity(false, false);
			lv_label_set_text(status_label, "PEER CAPTURING");
		} else {
			set_activity(false, true);
			lv_label_set_text(status_label, "CFDP RETURN");
			progress = MIN((elapsed - 1100U) / 30U, 100U);
			update_packet(rx_packet, rx_bar, progress, true);
			if (progress == 100U) {
				set_state(DEMO_COMPLETE, now_ms);
			}
		}
		break;
	case DEMO_DUPLEX:
		progress = MIN(elapsed / 40U, 100U);
		update_packet(tx_packet, tx_bar, progress, false);
		update_packet(rx_packet, rx_bar, MIN((elapsed * 4U) / 170U, 100U),
			      true);
		if (progress == 100U) {
			set_state(DEMO_COMPLETE, now_ms);
		}
		break;
	case DEMO_COMPLETE:
	case DEMO_FAILED:
	case DEMO_IDLE:
		break;
	}
}

