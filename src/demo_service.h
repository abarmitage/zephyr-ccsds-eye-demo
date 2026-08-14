/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_SERVICE_H
#define CCSDS_EYE_DEMO_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "demo_image.h"
#include "demo_protocol.h"

enum demo_ui_event_type {
	DEMO_UI_NETWORK_CONNECTING,
	DEMO_UI_NETWORK_READY,
	DEMO_UI_NETWORK_FAILED,
	DEMO_UI_PEER_ABSENT,
	DEMO_UI_PEER_READY,
	DEMO_UI_PEER_INVALID,
	DEMO_UI_TC_TX,
	DEMO_UI_TC_RX,
	DEMO_UI_COMMAND_RESULT,
	DEMO_UI_CAPTURING,
	DEMO_UI_BUSY,
	DEMO_UI_CFDP_TX,
	DEMO_UI_CFDP_RX,
	DEMO_UI_CFDP_PROGRESS,
	DEMO_UI_VERIFYING,
	DEMO_UI_COMPLETE,
	DEMO_UI_FAILED,
};

struct demo_ui_event {
	enum demo_ui_event_type type;
	uint32_t request_id;
	int32_t detail;
	uint32_t bytes_transferred;
	uint32_t file_size;
	bool recovery_activity;
};

enum demo_transfer_direction {
	DEMO_TRANSFER_TX = 1,
	DEMO_TRANSFER_RX = 2,
};

int demo_service_start(void);
bool demo_service_queue_local_send(void);
bool demo_service_queue_remote_request(void);
bool demo_service_get_ui_event(struct demo_ui_event *event);
int demo_service_acquire_display_image(const struct demo_image_object **object);
void demo_service_release_display_image(const struct demo_image_object *object);

#endif /* CCSDS_EYE_DEMO_SERVICE_H */
