/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_service.h"
#include "demo_camera.h"

#include <errno.h>
#include <string.h>

#include <ccsds/ccsds_cfdp_pdu.h>
#include <ccsds/ccsds_cfdp_service.h>
#include <ccsds/ccsds_profile.h>
#include <ccsds/ccsds_router.h>
#include <ccsds/ccsds_space_packet.h>
#include <ccsds/ccsds_udp.h>
#include <ccsds/ccsds_uslp_frame.h>
#include <ccsds/ccsds_uslp_peer.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(eye_service, CONFIG_LOG_DEFAULT_LEVEL);

#define RX_QUEUE_DEPTH               16u
#define ROUTER_QUEUE_DEPTH           8u
#define ACTION_QUEUE_DEPTH           4u
#define UI_QUEUE_DEPTH               16u
#define WORKER_STACK_SIZE            7168u
#define WORKER_PRIORITY              10
#define STATUS_PERIOD_MS             2000u
#define PEER_TIMEOUT_MS              6500u
#define COMMAND_TIMEOUT_MS           5000u
#define REQUEST_RETENTION_MS         60000u
#define WORKER_IDLE_MS               2u
#define CFDP_POLL_MS                 10u
#define LINK_SNAPSHOT_MS             5000u
#define LINK_UI_SNAPSHOT_MS          250u
#define PROGRESS_UI_PERIOD_MS        100u
#define PRE_CAPTURE_QUIET_PERIOD_MS  250u
#define POST_CAPTURE_QUIET_PERIOD_MS 250u
#define CFDP_FIXED_HEADER_LEN        (4u + (2u * 1u) + 2u)
#define CFDP_FILE_OFFSET_LEN         4u
#define CFDP_FILE_PDU_OVERHEAD       (CFDP_FIXED_HEADER_LEN + CFDP_FILE_OFFSET_LEN)
#define CFDP_PACKET_OVERHEAD         (CCSDS_SPACE_PACKET_PRIMARY_HDR_LEN + CFDP_FILE_PDU_OVERHEAD)
#define PEER_PACKET_CAPACITY         (CCSDS_SPACE_PACKET_PRIMARY_HDR_LEN + CCSDS_CFDP_MAX_PDU_SIZE)
#define PEER_PACKET_SLOTS            4u
#define LOCAL_SCID_INDICATION                                                                      \
	(IS_ENABLED(CONFIG_EYE_DEMO_LOCAL_SCID_IS_SOURCE) ? CCSDS_USLP_SCID_IS_SOURCE              \
							  : CCSDS_USLP_SCID_IS_DESTINATION)
#define PEER_SCID_INDICATION                                                                       \
	(IS_ENABLED(CONFIG_EYE_DEMO_PEER_SCID_IS_SOURCE) ? CCSDS_USLP_SCID_IS_SOURCE               \
							 : CCSDS_USLP_SCID_IS_DESTINATION)
#define CFDP_MAX_FILE_DATA_PDUS                                                                    \
	DIV_ROUND_UP(DEMO_IMAGE_OBJECT_SIZE, CONFIG_CCSDS_CFDP_MAX_SEGMENT_SIZE)
#define CFDP_MAX_SYNCHRONOUS_PACKETS (CFDP_MAX_FILE_DATA_PDUS + 2u)

BUILD_ASSERT(CONFIG_CCSDS_UDP_MAX_UNIT_LEN <= 1280,
	     "UDP unit must remain below a conservative non-fragmenting IPv4 payload");
BUILD_ASSERT(CONFIG_CCSDS_UDP_MAX_UNIT_LEN == CONFIG_CCSDS_MAX_FRAME_LEN,
	     "one complete USLP frame must fit one UDP datagram");
BUILD_ASSERT(PEER_PACKET_CAPACITY + CCSDS_USLP_PRIMARY_HDR_LEN + CCSDS_USLP_TFDF_HDR_LEN +
			     CCSDS_USLP_OCF_LEN <=
		     CONFIG_CCSDS_MAX_FRAME_LEN,
	     "maximum encoded Space Packet must fit one complete USLP frame");
BUILD_ASSERT(CONFIG_CCSDS_CFDP_MAX_SEGMENT_SIZE + CFDP_FILE_PDU_OVERHEAD <= CCSDS_CFDP_MAX_PDU_SIZE,
	     "CFDP File Data PDU must fit the configured PDU buffer");
BUILD_ASSERT(CONFIG_CCSDS_CFDP_MAX_SEGMENT_SIZE + CFDP_PACKET_OVERHEAD <= PEER_PACKET_CAPACITY,
	     "encoded CFDP Space Packet must fit peer admission storage");
BUILD_ASSERT(CONFIG_CCSDS_CFDP_MAX_SEGMENT_SIZE ==
		     MIN(CCSDS_CFDP_MAX_PDU_SIZE - CFDP_FILE_PDU_OVERHEAD,
			 PEER_PACKET_CAPACITY - CFDP_PACKET_OVERHEAD),
	     "select the largest CFDP file-data segment that fits the USLP profile");
BUILD_ASSERT(CFDP_MAX_SYNCHRONOUS_PACKETS == 117u,
	     "update the bounded CFDP burst analysis when the image or segment "
	     "size changes");

enum action_type {
	ACTION_LOCAL_SEND,
	ACTION_REMOTE_REQUEST,
	ACTION_LINK_UNLOCK,
	ACTION_LINK_SYNC_TX,
	ACTION_LINK_SET_VR,
};

enum network_event_type {
	NETWORK_ASSOCIATED,
	NETWORK_DISCONNECTED,
};

enum link_sync_phase {
	LINK_SYNC_CLCW,
	LINK_SYNC_UNLOCK,
	LINK_SYNC_READY,
	LINK_SYNC_FAILED,
};

enum operation_origin {
	OPERATION_LOCAL_BUTTON,
	OPERATION_REMOTE_COMMAND,
};

enum router_message_type {
	ROUTER_CFDP,
	ROUTER_CAPTURE_COMMAND,
	ROUTER_COMMAND_STATUS,
	ROUTER_PEER_STATUS,
};

struct rx_datagram {
	size_t length;
	uint8_t data[CONFIG_CCSDS_UDP_MAX_UNIT_LEN];
};

struct router_message {
	enum router_message_type type;
	size_t length;
	uint8_t payload[CCSDS_CFDP_MAX_PDU_SIZE];
};

struct action_message {
	enum action_type type;
};

struct network_event_message {
	enum network_event_type type;
};

struct send_operation {
	uint64_t destination_entity_id;
	enum operation_origin origin;
	uint32_t request_id;
	bool has_request_id;
};

struct progress_update {
	bool pending;
	bool recovery_activity;
	uint32_t bytes_transferred;
	uint32_t file_size;
	uint64_t next_ui_ms;
};

struct callback_progress_snapshot {
	bool pending;
	bool recovery_activity;
	uint32_t bytes_transferred;
	uint32_t file_size;
};

struct callback_terminal_snapshot {
	bool pending;
	ccsds_cfdp_event_t event;
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_datagram), RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(router_queue, sizeof(struct router_message), ROUTER_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(action_queue, sizeof(struct action_message), ACTION_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(network_event_queue, sizeof(struct network_event_message), 2, 4);
K_MSGQ_DEFINE(ui_queue, sizeof(struct demo_ui_event), UI_QUEUE_DEPTH, 4);
K_SEM_DEFINE(worker_wake, 0, 1);
K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK_SIZE);

static struct k_thread worker_thread;
static struct net_mgmt_event_callback wifi_callback;
static struct net_if *wifi_iface;
static struct ccsds_udp udp;
static struct ccsds_uslp_peer peer;
static struct ccsds_uslp_peer_config peer_config;
static struct ccsds_router router;
static struct ccsds_profile_input input_profile;
static struct ccsds_cfdp_service cfdp_service;
static struct demo_dedup_cache dedup_cache;
static bool protocol_ready;
static bool protocol_initialized;
static bool peer_ready;
static enum link_sync_phase link_sync_phase;
static bool operation_active;
static bool outgoing_active;
static struct send_operation active_operation;
static uint64_t peer_last_seen_ms;
static uint64_t next_status_ms;
static uint64_t next_cfdp_poll_ms;
static uint64_t next_link_snapshot_ms;
static uint64_t next_link_ui_snapshot_ms;
static uint32_t packet_sequence;
static uint32_t next_request_id = 1u;
static uint32_t pending_request_id;
static uint64_t pending_request_deadline_ms;
static bool receiver_wait_logged;
static uint32_t receiver_retry_logged;
static struct progress_update tx_progress;
static struct progress_update rx_progress;
static struct callback_progress_snapshot callback_progress[2];
static struct callback_terminal_snapshot callback_terminal[2];
static struct k_spinlock callback_progress_lock;
static struct demo_link_snapshot link_snapshot;
static struct k_spinlock link_snapshot_lock;
static bool link_snapshot_valid;
static atomic_t rx_queue_drops;
static atomic_t rx_queue_used;
static atomic_t rx_queue_peak;
static uint8_t peer_packet_queue[PEER_PACKET_SLOTS][PEER_PACKET_CAPACITY];
static size_t peer_packet_lengths[PEER_PACKET_SLOTS];
static uint8_t peer_sent_frames[CCSDS_USLP_PEER_WINDOW_K][CONFIG_CCSDS_MAX_FRAME_LEN];
static size_t peer_sent_lengths[CCSDS_USLP_PEER_WINDOW_K];
static size_t peak_outstanding;
static uint32_t submit_backpressure;
static uint32_t submit_terminal_errors;
static uint32_t injected_faults;
static uint32_t cfdp_naks_sent;
static uint32_t cfdp_naks_received;
static uint32_t cfdp_retransmissions;
static uint32_t validated_image_commits;
static int terminal_route_error;
static bool fault_injected;
static uint32_t link_failure_events;
static bool initial_link_acquisition = true;
static struct demo_image_object image_objects[DEMO_IMAGE_SLOT_COUNT]
	__attribute__((section(".ext_ram.bss.eye_images"), aligned(32)));
static struct demo_image_slot image_slots[DEMO_IMAGE_SLOT_COUNT];
static struct demo_image_store image_store;
static struct demo_image_source image_source;
static struct demo_image_receiver image_receiver;
static struct demo_image_slot *tx_slot;

static void send_peer_status(void);
static void process_progress_snapshots(uint64_t now);
static void service_peer_ingress(uint64_t now);
static void trace_link_snapshot(const char *reason);
static void publish_link_snapshot(void);
static void ui_event(enum demo_ui_event_type type, uint32_t request_id,
		     int32_t detail);
static struct k_mutex image_lock;
static struct demo_camera_adapter camera_adapter;
static int camera_status;

static int start_link_sync(uint64_t now, bool preserve_receive_sequence)
{
	struct ccsds_uslp_peer_config config = peer_config;
	int rc;

	if (preserve_receive_sequence) {
		struct ccsds_uslp_peer_snapshot snapshot;

		ccsds_uslp_peer_get_snapshot(&peer, &snapshot);
		config.initial_receive_sequence = snapshot.receive_sequence;
	}
	rc = ccsds_uslp_peer_init(&peer, &config, now);

	if (rc == 0) {
		rc = ccsds_uslp_peer_synchronize(&peer);
	}
	if (rc == 0) {
		link_sync_phase = LINK_SYNC_CLCW;
		peer_ready = false;
		peak_outstanding = 0u;
		LOG_INF("USLP COP-1 synchronization started");
	}
	return rc;
}

static int advance_link_sync(uint64_t now, int peer_rc,
			     const struct ccsds_uslp_peer_snapshot *snapshot)
{
	int rc;

	if (link_sync_phase == LINK_SYNC_FAILED) {
		return peer_rc;
	}
	if (peer_rc == -ETIMEDOUT || peer_rc == -ESHUTDOWN) {
		peer_ready = false;
		link_failure_events++;
		link_sync_phase = LINK_SYNC_FAILED;
		LOG_ERR("USLP COP-1 link error: %d (event %u); operator recovery required",
			peer_rc, link_failure_events);
		ui_event(DEMO_UI_LINK_FAILED, 0u, peer_rc);
		trace_link_snapshot("link-terminal");
		ccsds_cfdp_entity_abort_transactions(
			&cfdp_service.entity, CCSDS_CFDP_STATUS_INACTIVITY_DETECTED);
		return peer_rc;
	}
	if (peer_rc != 0) {
		return peer_rc;
	}
	if (snapshot->fop_state == CCSDS_USLP_FOP_INITIAL) {
		if (!initial_link_acquisition) {
			peer_ready = false;
			link_sync_phase = LINK_SYNC_FAILED;
			link_failure_events++;
			LOG_ERR("USLP COP-1 peer lockout after initial acquisition "
				"(event %u); operator recovery required",
				link_failure_events);
			ui_event(DEMO_UI_LINK_FAILED, 0u, -EACCES);
			trace_link_snapshot("link-lockout");
			ccsds_cfdp_entity_abort_transactions(
				&cfdp_service.entity, CCSDS_CFDP_STATUS_INACTIVITY_DETECTED);
			return -EACCES;
		}
		rc = ccsds_uslp_peer_send_unlock(&peer);
		if (rc == 0) {
			link_sync_phase = LINK_SYNC_UNLOCK;
			LOG_WRN("USLP COP-1 peer locked on initial acquisition; UNLOCK queued");
		}
		return rc;
	}
	if (snapshot->fop_state == CCSDS_USLP_FOP_ACTIVE &&
	    link_sync_phase != LINK_SYNC_READY) {
		link_sync_phase = LINK_SYNC_READY;
		next_status_ms = now;
		LOG_INF("USLP COP-1 synchronized at V(S)=%u from peer CLCW",
			snapshot->transmit_sequence);
	}
	return 0;
}

static void ui_event(enum demo_ui_event_type type, uint32_t request_id, int32_t detail)
{
	const struct demo_ui_event event = {
		.type = type,
		.request_id = request_id,
		.detail = detail,
	};

	(void)k_msgq_put(&ui_queue, &event, K_NO_WAIT);
}

static void ui_progress_event(enum demo_transfer_direction direction,
			      const struct progress_update *progress)
{
	const struct demo_ui_event event = {
		.type = DEMO_UI_CFDP_PROGRESS,
		.detail = direction,
		.bytes_transferred = progress->bytes_transferred,
		.file_size = progress->file_size,
		.recovery_activity = progress->recovery_activity,
	};

	(void)k_msgq_put(&ui_queue, &event, K_NO_WAIT);
}

static uint64_t now_ms(void *user)
{
	ARG_UNUSED(user);
	return (uint64_t)k_uptime_get();
}

static int source_open_read(void *user, const char *path, void **handle, uint32_t *size)
{
	struct demo_image_source *source = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_source_open(source, path, handle, size);
	k_mutex_unlock(&image_lock);
	return rc;
}

static int source_read(void *user, void *handle, uint32_t offset, uint8_t *buffer, size_t length,
		       size_t *read_length)
{
	struct demo_image_source *source = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_source_read(source, handle, offset, buffer, length, read_length);
	k_mutex_unlock(&image_lock);
	return rc;
}

static int source_close(void *user, void *handle)
{
	struct demo_image_source *source = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_source_close(source, handle);
	k_mutex_unlock(&image_lock);
	return rc;
}

static int receive_open_write(void *user, const char *path, void **handle)
{
	struct demo_image_receiver *receiver = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_receiver_open(receiver, path, handle);
	k_mutex_unlock(&image_lock);
	return rc;
}

static int receive_write(void *user, void *handle, uint32_t offset, const uint8_t *buffer,
			 size_t length)
{
	struct demo_image_receiver *receiver = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_receiver_write(receiver, handle, offset, buffer, length);
	k_mutex_unlock(&image_lock);
	return rc;
}

static int receive_commit(void *user, const char *path)
{
	struct demo_image_receiver *receiver = user;
	int rc;

	ui_event(DEMO_UI_VERIFYING, 0u, 0);
	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_receiver_validate_complete(receiver, path);
	k_mutex_unlock(&image_lock);
	LOG_INF("CFDP RX image validation %s", rc == 0 ? "passed" : "failed");
	return rc;
}

static int receive_discard(void *user, const char *path)
{
	struct demo_image_receiver *receiver = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_receiver_discard(receiver, path);
	k_mutex_unlock(&image_lock);
	return rc;
}

static int receive_read(void *user, void *handle, uint32_t offset, uint8_t *buffer, size_t length,
			size_t *read_length)
{
	struct demo_image_receiver *receiver = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_receiver_read(receiver, handle, offset, buffer, length, read_length);
	k_mutex_unlock(&image_lock);
	return rc;
}

static int receive_close(void *user, void *handle)
{
	struct demo_image_receiver *receiver = user;
	int rc;

	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_receiver_close(receiver, handle);
	k_mutex_unlock(&image_lock);
	return rc;
}

static const ccsds_cfdp_filestore_ops_t source_ops = {
	.user = &image_source,
	.open_read = source_open_read,
	.read = source_read,
	.close = source_close,
};

static const ccsds_cfdp_filestore_ops_t receive_ops = {
	.user = &image_receiver,
	.open_write_tmp = receive_open_write,
	.read = receive_read,
	.write = receive_write,
	.close = receive_close,
	.commit_tmp = receive_commit,
	.discard_tmp = receive_discard,
};

static int queue_router_message(enum router_message_type type, const uint8_t *payload,
				size_t length)
{
	struct router_message message;
	int rc;

	if (payload == NULL || length == 0u || length > sizeof(message.payload)) {
		return -EINVAL;
	}
	message.type = type;
	message.length = length;
	memcpy(message.payload, payload, length);
	rc = k_msgq_put(&router_queue, &message, K_NO_WAIT);
	return rc == -ENOMSG ? -ENOSPC : rc;
}

static int cfdp_router_handler(const struct ccsds_space_packet *packet, void *user_data)
{
	ccsds_cfdp_pdu_header_t header;
	size_t consumed;

	ARG_UNUSED(user_data);
	if (packet == NULL || packet->version != 0u || packet->secondary_header ||
	    packet->sequence_flags != CCSDS_SEQUENCE_UNSEGMENTED || packet->payload_len == 0u ||
	    packet->payload_len > CCSDS_CFDP_MAX_PDU_SIZE ||
	    ccsds_cfdp_decode_header(packet->payload, packet->payload_len, &header, &consumed) !=
		    CCSDS_CFDP_STATUS_OK) {
		return -EINVAL;
	}
	return queue_router_message(ROUTER_CFDP, packet->payload, packet->payload_len);
}

static int command_router_handler(const struct ccsds_space_packet *packet, void *user_data)
{
	ARG_UNUSED(user_data);
	if (packet == NULL || packet->type != CCSDS_PACKET_TYPE_TC || packet->payload == NULL ||
	    packet->payload_len == 0u || packet->payload_len > CCSDS_CFDP_MAX_PDU_SIZE) {
		return -EINVAL;
	}
	return queue_router_message(ROUTER_CAPTURE_COMMAND, packet->payload, packet->payload_len);
}

static int command_status_router_handler(const struct ccsds_space_packet *packet, void *user_data)
{
	struct demo_command_status status;

	ARG_UNUSED(user_data);
	if (packet == NULL || packet->type != CCSDS_PACKET_TYPE_TM ||
	    demo_command_status_decode(packet->payload, packet->payload_len, &status) != 0) {
		return -EINVAL;
	}
	return queue_router_message(ROUTER_COMMAND_STATUS, packet->payload, packet->payload_len);
}

static int peer_status_router_handler(const struct ccsds_space_packet *packet, void *user_data)
{
	ARG_UNUSED(user_data);
	if (packet == NULL || packet->type != CCSDS_PACKET_TYPE_TM || packet->payload == NULL ||
	    packet->payload_len != DEMO_PEER_STATUS_LEN) {
		return -EINVAL;
	}
	return queue_router_message(ROUTER_PEER_STATUS, packet->payload, packet->payload_len);
}

static int udp_receive(void *user, const uint8_t *unit, size_t length)
{
	struct rx_datagram datagram;
	int rc;

	ARG_UNUSED(user);
	if (length == 0u || length > sizeof(datagram.data)) {
		return -EMSGSIZE;
	}
	datagram.length = length;
	memcpy(datagram.data, unit, length);
	rc = k_msgq_put(&rx_queue, &datagram, K_NO_WAIT);
	if (rc == 0) {
		atomic_val_t used = atomic_inc(&rx_queue_used) + 1;
		atomic_val_t peak = atomic_get(&rx_queue_peak);

		while (used > peak && !atomic_cas(&rx_queue_peak, peak, used)) {
			peak = atomic_get(&rx_queue_peak);
		}
		k_sem_give(&worker_wake);
	} else {
		atomic_inc(&rx_queue_drops);
	}
	return rc;
}

static void cfdp_event_callback(void *user, const ccsds_cfdp_event_t *event)
{
	const bool progress_event = event->type == CCSDS_CFDP_EVENT_TRANSACTION_STARTED ||
				    event->type == CCSDS_CFDP_EVENT_FILE_SEGMENT_SENT ||
				    event->type == CCSDS_CFDP_EVENT_FILE_SEGMENT_RECV ||
				    event->type == CCSDS_CFDP_EVENT_NAK_SENT ||
				    event->type == CCSDS_CFDP_EVENT_NAK_RECV ||
				    event->type == CCSDS_CFDP_EVENT_RETRANSMIT;

	ARG_UNUSED(user);
	if (event->type == CCSDS_CFDP_EVENT_NAK_SENT) {
		cfdp_naks_sent++;
	} else if (event->type == CCSDS_CFDP_EVENT_NAK_RECV) {
		cfdp_naks_received++;
	} else if (event->type == CCSDS_CFDP_EVENT_RETRANSMIT) {
		cfdp_retransmissions++;
	}
	if (progress_event) {
		const size_t index = event->direction == CCSDS_CFDP_DIRECTION_SENDER ? 0u : 1u;
		k_spinlock_key_t key = k_spin_lock(&callback_progress_lock);

		callback_progress[index].bytes_transferred = event->bytes_transferred;
		callback_progress[index].file_size = event->file_size;
		callback_progress[index].recovery_activity |=
			event->phase == CCSDS_CFDP_PHASE_RECOVERY;
		callback_progress[index].pending = true;
		k_spin_unlock(&callback_progress_lock, key);
		k_sem_give(&worker_wake);
		return;
	}
	if (event->type == CCSDS_CFDP_EVENT_COMPLETE || event->type == CCSDS_CFDP_EVENT_FAILED) {
		const size_t index = event->direction == CCSDS_CFDP_DIRECTION_SENDER ? 0u : 1u;
		k_spinlock_key_t key = k_spin_lock(&callback_progress_lock);

		/* Keep the first terminal event until the worker consumes it.  It owns
		 * application cleanup and must never be displaced by later traffic.
		 */
		if (!callback_terminal[index].pending) {
			callback_terminal[index].event = *event;
			callback_terminal[index].pending = true;
		}
		k_spin_unlock(&callback_progress_lock, key);
		k_sem_give(&worker_wake);
	}
}

static int route_frame(void *user, const uint8_t *frame, size_t frame_len)
{
	const bool feedback_only = frame_len == (CCSDS_USLP_PRIMARY_HDR_LEN + CCSDS_USLP_OCF_LEN);
	int rc;

	ARG_UNUSED(user);
	if (IS_ENABLED(CONFIG_EYE_DEMO_FAULT_WITHHOLD_FEEDBACK) && !fault_injected &&
	    (outgoing_active || cfdp_service.entity.receiver.active) && feedback_only) {
		injected_faults++;
		if (injected_faults >= CONFIG_EYE_DEMO_FAULT_WITHHOLD_COUNT) {
			fault_injected = true;
		}
		return 0;
	}
	if (!fault_injected &&
	    ((IS_ENABLED(CONFIG_EYE_DEMO_FAULT_DROP_DATA) && outgoing_active && !feedback_only) ||
	     (IS_ENABLED(CONFIG_EYE_DEMO_FAULT_DROP_FEEDBACK) &&
	      (outgoing_active || cfdp_service.entity.receiver.active) && feedback_only))) {
		fault_injected = true;
		injected_faults++;
		LOG_WRN("USLP injected on-wire %s frame drop",
			feedback_only ? "feedback-only" : "packet-bearing");
		return 0;
	}
	rc = ccsds_udp_send(&udp, frame, frame_len);
	if (rc != 0) {
		terminal_route_error = rc;
	}
	return rc;
}

static int deliver_packet(void *user, const uint8_t *packet, size_t packet_len)
{
	ARG_UNUSED(user);
	return ccsds_profile_input_dispatch_unit(&input_profile, packet, packet_len);
}

static void service_peer_ingress(uint64_t now)
{
	struct rx_datagram datagram;

	while (k_msgq_get(&rx_queue, &datagram, K_NO_WAIT) == 0) {
		atomic_dec(&rx_queue_used);
		if (link_sync_phase == LINK_SYNC_FAILED) {
			continue;
		}
		/* A delayed or reordered UDP datagram can contain an obsolete CLCW.
		 * The peer rejects and counts its out-of-window ReportValue, while the
		 * FOP remains unchanged.  Lack of subsequent acknowledgement progress
		 * is detected by the normal COP-1 retry-exhaustion path.
		 */
		(void)ccsds_uslp_peer_receive(&peer, datagram.data, datagram.length, now);
	}
}

static int submit_packet_owned(const uint8_t *packet, size_t packet_len)
{
	int rc;

	while (true) {
		process_progress_snapshots((uint64_t)k_uptime_get());
		rc = ccsds_uslp_peer_submit(&peer, packet, packet_len);
		if (rc == 0) {
			return 0;
		}
		if (rc != -ENOSPC && rc != -EAGAIN) {
			submit_terminal_errors++;
			return rc;
		}
		submit_backpressure++;
		service_peer_ingress((uint64_t)k_uptime_get());
		rc = ccsds_uslp_peer_tick(&peer, (uint64_t)k_uptime_get());
		if (rc < 0 && rc != -EAGAIN) {
			submit_terminal_errors++;
			return rc;
		}
		(void)k_sem_take(&worker_wake, K_MSEC(WORKER_IDLE_MS));
	}
}

static int send_cfdp_packet(void *user, const uint8_t *packet, size_t length)
{
	ARG_UNUSED(user);
	return submit_packet_owned(packet, length);
}

static int send_space_packet(uint16_t apid, enum ccsds_packet_type type, const uint8_t *payload,
			     size_t payload_length)
{
	uint8_t buffer[CCSDS_SPACE_PACKET_PRIMARY_HDR_LEN + DEMO_PEER_STATUS_LEN];
	size_t length;
	struct ccsds_space_packet packet = {
		.version = 0u,
		.type = type,
		.secondary_header = false,
		.apid = apid,
		.sequence_flags = CCSDS_SEQUENCE_UNSEGMENTED,
		.sequence_count = packet_sequence++ & 0x3fffu,
		.payload = payload,
		.payload_len = payload_length,
	};
	int rc = ccsds_space_packet_encode(&packet, buffer, sizeof(buffer), &length);

	return rc == 0 ? submit_packet_owned(buffer, length) : rc;
}

static int configured_ipv4(const char *text, uint32_t *address)
{
	struct in_addr parsed;
	int rc = net_addr_pton(AF_INET, text, &parsed);

	if (rc == 0) {
		*address = sys_be32_to_cpu(parsed.s_addr);
	}
	return rc;
}

static void send_peer_status(void)
{
	struct ccsds_uslp_peer_snapshot snapshot;
	uint8_t payload[DEMO_PEER_STATUS_LEN];
	struct demo_peer_status status = {
		.entity_id = CONFIG_EYE_DEMO_LOCAL_ENTITY_ID,
		.expected_peer_entity_id = CONFIG_EYE_DEMO_PEER_ENTITY_ID,
		.local_udp_port = CONFIG_EYE_DEMO_LOCAL_UDP_PORT,
		.peer_udp_port = CONFIG_EYE_DEMO_PEER_UDP_PORT,
		.cfdp_apid = CONFIG_EYE_DEMO_CFDP_APID,
		.command_apid = CONFIG_EYE_DEMO_COMMAND_APID,
		.command_status_apid = CONFIG_EYE_DEMO_COMMAND_STATUS_APID,
		.peer_status_apid = CONFIG_EYE_DEMO_PEER_STATUS_APID,
		.local_spacecraft_id = CONFIG_EYE_DEMO_LOCAL_SPACECRAFT_ID,
		.peer_spacecraft_id = CONFIG_EYE_DEMO_PEER_SPACECRAFT_ID,
		.local_source_or_destination = LOCAL_SCID_INDICATION,
		.peer_source_or_destination = PEER_SCID_INDICATION,
		.transmit_vcid = CONFIG_EYE_DEMO_TRANSMIT_VCID,
		.receive_vcid = CONFIG_EYE_DEMO_RECEIVE_VCID,
		.transmit_map_id = CONFIG_EYE_DEMO_TRANSMIT_MAP_ID,
		.receive_map_id = CONFIG_EYE_DEMO_RECEIVE_MAP_ID,
		.maximum_frame_length = CONFIG_CCSDS_MAX_FRAME_LEN,
		.cop1_window_k = CCSDS_USLP_PEER_WINDOW_K,
		.farm_window_width = CONFIG_EYE_DEMO_FARM_WINDOW_WIDTH,
		.minimum_transmit_interval_ms = CONFIG_EYE_DEMO_MINIMUM_TRANSMIT_INTERVAL_MS,
		.retransmission_timeout_ms = CONFIG_EYE_DEMO_RETRANSMISSION_TIMEOUT_MS,
		.feedback_interval_ms = CONFIG_EYE_DEMO_FEEDBACK_INTERVAL_MS,
		.transmission_limit = CONFIG_EYE_DEMO_TRANSMISSION_LIMIT,
		.initial_transmit_sequence = CONFIG_EYE_DEMO_INITIAL_TRANSMIT_SEQUENCE,
		.initial_receive_sequence = CONFIG_EYE_DEMO_INITIAL_RECEIVE_SEQUENCE,
	};

	/* Peer presence is periodic housekeeping, not transfer traffic.  Never let
	 * it queue behind an occupied Sent Queue: synchronous CFDP emission pumps
	 * CLCWs but deliberately cannot reenter the Space Packet router.
	 */
	ccsds_uslp_peer_get_snapshot(&peer, &snapshot);
	if (snapshot.outstanding_frames != 0u || snapshot.queued_packets != 0u) {
		return;
	}
	(void)configured_ipv4(CONFIG_EYE_DEMO_LOCAL_IPV4, &status.local_ipv4);
	(void)configured_ipv4(CONFIG_EYE_DEMO_PEER_IPV4, &status.peer_ipv4);
	strncpy(status.callsign, CONFIG_EYE_DEMO_CALLSIGN, sizeof(status.callsign));
	if (demo_peer_status_encode(&status, payload, sizeof(payload)) > 0) {
		(void)send_space_packet(CONFIG_EYE_DEMO_PEER_STATUS_APID, CCSDS_PACKET_TYPE_TM,
					payload, sizeof(payload));
	}
}

static void send_command_status(uint32_t request_id, enum demo_command_result result)
{
	uint8_t payload[DEMO_COMMAND_STATUS_LEN];
	const struct demo_command_status status = {
		.request_id = request_id,
		.responding_entity_id = CONFIG_EYE_DEMO_LOCAL_ENTITY_ID,
		.result = result,
	};
	int length = demo_command_status_encode(&status, payload, sizeof(payload));

	if (length > 0) {
		(void)send_space_packet(CONFIG_EYE_DEMO_COMMAND_STATUS_APID, CCSDS_PACKET_TYPE_TM,
					payload, (size_t)length);
	}
}

static int claim_send_operation(const struct send_operation *operation)
{
	if (!protocol_ready || !peer_ready) {
		return -ENETUNREACH;
	}
	if (operation_active) {
		return -EBUSY;
	}
	if (operation->destination_entity_id != CONFIG_EYE_DEMO_PEER_ENTITY_ID) {
		return -EINVAL;
	}
	active_operation = *operation;
	operation_active = true;
	return 0;
}

static int execute_send_operation(void)
{
	const struct send_operation *operation = &active_operation;
	struct demo_image_slot *staging = NULL;
	ccsds_cfdp_transaction_id_t transaction_id;
	const ccsds_cfdp_put_request_t request = {
		.source_path = DEMO_IMAGE_SOURCE_PATH,
		.destination_path = DEMO_IMAGE_DEST_PATH,
		.checksum_type = CCSDS_CFDP_CHECKSUM_TYPE_MODULAR,
		.closure_requested = true,
		.acknowledged_mode = true,
	};
	enum ccsds_cfdp_status status;
	int rc;

	ui_event(DEMO_UI_CAPTURING, operation->request_id, 0);
	k_sleep(K_MSEC(PRE_CAPTURE_QUIET_PERIOD_MS));
	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_store_claim_staging(&image_store, &staging);
	if (rc != 0) {
		LOG_ERR("image staging claim failed: %d owners=%02x,%02x,%02x", rc,
			image_slots[0].owners, image_slots[1].owners, image_slots[2].owners);
	}
	k_mutex_unlock(&image_lock);
	if (rc == 0) {
		rc = camera_status != 0
			     ? camera_status
			     : demo_camera_acquire_object(&camera_adapter, staging->object);
		if (rc != 0) {
			LOG_ERR("camera acquisition failed: %d", rc);
		}
	}
	if (rc != 0) {
		if (staging != NULL) {
			k_mutex_lock(&image_lock, K_FOREVER);
			(void)demo_image_store_abort_staging(&image_store, staging);
			k_mutex_unlock(&image_lock);
		}
		operation_active = false;
		ui_event(DEMO_UI_FAILED, operation->request_id, rc);
		return rc;
	}
	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_store_promote(&image_store, staging);
	if (rc == 0) {
		rc = demo_image_store_retain_tx(&image_store, staging);
	}
	if (rc == 0) {
		rc = demo_image_source_bind(&image_source, staging);
	}
	if (rc == 0) {
		tx_slot = staging;
	}
	if (rc != 0 && (staging->owners & DEMO_IMAGE_OWNER_TX) != 0u) {
		(void)demo_image_store_release_tx(&image_store, staging);
	}
	k_mutex_unlock(&image_lock);
	if (rc != 0) {
		operation_active = false;
		ui_event(DEMO_UI_FAILED, operation->request_id, rc);
		return rc;
	}
	LOG_INF("local image ready request=%u", operation->request_id);
	k_sleep(K_MSEC(POST_CAPTURE_QUIET_PERIOD_MS));
	outgoing_active = true;
	ui_event(DEMO_UI_CFDP_TX, operation->request_id, 0);
	status =
		ccsds_cfdp_service_send_file(&cfdp_service, &source_ops, &request, &transaction_id);
	if (status != CCSDS_CFDP_STATUS_OK) {
		k_mutex_lock(&image_lock, K_FOREVER);
		if (tx_slot != NULL) {
			__ASSERT_NO_MSG(!image_source.open);
			demo_image_source_unbind(&image_source);
			(void)demo_image_store_release_tx(&image_store, tx_slot);
			tx_slot = NULL;
		}
		k_mutex_unlock(&image_lock);
		operation_active = false;
		outgoing_active = false;
		ui_event(DEMO_UI_FAILED, operation->request_id, status);
		return -EIO;
	}
	return 0;
}

static int start_send_operation(const struct send_operation *operation)
{
	int rc = claim_send_operation(operation);

	return rc == 0 ? execute_send_operation() : rc;
}

static void process_capture_command(const struct router_message *message, uint64_t now)
{
	struct demo_capture_command command = {0};
	struct send_operation operation;
	enum demo_command_result result;
	uint32_t response_request_id = 1u;

	ui_event(DEMO_UI_TC_RX, 0u, 0);
	if (message->length >= 8u && sys_get_be32(&message->payload[4]) != 0u) {
		response_request_id = sys_get_be32(&message->payload[4]);
	}
	if (demo_capture_command_decode(message->payload, message->length, &command) != 0 ||
	    command.requesting_entity_id != CONFIG_EYE_DEMO_PEER_ENTITY_ID) {
		result = DEMO_COMMAND_INVALID;
	} else if (demo_dedup_check_and_record(&dedup_cache, command.requesting_entity_id,
					       command.request_id, now, REQUEST_RETENTION_MS)) {
		result = DEMO_COMMAND_DUPLICATE;
	} else if (operation_active || !peer_ready) {
		result = DEMO_COMMAND_BUSY;
	} else {
		operation = (struct send_operation){
			.destination_entity_id = command.requesting_entity_id,
			.origin = OPERATION_REMOTE_COMMAND,
			.request_id = command.request_id,
			.has_request_id = true,
		};
		if (claim_send_operation(&operation) == 0) {
			send_command_status(command.request_id == 0u ? response_request_id
								     : command.request_id,
					    DEMO_COMMAND_ACCEPTED);
			(void)execute_send_operation();
			return;
		}
		result = DEMO_COMMAND_BUSY;
	}
	send_command_status(command.request_id == 0u ? response_request_id : command.request_id,
			    result);
}

static void process_command_status(const struct router_message *message)
{
	struct demo_command_status status;

	if (demo_command_status_decode(message->payload, message->length, &status) != 0 ||
	    status.responding_entity_id != CONFIG_EYE_DEMO_PEER_ENTITY_ID) {
		return;
	}
	if (status.request_id == pending_request_id) {
		pending_request_deadline_ms = 0u;
	}
	ui_event(DEMO_UI_COMMAND_RESULT, status.request_id, status.result);
}

static void process_peer_status(const struct router_message *message, uint64_t now)
{
	struct demo_peer_status status;
	struct demo_peer_expectation expected = {
		.local_entity_id = CONFIG_EYE_DEMO_LOCAL_ENTITY_ID,
		.peer_entity_id = CONFIG_EYE_DEMO_PEER_ENTITY_ID,
		.local_udp_port = CONFIG_EYE_DEMO_LOCAL_UDP_PORT,
		.peer_udp_port = CONFIG_EYE_DEMO_PEER_UDP_PORT,
		.cfdp_apid = CONFIG_EYE_DEMO_CFDP_APID,
		.command_apid = CONFIG_EYE_DEMO_COMMAND_APID,
		.command_status_apid = CONFIG_EYE_DEMO_COMMAND_STATUS_APID,
		.peer_status_apid = CONFIG_EYE_DEMO_PEER_STATUS_APID,
		.local_spacecraft_id = CONFIG_EYE_DEMO_LOCAL_SPACECRAFT_ID,
		.peer_spacecraft_id = CONFIG_EYE_DEMO_PEER_SPACECRAFT_ID,
		.local_source_or_destination = LOCAL_SCID_INDICATION,
		.peer_source_or_destination = PEER_SCID_INDICATION,
		.transmit_vcid = CONFIG_EYE_DEMO_TRANSMIT_VCID,
		.receive_vcid = CONFIG_EYE_DEMO_RECEIVE_VCID,
		.transmit_map_id = CONFIG_EYE_DEMO_TRANSMIT_MAP_ID,
		.receive_map_id = CONFIG_EYE_DEMO_RECEIVE_MAP_ID,
		.maximum_frame_length = CONFIG_CCSDS_MAX_FRAME_LEN,
		.cop1_window_k = CCSDS_USLP_PEER_WINDOW_K,
		.farm_window_width = CONFIG_EYE_DEMO_FARM_WINDOW_WIDTH,
		.minimum_transmit_interval_ms = CONFIG_EYE_DEMO_MINIMUM_TRANSMIT_INTERVAL_MS,
		.retransmission_timeout_ms = CONFIG_EYE_DEMO_RETRANSMISSION_TIMEOUT_MS,
		.feedback_interval_ms = CONFIG_EYE_DEMO_FEEDBACK_INTERVAL_MS,
		.transmission_limit = CONFIG_EYE_DEMO_TRANSMISSION_LIMIT,
		.initial_transmit_sequence = CONFIG_EYE_DEMO_INITIAL_TRANSMIT_SEQUENCE,
		.initial_receive_sequence = CONFIG_EYE_DEMO_INITIAL_RECEIVE_SEQUENCE,
	};
	enum demo_peer_validation validation;

	(void)configured_ipv4(CONFIG_EYE_DEMO_LOCAL_IPV4, &expected.local_ipv4);
	(void)configured_ipv4(CONFIG_EYE_DEMO_PEER_IPV4, &expected.peer_ipv4);
	strncpy(expected.peer_callsign, CONFIG_EYE_DEMO_PEER_CALLSIGN,
		sizeof(expected.peer_callsign));
	if (demo_peer_status_decode(message->payload, message->length, &status) != 0) {
		ui_event(DEMO_UI_PEER_INVALID, 0u, DEMO_PEER_INCOMPATIBLE);
		return;
	}
	validation = demo_peer_status_validate(&status, &expected);
	if (validation == DEMO_PEER_VALID) {
		if (link_sync_phase == LINK_SYNC_FAILED) {
			return;
		}
		peer_last_seen_ms = now;
		if (!peer_ready) {
			peer_ready = true;
			initial_link_acquisition = false;
			ui_event(DEMO_UI_PEER_READY, 0u, 0);
		}
	} else {
		peer_ready = false;
		ui_event(DEMO_UI_PEER_INVALID, 0u, validation);
	}
}

static void process_router_messages(uint64_t now)
{
	struct router_message message;

	while (k_msgq_get(&router_queue, &message, K_NO_WAIT) == 0) {
		if (message.type != ROUTER_PEER_STATUS) {
			peer_last_seen_ms = now;
		}
		switch (message.type) {
		case ROUTER_CFDP: {
			ccsds_cfdp_pdu_header_t header;
			size_t header_length = 0u;
			enum ccsds_cfdp_status status;
			bool starts_receive;

			pending_request_deadline_ms = 0u;

			starts_receive =
				ccsds_cfdp_decode_header(message.payload, message.length, &header,
							 &header_length) == CCSDS_CFDP_STATUS_OK &&
				header.direction == CCSDS_CFDP_DIRECTION_TOWARD_RECEIVER &&
				header.pdu_type == CCSDS_CFDP_PDU_TYPE_FILE_DIRECTIVE &&
				header_length < message.length &&
				message.payload[header_length] == CCSDS_CFDP_DIRECTIVE_METADATA;
			if (!cfdp_service.entity.receiver.active && starts_receive) {
				ui_event(DEMO_UI_CFDP_RX, 0u, 0);
			}
			status = ccsds_cfdp_entity_receive_pdu(&cfdp_service.entity, &receive_ops,
							       message.payload, message.length);
			if (status != CCSDS_CFDP_STATUS_OK) {
				LOG_WRN("CFDP input rejected status=%d sender_active=%u "
					"receiver_active=%u",
					status, cfdp_service.entity.sender.active,
					cfdp_service.entity.receiver.active);
			}
			break;
		}
		case ROUTER_CAPTURE_COMMAND:
			process_capture_command(&message, now);
			break;
		case ROUTER_COMMAND_STATUS:
			process_command_status(&message);
			break;
		case ROUTER_PEER_STATUS:
			process_peer_status(&message, now);
			break;
		}
	}
}

static void flush_progress(struct progress_update *progress, enum demo_transfer_direction direction,
			   uint64_t now, bool force)
{
	if (!progress->pending || (!force && now < progress->next_ui_ms)) {
		return;
	}

	ui_progress_event(direction, progress);
	progress->pending = false;
	progress->next_ui_ms = now + PROGRESS_UI_PERIOD_MS;
}

static void process_progress_snapshots(uint64_t now)
{
	struct callback_progress_snapshot snapshots[ARRAY_SIZE(callback_progress)];
	k_spinlock_key_t key = k_spin_lock(&callback_progress_lock);

	memcpy(snapshots, callback_progress, sizeof(snapshots));
	for (size_t i = 0; i < ARRAY_SIZE(callback_progress); ++i) {
		callback_progress[i].pending = false;
		callback_progress[i].recovery_activity = false;
	}
	k_spin_unlock(&callback_progress_lock, key);

	for (size_t i = 0; i < ARRAY_SIZE(snapshots); ++i) {
		struct progress_update *progress = i == 0u ? &tx_progress : &rx_progress;
		enum demo_transfer_direction direction =
			i == 0u ? DEMO_TRANSFER_TX : DEMO_TRANSFER_RX;

		if (!snapshots[i].pending) {
			continue;
		}
		progress->bytes_transferred = snapshots[i].bytes_transferred;
		progress->file_size = snapshots[i].file_size;
		progress->recovery_activity = snapshots[i].recovery_activity;
		progress->pending = true;
		flush_progress(progress, direction, now, false);
	}
}

static void process_cfdp_events(uint64_t now)
{
	struct callback_terminal_snapshot snapshots[ARRAY_SIZE(callback_terminal)];
	k_spinlock_key_t key;

	process_progress_snapshots(now);
	key = k_spin_lock(&callback_progress_lock);
	memcpy(snapshots, callback_terminal, sizeof(snapshots));
	for (size_t i = 0; i < ARRAY_SIZE(callback_terminal); ++i) {
		callback_terminal[i].pending = false;
	}
	k_spin_unlock(&callback_progress_lock, key);

	for (size_t i = 0; i < ARRAY_SIZE(snapshots); ++i) {
		if (!snapshots[i].pending) {
			continue;
		}
		const ccsds_cfdp_event_t *event = &snapshots[i].event;
		struct progress_update *progress = event->direction == CCSDS_CFDP_DIRECTION_SENDER
							   ? &tx_progress
							   : &rx_progress;
		enum demo_transfer_direction direction =
			event->direction == CCSDS_CFDP_DIRECTION_SENDER ? DEMO_TRANSFER_TX
									: DEMO_TRANSFER_RX;

		{
			bool sender = event->direction == CCSDS_CFDP_DIRECTION_SENDER;
			bool success = event->type == CCSDS_CFDP_EVENT_COMPLETE &&
				       event->status == CCSDS_CFDP_STATUS_OK;
			uint32_t request_id = sender ? active_operation.request_id : 0u;
			int image_rc = 0;

			progress->bytes_transferred = event->bytes_transferred;
			progress->file_size = event->file_size;
			progress->recovery_activity = false;
			progress->pending = true;
			flush_progress(progress, direction, now, true);

			if (sender) {
				int release_rc = 0;

				k_mutex_lock(&image_lock, K_FOREVER);
				if (tx_slot != NULL) {
					__ASSERT_NO_MSG(!image_source.open);
					demo_image_source_unbind(&image_source);
					release_rc =
						demo_image_store_release_tx(&image_store, tx_slot);
					__ASSERT_NO_MSG(release_rc == 0);
					if (release_rc == 0) {
						tx_slot = NULL;
					}
				}
				k_mutex_unlock(&image_lock);
				if (release_rc != 0) {
					LOG_ERR("CFDP TX image release failed: %d", release_rc);
				}
			} else {
				k_mutex_lock(&image_lock, K_FOREVER);
				image_rc = demo_image_receiver_terminal(&image_receiver, success);
				k_mutex_unlock(&image_lock);
				success = success && image_rc == 0;
				if (success) {
					validated_image_commits++;
				}
			}
			LOG_INF("CFDP %s terminal event=%u status=%d transaction=%llu:%llu "
				"bytes=%u file=%u",
				sender ? "TX" : "RX", event->type, event->status,
				(unsigned long long)event->transaction_id.source_entity_id,
				(unsigned long long)
					event->transaction_id.transaction_sequence_number,
				event->bytes_transferred, event->file_size);
			if (!sender) {
				LOG_INF("CFDP RX validation=%s image_status=%d commits=%u "
					"ingress_drops=%ld",
					success ? "OK" : "FAILED", image_rc,
					validated_image_commits, atomic_get(&rx_queue_drops));
			}
			trace_link_snapshot(success ? "cfdp-complete" : "cfdp-failed");
			if (sender && active_operation.has_request_id && !success &&
			    event->status == CCSDS_CFDP_STATUS_INACTIVITY_DETECTED) {
				send_command_status(active_operation.request_id,
						    DEMO_COMMAND_TIMED_OUT);
			}
			ui_event(success ? DEMO_UI_COMPLETE : DEMO_UI_FAILED, request_id,
				 sender ? DEMO_TRANSFER_TX : DEMO_TRANSFER_RX);
			if (sender) {
				operation_active = false;
				outgoing_active = false;
			}
		}
	}

	flush_progress(&tx_progress, DEMO_TRANSFER_TX, now, false);
	flush_progress(&rx_progress, DEMO_TRANSFER_RX, now, false);
}

static int process_link_action(enum action_type action, uint64_t now)
{
	struct ccsds_uslp_peer_snapshot snapshot;
	int rc;

	if (!protocol_ready || operation_active || cfdp_service.entity.receiver.active ||
	    pending_request_deadline_ms != 0u) {
		return -EBUSY;
	}
	ccsds_uslp_peer_get_snapshot(&peer, &snapshot);
	if ((snapshot.outstanding_frames != 0u || snapshot.queued_packets != 0u ||
	     atomic_get(&rx_queue_used) != 0) &&
	    !snapshot.terminal_failure && link_sync_phase != LINK_SYNC_FAILED) {
		return -EBUSY;
	}
	if (action == ACTION_LINK_SYNC_TX) {
		rc = start_link_sync(now, true);
	} else {
		if (snapshot.terminal_failure || link_sync_phase == LINK_SYNC_FAILED) {
			struct ccsds_uslp_peer_config config = peer_config;

			config.initial_transmit_sequence = snapshot.transmit_sequence;
			config.initial_receive_sequence = snapshot.receive_sequence;
			rc = ccsds_uslp_peer_init(&peer, &config, now);
			if (rc != 0) {
				return rc;
			}
		}
		if (action == ACTION_LINK_UNLOCK) {
			rc = ccsds_uslp_peer_send_unlock(&peer);
		} else {
			rc = ccsds_uslp_peer_send_set_vr(&peer, snapshot.transmit_sequence);
		}
		if (rc == 0) {
			link_sync_phase = LINK_SYNC_UNLOCK;
		}
	}
	if (rc == 0) {
		peer_ready = false;
		peak_outstanding = 0u;
		LOG_WRN("USLP operator action=%u queued at V(S)=%u report=%u",
			(unsigned int)action, snapshot.transmit_sequence,
			snapshot.last_clcw_report);
	}
	return rc;
}

static void process_actions(uint64_t now)
{
	struct action_message action;

	while (k_msgq_get(&action_queue, &action, K_NO_WAIT) == 0) {
		if (action.type == ACTION_LINK_UNLOCK || action.type == ACTION_LINK_SYNC_TX ||
		    action.type == ACTION_LINK_SET_VR) {
			enum demo_link_action ui_action =
				action.type == ACTION_LINK_UNLOCK
					? DEMO_LINK_UNLOCK
					: (action.type == ACTION_LINK_SYNC_TX ? DEMO_LINK_SYNC_TX
									 : DEMO_LINK_SET_VR);
			int rc = process_link_action(action.type, now);

			ui_event(DEMO_UI_LINK_ACTION_RESULT, (uint32_t)ui_action, rc);
			publish_link_snapshot();
		} else if (action.type == ACTION_LOCAL_SEND) {
			const struct send_operation operation = {
				.destination_entity_id = CONFIG_EYE_DEMO_PEER_ENTITY_ID,
				.origin = OPERATION_LOCAL_BUTTON,
			};

			int rc = start_send_operation(&operation);

			if (rc == -EBUSY) {
				ui_event(DEMO_UI_BUSY, 0u, -EBUSY);
			} else if (rc != 0) {
				ui_event(link_sync_phase == LINK_SYNC_FAILED
						 ? DEMO_UI_LINK_FAILED
						 : DEMO_UI_PEER_ABSENT,
					 0u, rc);
			}
		} else {
			uint8_t payload[DEMO_CAPTURE_COMMAND_LEN];
			struct demo_capture_command command = {
				.request_id = next_request_id++,
				.requesting_entity_id = CONFIG_EYE_DEMO_LOCAL_ENTITY_ID,
			};
			int length;

			if (next_request_id == 0u) {
				next_request_id = 1u;
			}
			length = demo_capture_command_encode(&command, payload, sizeof(payload));
			if (!peer_ready) {
				ui_event(link_sync_phase == LINK_SYNC_FAILED
						 ? DEMO_UI_LINK_FAILED
						 : DEMO_UI_PEER_ABSENT,
					 command.request_id, -ENETUNREACH);
			} else if (length < 0) {
				ui_event(DEMO_UI_FAILED, command.request_id, length);
			} else if (send_space_packet(CONFIG_EYE_DEMO_COMMAND_APID,
						     CCSDS_PACKET_TYPE_TC, payload,
						     (size_t)length) != 0) {
				ui_event(DEMO_UI_PEER_ABSENT, command.request_id, -ENETUNREACH);
			} else {
				pending_request_id = command.request_id;
				pending_request_deadline_ms = now + COMMAND_TIMEOUT_MS;
				ui_event(DEMO_UI_TC_TX, command.request_id, 0);
			}
		}
	}
}

static void trace_receiver_closure(void)
{
	const bool waiting = cfdp_service.entity.receiver.active &&
			     cfdp_service.entity.receiver.waiting_for_finished_ack;
	const uint32_t retries = cfdp_service.entity.receiver.retry_count;

	if (waiting && (!receiver_wait_logged || retries != receiver_retry_logged)) {
		LOG_INF("CFDP RX waiting for ACK(Finished), retry=%u", retries);
	}
	receiver_wait_logged = waiting;
	receiver_retry_logged = retries;
}

static void publish_link_snapshot(void)
{
	struct ccsds_uslp_peer_snapshot peer_state;
	struct ccsds_udp_stats udp_stats;
	struct demo_link_snapshot next;
	k_spinlock_key_t key;

	ccsds_uslp_peer_get_snapshot(&peer, &peer_state);
	ccsds_udp_get_stats(&udp, &udp_stats);
	next = (struct demo_link_snapshot){
		.new_frames = peer_state.stats.new_frames_emitted,
		.retransmitted_frames = peer_state.stats.retransmitted_frames,
		.packet_frames = peer_state.stats.packet_frames_emitted,
		.feedback_frames = peer_state.stats.feedback_frames_emitted,
		.received_frames = peer_state.stats.received_frames_accepted,
		.duplicate_frames = peer_state.stats.received_frames_duplicated,
		.rejected_frames = peer_state.stats.received_frames_rejected,
		.dispatched_packets = peer_state.stats.packets_dispatched,
		.clcws = peer_state.stats.clcws_accepted,
		.acknowledgements = peer_state.stats.acknowledgements,
		.timeout_events = peer_state.stats.timeout_events,
		.retry_exhaustion = peer_state.stats.retry_exhaustion,
		.wait_events = peer_state.stats.wait_events,
		.lockout_events = peer_state.stats.lockout_events,
		.terminal_failures = peer_state.stats.terminal_failures,
		.submit_backpressure = submit_backpressure,
		.cfdp_naks_sent = cfdp_naks_sent,
		.cfdp_naks_received = cfdp_naks_received,
		.cfdp_retransmissions = cfdp_retransmissions,
		.route_failures = peer_state.stats.route_failures,
		.fop_state = (uint8_t)peer_state.fop_state,
		.farm_state = (uint8_t)peer_state.farm_state,
		.transmit_sequence = peer_state.transmit_sequence,
		.expected_acknowledgement = peer_state.expected_acknowledgement,
		.receive_sequence = peer_state.receive_sequence,
		.report_value = peer_state.last_clcw_report,
		.report_advance = (uint8_t)peer_state.last_clcw_advance,
		.transmission_count = peer_state.transmission_count,
		.outstanding_frames = (uint8_t)peer_state.outstanding_frames,
		.peak_outstanding = (uint8_t)peak_outstanding,
		.ingress_used = (uint8_t)atomic_get(&rx_queue_used),
		.ingress_peak = (uint8_t)atomic_get(&rx_queue_peak),
		.ingress_capacity = RX_QUEUE_DEPTH,
		.peer_available = peer_ready,
		.cfdp_tx_active = cfdp_service.entity.sender.active,
		.cfdp_rx_active = cfdp_service.entity.receiver.active,
		.terminal_failure = peer_state.terminal_failure,
		.peer_error = peer_state.stats.last_error,
		.route_error = terminal_route_error,
		.udp_error = udp_stats.last_error,
		.ingress_overflow = (uint32_t)atomic_get(&rx_queue_drops),
		.injected_faults = injected_faults,
		.submit_terminal_errors = submit_terminal_errors,
	};
	key = k_spin_lock(&link_snapshot_lock);
	link_snapshot = next;
	link_snapshot_valid = true;
	k_spin_unlock(&link_snapshot_lock, key);
}

static void trace_link_snapshot(const char *reason)
{
	struct ccsds_uslp_peer_snapshot snapshot;
	struct ccsds_udp_stats udp_stats;

	ccsds_uslp_peer_get_snapshot(&peer, &snapshot);
	ccsds_udp_get_stats(&udp, &udp_stats);
	peak_outstanding = MAX(peak_outstanding, snapshot.outstanding_frames);
	LOG_INF("USLP LINK reason=%s role=%s tx=%04x/%u/%u/%u rx=%04x/%u/%u/%u "
		"fop=%u farm=%u new=%u retx=%u packet=%u feedback=%u",
		reason, CONFIG_EYE_DEMO_CALLSIGN, CONFIG_EYE_DEMO_LOCAL_SPACECRAFT_ID,
		LOCAL_SCID_INDICATION, CONFIG_EYE_DEMO_TRANSMIT_VCID,
		CONFIG_EYE_DEMO_TRANSMIT_MAP_ID, CONFIG_EYE_DEMO_PEER_SPACECRAFT_ID,
		PEER_SCID_INDICATION, CONFIG_EYE_DEMO_RECEIVE_VCID, CONFIG_EYE_DEMO_RECEIVE_MAP_ID,
		snapshot.fop_state, snapshot.farm_state, snapshot.stats.new_frames_emitted,
		snapshot.stats.retransmitted_frames, snapshot.stats.packet_frames_emitted,
		snapshot.stats.feedback_frames_emitted);
	LOG_INF("USLP LINK rx_accept=%u duplicate=%u rejected=%u dispatched=%u clcw=%u "
		"ack=%u vs=%u nnr=%u vr=%u report=%u advance=%u tx_count=%u terminal=%u "
		"timeout=%u exhausted=%u window=%u peak=%u ingress=%ld/%ld/%u "
		"backpressure=%u overflow=%ld injected=%u",
		snapshot.stats.received_frames_accepted, snapshot.stats.received_frames_duplicated,
		snapshot.stats.received_frames_rejected, snapshot.stats.packets_dispatched,
		snapshot.stats.clcws_accepted, snapshot.stats.acknowledgements,
		snapshot.transmit_sequence, snapshot.expected_acknowledgement,
		snapshot.receive_sequence, snapshot.last_clcw_report,
		(unsigned int)snapshot.last_clcw_advance, snapshot.transmission_count,
		snapshot.terminal_failure, snapshot.stats.timeout_events,
		snapshot.stats.retry_exhaustion, (unsigned int)snapshot.outstanding_frames,
		(unsigned int)peak_outstanding, atomic_get(&rx_queue_used),
		atomic_get(&rx_queue_peak), RX_QUEUE_DEPTH, submit_backpressure,
		atomic_get(&rx_queue_drops), injected_faults);
	LOG_INF("USLP LINK cfdp_nak_tx=%u cfdp_nak_rx=%u cfdp_retx=%u route_fail=%u "
		"peer_error=%d route_error=%d udp_error=%d submit_terminal=%u",
		cfdp_naks_sent, cfdp_naks_received, cfdp_retransmissions,
		snapshot.stats.route_failures, snapshot.stats.last_error, terminal_route_error,
		udp_stats.last_error, submit_terminal_errors);
}

static int apply_static_ipv4(struct net_if *iface)
{
	struct in_addr address;
	struct in_addr netmask;
	struct in_addr gateway;

	if (net_addr_pton(AF_INET, CONFIG_EYE_DEMO_LOCAL_IPV4, &address) != 0 ||
	    net_addr_pton(AF_INET, CONFIG_EYE_DEMO_NETMASK, &netmask) != 0 ||
	    net_addr_pton(AF_INET, CONFIG_EYE_DEMO_GATEWAY, &gateway) != 0) {
		return -EINVAL;
	}
	if (net_if_ipv4_addr_add(iface, &address, NET_ADDR_MANUAL, 0) == NULL ||
	    !net_if_ipv4_set_netmask_by_addr(iface, &address, &netmask)) {
		return -EADDRNOTAVAIL;
	}
	net_if_ipv4_set_gw(iface, &gateway);
	return 0;
}

static int initialize_protocol(void)
{
	struct ccsds_udp_config udp_config = {
		.local_ip = CONFIG_EYE_DEMO_LOCAL_IPV4,
		.local_port = CONFIG_EYE_DEMO_LOCAL_UDP_PORT,
		.peer_ip = CONFIG_EYE_DEMO_PEER_IPV4,
		.peer_port = CONFIG_EYE_DEMO_PEER_UDP_PORT,
		.max_unit_len = CONFIG_CCSDS_UDP_MAX_UNIT_LEN,
		.thread_priority = 11,
		.thread_name = "eye_udp",
		.receive = udp_receive,
	};
	peer_config = (struct ccsds_uslp_peer_config){
		.transmit_channel =
			{
				.spacecraft_id = CONFIG_EYE_DEMO_LOCAL_SPACECRAFT_ID,
				.source_or_destination = LOCAL_SCID_INDICATION,
				.virtual_channel_id = CONFIG_EYE_DEMO_TRANSMIT_VCID,
				.map_id = CONFIG_EYE_DEMO_TRANSMIT_MAP_ID,
			},
		.receive_channel =
			{
				.spacecraft_id = CONFIG_EYE_DEMO_PEER_SPACECRAFT_ID,
				.source_or_destination = PEER_SCID_INDICATION,
				.virtual_channel_id = CONFIG_EYE_DEMO_RECEIVE_VCID,
				.map_id = CONFIG_EYE_DEMO_RECEIVE_MAP_ID,
			},
		.storage =
			{
				.packet_queue = &peer_packet_queue[0][0],
				.packet_lengths = peer_packet_lengths,
				.packet_capacity = PEER_PACKET_CAPACITY,
				.packet_slots = PEER_PACKET_SLOTS,
				.sent_frames = &peer_sent_frames[0][0],
				.sent_lengths = peer_sent_lengths,
				.frame_capacity = CONFIG_CCSDS_MAX_FRAME_LEN,
				.sent_slots = CCSDS_USLP_PEER_WINDOW_K,
			},
		.route = route_frame,
		.deliver_packet = deliver_packet,
		.retransmission_timeout_ms = CONFIG_EYE_DEMO_RETRANSMISSION_TIMEOUT_MS,
		.feedback_interval_ms = CONFIG_EYE_DEMO_FEEDBACK_INTERVAL_MS,
		.minimum_transmit_interval_ms = CONFIG_EYE_DEMO_MINIMUM_TRANSMIT_INTERVAL_MS,
		.transmission_limit = CONFIG_EYE_DEMO_TRANSMISSION_LIMIT,
		.farm_window_width = CONFIG_EYE_DEMO_FARM_WINDOW_WIDTH,
		.initial_transmit_sequence = CONFIG_EYE_DEMO_INITIAL_TRANSMIT_SEQUENCE,
		.initial_receive_sequence = CONFIG_EYE_DEMO_INITIAL_RECEIVE_SEQUENCE,
	};
	struct ccsds_cfdp_service_config cfdp_config = {
		.local_entity_id = CONFIG_EYE_DEMO_LOCAL_ENTITY_ID,
		.remote_entity_id = CONFIG_EYE_DEMO_PEER_ENTITY_ID,
		.entity_id_len = 1u,
		.transaction_sequence_number_len = 2u,
		.initial_transaction_sequence_number = 1u,
		.local_apid = CONFIG_EYE_DEMO_CFDP_APID,
		.remote_apid = CONFIG_EYE_DEMO_CFDP_APID,
		.packet_type = CCSDS_PACKET_TYPE_TC,
		.send_packet = send_cfdp_packet,
		.send_user = NULL,
		.now_ms = now_ms,
		.receive_filestore = &receive_ops,
		.event_cb = cfdp_event_callback,
	};
	int rc;

	ccsds_router_init(&router);
	ccsds_profile_input_init(&input_profile, &router, NULL);
	if (ccsds_udp_init(&udp, &udp_config) != 0 ||
	    start_link_sync((uint64_t)k_uptime_get(), false) != 0 ||
	    ccsds_cfdp_service_init(&cfdp_service, &cfdp_config) != CCSDS_CFDP_STATUS_OK) {
		return -EINVAL;
	}
	rc = ccsds_router_register_apid(&router, CONFIG_EYE_DEMO_CFDP_APID, cfdp_router_handler,
					NULL);
	rc = rc == 0 ? ccsds_router_register_apid(&router, CONFIG_EYE_DEMO_COMMAND_APID,
						  command_router_handler, NULL)
		     : rc;
	rc = rc == 0 ? ccsds_router_register_apid(&router, CONFIG_EYE_DEMO_COMMAND_STATUS_APID,
						  command_status_router_handler, NULL)
		     : rc;
	rc = rc == 0 ? ccsds_router_register_apid(&router, CONFIG_EYE_DEMO_PEER_STATUS_APID,
						  peer_status_router_handler, NULL)
		     : rc;
	return rc == 0 ? ccsds_udp_start(&udp) : rc;
}

static void wifi_event_handler(struct net_mgmt_event_callback *callback, uint64_t event,
			       struct net_if *iface)
{
	const struct wifi_status *status = callback->info;
	struct network_event_message message;

	ARG_UNUSED(iface);
	message.type =
		event == NET_EVENT_WIFI_CONNECT_RESULT && status != NULL && status->status == 0
			? NETWORK_ASSOCIATED
			: NETWORK_DISCONNECTED;
	if (k_msgq_put(&network_event_queue, &message, K_NO_WAIT) == 0) {
		k_sem_give(&worker_wake);
	}
}

static void worker(void *p1, void *p2, void *p3)
{
	struct wifi_connect_req_params connect = {
		.ssid = (const uint8_t *)CONFIG_EYE_DEMO_WIFI_SSID,
		.ssid_length = sizeof(CONFIG_EYE_DEMO_WIFI_SSID) - 1u,
		.psk = (const uint8_t *)CONFIG_EYE_DEMO_WIFI_PASSWORD,
		.psk_length = sizeof(CONFIG_EYE_DEMO_WIFI_PASSWORD) - 1u,
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};
	bool association_handled = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	camera_status = demo_camera_init(&camera_adapter);
	if (camera_status != 0) {
		LOG_ERR("camera initialization failed: %d", camera_status);
	}
	ui_event(DEMO_UI_NETWORK_CONNECTING, 0u, 0);
	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &connect, sizeof(connect)) != 0) {
		ui_event(DEMO_UI_NETWORK_FAILED, 0u, -EIO);
		return;
	}

	while (true) {
		struct network_event_message network_event;
		uint64_t now = (uint64_t)k_uptime_get();

		while (k_msgq_get(&network_event_queue, &network_event, K_NO_WAIT) == 0) {
			if (network_event.type == NETWORK_ASSOCIATED && !association_handled) {
				association_handled = true;
				if (apply_static_ipv4(wifi_iface) == 0 &&
				    (protocol_initialized || initialize_protocol() == 0)) {
					protocol_initialized = true;
					protocol_ready = true;
					next_status_ms = now;
					next_cfdp_poll_ms = now;
					next_link_snapshot_ms = now;
					next_link_ui_snapshot_ms = now;
					ui_event(DEMO_UI_NETWORK_READY, 0u, 0);
					LOG_INF("network ready %s:%u peer %s:%u",
						CONFIG_EYE_DEMO_LOCAL_IPV4,
						CONFIG_EYE_DEMO_LOCAL_UDP_PORT,
						CONFIG_EYE_DEMO_PEER_IPV4,
						CONFIG_EYE_DEMO_PEER_UDP_PORT);
					LOG_INF("USLP profile frame=%u packet_slots=%u "
						"sent_slots=%u "
						"ingress_slots=%u K=%u pacing=%u ms burst=%u "
						"fault=%s",
						CONFIG_CCSDS_MAX_FRAME_LEN, PEER_PACKET_SLOTS,
						CCSDS_USLP_PEER_WINDOW_K, RX_QUEUE_DEPTH,
						CCSDS_USLP_PEER_WINDOW_K,
						CONFIG_EYE_DEMO_MINIMUM_TRANSMIT_INTERVAL_MS,
						CFDP_MAX_SYNCHRONOUS_PACKETS,
						IS_ENABLED(CONFIG_EYE_DEMO_FAULT_DROP_DATA)
							? "drop-data"
							: (IS_ENABLED(
								   CONFIG_EYE_DEMO_FAULT_DROP_FEEDBACK)
								   ? "drop-feedback"
								   : (IS_ENABLED(
									      CONFIG_EYE_DEMO_FAULT_WITHHOLD_FEEDBACK)
									      ? "withhold-feedback"
									      : "none")));
				} else {
					ui_event(DEMO_UI_NETWORK_FAILED, 0u, -EADDRNOTAVAIL);
				}
			} else if (network_event.type == NETWORK_DISCONNECTED) {
				association_handled = false;
				protocol_ready = false;
				peer_ready = false;
				ui_event(DEMO_UI_NETWORK_FAILED, 0u, -ENETDOWN);
			}
		}
		service_peer_ingress(now);
		process_router_messages(now);
		ccsds_uslp_peer_receiver_ready(&peer);
		process_cfdp_events(now);
		if (protocol_ready) {
			process_actions(now);
			if (link_sync_phase != LINK_SYNC_FAILED) {
				int peer_rc = ccsds_uslp_peer_tick(&peer, now);
				struct ccsds_uslp_peer_snapshot snapshot;

				ccsds_uslp_peer_get_snapshot(&peer, &snapshot);
				(void)advance_link_sync(now, peer_rc, &snapshot);
				ccsds_uslp_peer_get_snapshot(&peer, &snapshot);
				peak_outstanding =
					MAX(peak_outstanding, snapshot.outstanding_frames);
			}
			if (now >= next_cfdp_poll_ms) {
				ccsds_cfdp_service_poll(&cfdp_service, now);
				next_cfdp_poll_ms = now + CFDP_POLL_MS;
			}
			trace_receiver_closure();
			if (link_sync_phase == LINK_SYNC_READY && now >= next_status_ms) {
				if (!operation_active && !cfdp_service.entity.receiver.active) {
					send_peer_status();
				}
				next_status_ms = now + STATUS_PERIOD_MS;
			}
			if (peer_ready && !operation_active &&
			    !cfdp_service.entity.receiver.active &&
			    now - peer_last_seen_ms > PEER_TIMEOUT_MS) {
				peer_ready = false;
				ui_event(DEMO_UI_PEER_ABSENT, 0u, 0);
			}
			if (pending_request_deadline_ms != 0u &&
			    now >= pending_request_deadline_ms) {
				ui_event(DEMO_UI_COMMAND_RESULT, pending_request_id,
					 DEMO_COMMAND_TIMED_OUT);
				pending_request_deadline_ms = 0u;
			}
			if (now >= next_link_snapshot_ms) {
				trace_link_snapshot("periodic");
				next_link_snapshot_ms = now + LINK_SNAPSHOT_MS;
			}
			if (now >= next_link_ui_snapshot_ms) {
				publish_link_snapshot();
				next_link_ui_snapshot_ms = now + LINK_UI_SNAPSHOT_MS;
			}
		}
		(void)k_sem_take(&worker_wake, K_MSEC(WORKER_IDLE_MS));
	}
}

int demo_service_start(void)
{
	k_mutex_init(&image_lock);
	demo_image_store_init(&image_store, image_slots, image_objects, ARRAY_SIZE(image_slots));
	demo_image_source_init(&image_source);
	demo_image_receiver_init(&image_receiver, &image_store);
	wifi_iface = net_if_get_wifi_sta();
	if (wifi_iface == NULL) {
		return -ENODEV;
	}
	net_mgmt_init_event_callback(&wifi_callback, wifi_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_callback);
	k_thread_create(&worker_thread, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), worker,
			NULL, NULL, NULL, WORKER_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&worker_thread, "eye_protocol");
	return 0;
}

bool demo_service_queue_local_send(void)
{
	const struct action_message action = {.type = ACTION_LOCAL_SEND};
	int rc = k_msgq_put(&action_queue, &action, K_NO_WAIT);

	if (rc == 0) {
		k_sem_give(&worker_wake);
	}
	return rc == 0;
}

bool demo_service_queue_remote_request(void)
{
	const struct action_message action = {.type = ACTION_REMOTE_REQUEST};
	int rc = k_msgq_put(&action_queue, &action, K_NO_WAIT);

	if (rc == 0) {
		k_sem_give(&worker_wake);
	}
	return rc == 0;
}

bool demo_service_queue_link_action(enum demo_link_action action)
{
	struct action_message message;
	int rc;

	switch (action) {
	case DEMO_LINK_UNLOCK:
		message.type = ACTION_LINK_UNLOCK;
		break;
	case DEMO_LINK_SYNC_TX:
		message.type = ACTION_LINK_SYNC_TX;
		break;
	case DEMO_LINK_SET_VR:
		message.type = ACTION_LINK_SET_VR;
		break;
	default:
		return false;
	}
	rc = k_msgq_put(&action_queue, &message, K_NO_WAIT);
	if (rc == 0) {
		k_sem_give(&worker_wake);
	}
	return rc == 0;
}

bool demo_service_get_ui_event(struct demo_ui_event *event)
{
	return k_msgq_get(&ui_queue, event, K_NO_WAIT) == 0;
}

bool demo_service_get_link_snapshot(struct demo_link_snapshot *snapshot)
{
	k_spinlock_key_t key;
	bool valid;

	if (snapshot == NULL) {
		return false;
	}
	key = k_spin_lock(&link_snapshot_lock);
	valid = link_snapshot_valid;
	if (valid) {
		*snapshot = link_snapshot;
	}
	k_spin_unlock(&link_snapshot_lock, key);
	return valid;
}

int demo_service_acquire_display_image(const struct demo_image_object **object)
{
	struct demo_image_slot *slot = NULL;
	int rc;

	if (object == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_store_acquire_display(&image_store, &slot);
	k_mutex_unlock(&image_lock);
	if (rc == 0) {
		*object = slot->object;
	} else {
		LOG_WRN("SHOW cannot acquire local image: %d", rc);
	}
	return rc;
}

void demo_service_release_display_image(const struct demo_image_object *object)
{
	if (object == NULL) {
		return;
	}
	k_mutex_lock(&image_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(image_slots); ++i) {
		if (image_slots[i].object == object) {
			int rc = demo_image_store_release_display(&image_store, &image_slots[i]);

			k_mutex_unlock(&image_lock);
			if (rc != 0) {
				LOG_ERR("SHOW release failed object=%p slot=%u rc=%d", object,
					(unsigned int)i, rc);
			}
			__ASSERT_NO_MSG(rc == 0);
			return;
		}
	}
	k_mutex_unlock(&image_lock);
	__ASSERT_NO_MSG(false);
}
