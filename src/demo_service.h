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
	DEMO_UI_LINK_FAILED,
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
	DEMO_UI_LINK_ACTION_RESULT,
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

enum demo_link_action {
	DEMO_LINK_UNLOCK = 1,
	DEMO_LINK_SYNC_TX,
	DEMO_LINK_SET_VR,
};

struct demo_link_snapshot {
	uint32_t new_frames;
	uint32_t retransmitted_frames;
	uint32_t packet_frames;
	uint32_t feedback_frames;
	uint32_t received_frames;
	uint32_t duplicate_frames;
	uint32_t rejected_frames;
	uint32_t dispatched_packets;
	uint32_t clcws;
	uint32_t acknowledgements;
	uint32_t timeout_events;
	uint32_t retry_exhaustion;
	uint32_t wait_events;
	uint32_t lockout_events;
	uint32_t terminal_failures;
	uint32_t submit_backpressure;
	uint32_t cfdp_naks_sent;
	uint32_t cfdp_naks_received;
	uint32_t cfdp_retransmissions;
	uint32_t route_failures;
	uint32_t sdls_protected;
	uint32_t sdls_authenticated;
	uint32_t sdls_failures;
	uint32_t sdls_replay_failures;
	uint32_t sdls_auth_failures;
	uint32_t sdls_sa_failures;
	uint32_t fsrs_sent;
	uint32_t fsrs_received;
	uint32_t otar_attempts;
	uint32_t otar_failures;
	uint8_t fop_state;
	uint8_t farm_state;
	uint8_t transmit_sequence;
	uint8_t expected_acknowledgement;
	uint8_t receive_sequence;
	uint8_t report_value;
	uint8_t report_advance;
	uint8_t transmission_count;
	uint8_t outstanding_frames;
	uint8_t peak_outstanding;
	uint8_t ingress_used;
	uint8_t ingress_peak;
	uint8_t ingress_capacity;
	bool peer_available;
	bool cfdp_tx_active;
	bool cfdp_rx_active;
	bool terminal_failure;
	bool sdls_adoption_armed;
	bool otar_pending;
	bool otar_cutover;
	bool otar_confirmed;
	bool otar_timed_out;
	int peer_error;
	int route_error;
	int udp_error;
	uint32_t ingress_overflow;
	uint32_t injected_faults;
	uint32_t submit_terminal_errors;
};

int demo_service_start(void);
bool demo_service_queue_local_send(void);
bool demo_service_queue_remote_request(void);
bool demo_service_queue_link_action(enum demo_link_action action);
bool demo_service_get_ui_event(struct demo_ui_event *event);
bool demo_service_get_link_snapshot(struct demo_link_snapshot *snapshot);
int demo_service_acquire_display_image(const struct demo_image_object **object);
void demo_service_release_display_image(const struct demo_image_object *object);

#endif /* CCSDS_EYE_DEMO_SERVICE_H */
