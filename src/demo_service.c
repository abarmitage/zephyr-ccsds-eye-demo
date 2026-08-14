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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(eye_service, CONFIG_LOG_DEFAULT_LEVEL);

#define CFDP_FILE_PDU_COUNT DIV_ROUND_UP(DEMO_TEST_OBJECT_SIZE, CONFIG_CCSDS_CFDP_MAX_SEGMENT_SIZE)
/* One synchronous send emits metadata, every File Data PDU, and EOF as a burst.
 * Leave room for the command response and periodic/control traffic as well.
 */
#define RX_QUEUE_DEPTH      (CFDP_FILE_PDU_COUNT + 6u)
#define ROUTER_QUEUE_DEPTH  8u
#define CFDP_EVENT_QUEUE_DEPTH 24u
#define ACTION_QUEUE_DEPTH     4u
#define UI_QUEUE_DEPTH         16u
#define WORKER_STACK_SIZE      7168u
#define WORKER_PRIORITY        10
#define STATUS_PERIOD_MS       1000u
#define PEER_TIMEOUT_MS        3500u
#define COMMAND_TIMEOUT_MS     5000u
#define REQUEST_RETENTION_MS   60000u
#define SERVICE_POLL_MS        25u
#define PROGRESS_UI_PERIOD_MS  100u
#define TEST_SOURCE_NAME       "eye-test-v1.bin"
#define TEST_DEST_NAME         "eye-received-v1.bin"

BUILD_ASSERT(CONFIG_CCSDS_UDP_MAX_UNIT_LEN <= 1280,
	     "UDP unit must remain below a conservative non-fragmenting IPv4 payload");
BUILD_ASSERT(CCSDS_SPACE_PACKET_PRIMARY_HDR_LEN + CCSDS_CFDP_MAX_PDU_SIZE <=
		     CONFIG_CCSDS_UDP_MAX_UNIT_LEN,
	     "CFDP Space Packet must fit one UDP datagram");
BUILD_ASSERT(CONFIG_CCSDS_CFDP_MAX_SEGMENT_SIZE < DEMO_TEST_OBJECT_SIZE,
	     "test object must require multiple File Data PDUs");

enum action_type {
	ACTION_LOCAL_SEND,
	ACTION_REMOTE_REQUEST,
};

enum network_event_type {
	NETWORK_ASSOCIATED,
	NETWORK_DISCONNECTED,
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

struct cfdp_event_message {
	ccsds_cfdp_event_t event;
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

struct memory_object {
	uint8_t data[DEMO_TEST_OBJECT_SIZE];
	uint32_t size;
	bool open;
	bool verified;
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_datagram), RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(router_queue, sizeof(struct router_message), ROUTER_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(action_queue, sizeof(struct action_message), ACTION_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(network_event_queue, sizeof(struct network_event_message), 2, 4);
K_MSGQ_DEFINE(cfdp_event_queue, sizeof(struct cfdp_event_message), CFDP_EVENT_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(ui_queue, sizeof(struct demo_ui_event), UI_QUEUE_DEPTH, 4);
K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK_SIZE);

static struct k_thread worker_thread;
static struct net_mgmt_event_callback wifi_callback;
static struct net_if *wifi_iface;
static struct ccsds_udp udp;
static struct ccsds_router router;
static struct ccsds_profile_input input_profile;
static struct ccsds_cfdp_service cfdp_service;
static struct memory_object source_object;
static struct memory_object receive_object;
static struct demo_dedup_cache dedup_cache;
static bool protocol_ready;
static bool protocol_initialized;
static bool peer_ready;
static bool operation_active;
static bool outgoing_active;
static struct send_operation active_operation;
static uint64_t peer_last_seen_ms;
static uint64_t next_status_ms;
static uint32_t packet_sequence;
static uint32_t next_request_id = 1u;
static uint32_t pending_request_id;
static uint64_t pending_request_deadline_ms;
static bool receiver_wait_logged;
static uint32_t receiver_retry_logged;
static struct progress_update tx_progress;
static struct progress_update rx_progress;
static struct demo_image_object image_objects[DEMO_IMAGE_SLOT_COUNT]
	__attribute__((section(".ext_ram.bss.eye_images"), aligned(32)));
static struct demo_image_slot image_slots[DEMO_IMAGE_SLOT_COUNT];
static struct demo_image_store image_store;
static struct k_mutex image_lock;
static struct demo_camera_adapter camera_adapter;
static int camera_status;

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
	struct memory_object *object = user;

	if (strcmp(path, TEST_SOURCE_NAME) != 0 || object->open) {
		return -EINVAL;
	}
	object->open = true;
	*handle = object;
	*size = object->size;
	return 0;
}

static int object_read(void *user, void *handle, uint32_t offset, uint8_t *buffer, size_t length,
		       size_t *read_length)
{
	struct memory_object *object = handle;
	size_t available;

	ARG_UNUSED(user);
	if (object == NULL || !object->open || offset > object->size) {
		return -EINVAL;
	}
	available = object->size - offset;
	*read_length = MIN(length, available);
	memcpy(buffer, &object->data[offset], *read_length);
	return 0;
}

static int object_close(void *user, void *handle)
{
	struct memory_object *object = handle;

	ARG_UNUSED(user);
	if (object == NULL || !object->open) {
		return -EINVAL;
	}
	object->open = false;
	return 0;
}

static int receive_open_write(void *user, const char *path, void **handle)
{
	struct memory_object *object = user;

	if (strcmp(path, TEST_DEST_NAME) != 0 || object->open) {
		return -EINVAL;
	}
	memset(object, 0, sizeof(*object));
	object->open = true;
	*handle = object;
	return 0;
}

static int receive_write(void *user, void *handle, uint32_t offset, const uint8_t *buffer,
			 size_t length)
{
	struct memory_object *object = handle;
	size_t end = (size_t)offset + length;

	ARG_UNUSED(user);
	if (object == NULL || !object->open || end > sizeof(object->data)) {
		return -EFBIG;
	}
	memcpy(&object->data[offset], buffer, length);
	object->size = MAX(object->size, (uint32_t)end);
	return 0;
}

static int receive_commit(void *user, const char *path)
{
	struct memory_object *object = user;

	if (strcmp(path, TEST_DEST_NAME) != 0) {
		return -EINVAL;
	}
	ui_event(DEMO_UI_VERIFYING, 0u, 0);
	object->verified = demo_test_object_verify(object->data, object->size);
	LOG_INF("CFDP RX object verification %s", object->verified ? "passed" : "failed");
	return object->verified ? 0 : -EBADMSG;
}

static int receive_discard(void *user, const char *path)
{
	struct memory_object *object = user;

	ARG_UNUSED(path);
	memset(object, 0, sizeof(*object));
	return 0;
}

static const ccsds_cfdp_filestore_ops_t source_ops = {
	.user = &source_object,
	.open_read = source_open_read,
	.read = object_read,
	.close = object_close,
};

static const ccsds_cfdp_filestore_ops_t receive_ops = {
	.user = &receive_object,
	.open_write_tmp = receive_open_write,
	.read = object_read,
	.write = receive_write,
	.close = object_close,
	.commit_tmp = receive_commit,
	.discard_tmp = receive_discard,
};

static int queue_router_message(enum router_message_type type, const uint8_t *payload,
				size_t length)
{
	struct router_message message;

	if (payload == NULL || length == 0u || length > sizeof(message.payload)) {
		return -EINVAL;
	}
	message.type = type;
	message.length = length;
	memcpy(message.payload, payload, length);
	return k_msgq_put(&router_queue, &message, K_NO_WAIT);
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

	ARG_UNUSED(user);
	if (length == 0u || length > sizeof(datagram.data)) {
		return -EMSGSIZE;
	}
	datagram.length = length;
	memcpy(datagram.data, unit, length);
	return k_msgq_put(&rx_queue, &datagram, K_NO_WAIT);
}

static void cfdp_event_callback(void *user, const ccsds_cfdp_event_t *event)
{
	const struct cfdp_event_message message = {.event = *event};

	ARG_UNUSED(user);
	(void)k_msgq_put(&cfdp_event_queue, &message, K_NO_WAIT);
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

	return rc == 0 ? ccsds_udp_send(&udp, buffer, length) : rc;
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
	};

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

static int start_send_operation(const struct send_operation *operation)
{
	struct demo_image_slot *staging = NULL;
	ccsds_cfdp_transaction_id_t transaction_id;
	const ccsds_cfdp_put_request_t request = {
		.source_path = TEST_SOURCE_NAME,
		.destination_path = TEST_DEST_NAME,
		.checksum_type = CCSDS_CFDP_CHECKSUM_TYPE_MODULAR,
		.closure_requested = true,
		.acknowledged_mode = true,
	};
	enum ccsds_cfdp_status status;
	int rc;

	if (!protocol_ready || !peer_ready || operation_active ||
	    operation->destination_entity_id != CONFIG_EYE_DEMO_PEER_ENTITY_ID) {
		return -EBUSY;
	}
	active_operation = *operation;
	operation_active = true;
	ui_event(DEMO_UI_CAPTURING, operation->request_id, 0);
	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_store_claim_staging(&image_store, &staging);
	k_mutex_unlock(&image_lock);
	if (rc == 0) {
		rc = camera_status != 0
			     ? camera_status
			     : demo_camera_acquire_object(&camera_adapter, staging->object);
	}
	if (rc != 0) {
		if (staging != NULL) {
			k_mutex_lock(&image_lock, K_FOREVER);
			(void)demo_image_store_abort_staging(&image_store, staging);
			k_mutex_unlock(&image_lock);
		}
		operation_active = false;
		ui_event(DEMO_UI_FAILED, operation->request_id, rc);
		LOG_ERR("camera acquisition failed: %d", rc);
		return rc;
	}
	k_mutex_lock(&image_lock, K_FOREVER);
	rc = demo_image_store_promote(&image_store, staging);
	k_mutex_unlock(&image_lock);
	if (rc != 0) {
		operation_active = false;
		ui_event(DEMO_UI_FAILED, operation->request_id, rc);
		return rc;
	}
	LOG_INF("local image ready request=%u", operation->request_id);
	outgoing_active = true;
	ui_event(DEMO_UI_CFDP_TX, operation->request_id, 0);
	status =
		ccsds_cfdp_service_send_file(&cfdp_service, &source_ops, &request, &transaction_id);
	if (status != CCSDS_CFDP_STATUS_OK) {
		operation_active = false;
		outgoing_active = false;
		ui_event(DEMO_UI_FAILED, operation->request_id, status);
		return -EIO;
	}
	return 0;
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
		result = start_send_operation(&operation) == 0 ? DEMO_COMMAND_ACCEPTED
							       : DEMO_COMMAND_BUSY;
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
		peer_last_seen_ms = now;
		if (!peer_ready) {
			peer_ready = true;
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
		switch (message.type) {
		case ROUTER_CFDP: {
			ccsds_cfdp_pdu_header_t header;
			size_t header_length = 0u;
			enum ccsds_cfdp_status status;
			bool starts_receive;

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

static void process_cfdp_events(uint64_t now)
{
	struct cfdp_event_message message;

	while (k_msgq_get(&cfdp_event_queue, &message, K_NO_WAIT) == 0) {
		struct progress_update *progress =
			message.event.direction == CCSDS_CFDP_DIRECTION_SENDER ? &tx_progress
									       : &rx_progress;
		enum demo_transfer_direction direction =
			message.event.direction == CCSDS_CFDP_DIRECTION_SENDER ? DEMO_TRANSFER_TX
									       : DEMO_TRANSFER_RX;

		if (message.event.type == CCSDS_CFDP_EVENT_TRANSACTION_STARTED ||
		    message.event.type == CCSDS_CFDP_EVENT_FILE_SEGMENT_SENT ||
		    message.event.type == CCSDS_CFDP_EVENT_FILE_SEGMENT_RECV ||
		    message.event.type == CCSDS_CFDP_EVENT_NAK_SENT ||
		    message.event.type == CCSDS_CFDP_EVENT_NAK_RECV ||
		    message.event.type == CCSDS_CFDP_EVENT_RETRANSMIT) {
			progress->bytes_transferred = message.event.bytes_transferred;
			progress->file_size = message.event.file_size;
			progress->recovery_activity =
				message.event.phase == CCSDS_CFDP_PHASE_RECOVERY;
			progress->pending = true;
			flush_progress(progress, direction, now, false);
		}

		if (message.event.type == CCSDS_CFDP_EVENT_COMPLETE ||
		    message.event.type == CCSDS_CFDP_EVENT_FAILED) {
			bool sender = message.event.direction == CCSDS_CFDP_DIRECTION_SENDER;
			bool success = message.event.type == CCSDS_CFDP_EVENT_COMPLETE &&
				       message.event.status == CCSDS_CFDP_STATUS_OK;
			uint32_t request_id = sender ? active_operation.request_id : 0u;

			progress->bytes_transferred = message.event.bytes_transferred;
			progress->file_size = message.event.file_size;
			progress->recovery_activity = false;
			progress->pending = true;
			flush_progress(progress, direction, now, true);

			if (!sender) {
				success = success && receive_object.verified;
			}
			LOG_INF("CFDP %s terminal event=%u status=%d transaction=%llu:%llu",
				sender ? "TX" : "RX", message.event.type, message.event.status,
				(unsigned long long)message.event.transaction_id.source_entity_id,
				(unsigned long long)
					message.event.transaction_id.transaction_sequence_number);
			if (sender && active_operation.has_request_id && !success &&
			    message.event.status == CCSDS_CFDP_STATUS_INACTIVITY_DETECTED) {
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

static void process_actions(uint64_t now)
{
	struct action_message action;

	while (k_msgq_get(&action_queue, &action, K_NO_WAIT) == 0) {
		if (action.type == ACTION_LOCAL_SEND) {
			const struct send_operation operation = {
				.destination_entity_id = CONFIG_EYE_DEMO_PEER_ENTITY_ID,
				.origin = OPERATION_LOCAL_BUTTON,
			};

			if (start_send_operation(&operation) == -EBUSY) {
				ui_event(DEMO_UI_BUSY, 0u, -EBUSY);
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
			if (!peer_ready || length < 0 ||
			    send_space_packet(CONFIG_EYE_DEMO_COMMAND_APID, CCSDS_PACKET_TYPE_TC,
					      payload, (size_t)length) != 0) {
				ui_event(DEMO_UI_FAILED, command.request_id, -ENETUNREACH);
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
	struct ccsds_cfdp_service_config cfdp_config = {
		.local_entity_id = CONFIG_EYE_DEMO_LOCAL_ENTITY_ID,
		.remote_entity_id = CONFIG_EYE_DEMO_PEER_ENTITY_ID,
		.entity_id_len = 1u,
		.transaction_sequence_number_len = 2u,
		.initial_transaction_sequence_number = 1u,
		.local_apid = CONFIG_EYE_DEMO_CFDP_APID,
		.remote_apid = CONFIG_EYE_DEMO_CFDP_APID,
		.packet_type = CCSDS_PACKET_TYPE_TC,
		.send_packet = ccsds_udp_send,
		.send_user = &udp,
		.now_ms = now_ms,
		.receive_filestore = &receive_ops,
		.event_cb = cfdp_event_callback,
	};
	int rc;

	demo_test_object_generate(source_object.data);
	source_object.size = sizeof(source_object.data);
	ccsds_router_init(&router);
	ccsds_profile_input_init(&input_profile, &router, NULL);
	if (ccsds_udp_init(&udp, &udp_config) != 0 ||
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
	(void)k_msgq_put(&network_event_queue, &message, K_NO_WAIT);
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
		struct rx_datagram datagram;
		uint64_t now = (uint64_t)k_uptime_get();

		while (k_msgq_get(&network_event_queue, &network_event, K_NO_WAIT) == 0) {
			if (network_event.type == NETWORK_ASSOCIATED && !association_handled) {
				association_handled = true;
				if (apply_static_ipv4(wifi_iface) == 0 &&
				    (protocol_initialized || initialize_protocol() == 0)) {
					protocol_initialized = true;
					protocol_ready = true;
					next_status_ms = now;
					ui_event(DEMO_UI_NETWORK_READY, 0u, 0);
					LOG_INF("network ready %s:%u peer %s:%u",
						CONFIG_EYE_DEMO_LOCAL_IPV4,
						CONFIG_EYE_DEMO_LOCAL_UDP_PORT,
						CONFIG_EYE_DEMO_PEER_IPV4,
						CONFIG_EYE_DEMO_PEER_UDP_PORT);
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
		while (k_msgq_get(&rx_queue, &datagram, K_NO_WAIT) == 0) {
			(void)ccsds_profile_input_dispatch_unit(&input_profile, datagram.data,
								datagram.length);
			process_router_messages(now);
		}
		process_router_messages(now);
		process_cfdp_events(now);
		if (protocol_ready) {
			process_actions(now);
			ccsds_cfdp_service_poll(&cfdp_service, now);
			trace_receiver_closure();
			if (now >= next_status_ms) {
				send_peer_status();
				next_status_ms = now + STATUS_PERIOD_MS;
			}
			if (peer_ready && now - peer_last_seen_ms > PEER_TIMEOUT_MS) {
				peer_ready = false;
				ui_event(DEMO_UI_PEER_ABSENT, 0u, 0);
			}
			if (pending_request_deadline_ms != 0u &&
			    now >= pending_request_deadline_ms) {
				ui_event(DEMO_UI_COMMAND_RESULT, pending_request_id,
					 DEMO_COMMAND_TIMED_OUT);
				pending_request_deadline_ms = 0u;
			}
		}
		k_sleep(K_MSEC(SERVICE_POLL_MS));
	}
}

int demo_service_start(void)
{
	k_mutex_init(&image_lock);
	demo_image_store_init(&image_store, image_slots, image_objects, ARRAY_SIZE(image_slots));
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

	return k_msgq_put(&action_queue, &action, K_NO_WAIT) == 0;
}

bool demo_service_queue_remote_request(void)
{
	const struct action_message action = {.type = ACTION_REMOTE_REQUEST};

	return k_msgq_put(&action_queue, &action, K_NO_WAIT) == 0;
}

bool demo_service_get_ui_event(struct demo_ui_event *event)
{
	return k_msgq_get(&ui_queue, event, K_NO_WAIT) == 0;
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
