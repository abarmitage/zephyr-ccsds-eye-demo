/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_protocol.h"
#include "demo_image.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

static bool valid_result(uint8_t result)
{
	return result <= DEMO_COMMAND_TIMED_OUT;
}

int demo_capture_command_encode(const struct demo_capture_command *command,
				uint8_t *buffer, size_t capacity)
{
	if (command == NULL || buffer == NULL || capacity < DEMO_CAPTURE_COMMAND_LEN ||
	    command->request_id == 0u || command->requesting_entity_id == 0u) {
		return -EINVAL;
	}
	buffer[0] = DEMO_PROTOCOL_VERSION;
	buffer[1] = DEMO_MSG_CAPTURE_AND_RETURN;
	buffer[2] = 0u;
	buffer[3] = 0u;
	sys_put_be32(command->request_id, &buffer[4]);
	sys_put_be32((uint32_t)command->requesting_entity_id, &buffer[8]);
	return DEMO_CAPTURE_COMMAND_LEN;
}

int demo_capture_command_decode(const uint8_t *buffer, size_t length,
				struct demo_capture_command *command)
{
	if (buffer == NULL || command == NULL || length != DEMO_CAPTURE_COMMAND_LEN ||
	    buffer[0] != DEMO_PROTOCOL_VERSION ||
	    buffer[1] != DEMO_MSG_CAPTURE_AND_RETURN || buffer[2] != 0u ||
	    buffer[3] != 0u) {
		return -EINVAL;
	}
	command->request_id = sys_get_be32(&buffer[4]);
	command->requesting_entity_id = sys_get_be32(&buffer[8]);
	return command->request_id != 0u && command->requesting_entity_id != 0u ? 0 : -EINVAL;
}

int demo_command_status_encode(const struct demo_command_status *status,
			       uint8_t *buffer, size_t capacity)
{
	if (status == NULL || buffer == NULL || capacity < DEMO_COMMAND_STATUS_LEN ||
	    status->request_id == 0u || status->responding_entity_id == 0u ||
	    !valid_result((uint8_t)status->result)) {
		return -EINVAL;
	}
	buffer[0] = DEMO_PROTOCOL_VERSION;
	buffer[1] = DEMO_MSG_COMMAND_STATUS;
	buffer[2] = (uint8_t)status->result;
	buffer[3] = 0u;
	sys_put_be32(status->request_id, &buffer[4]);
	sys_put_be32((uint32_t)status->responding_entity_id, &buffer[8]);
	sys_put_be32(0u, &buffer[12]);
	return DEMO_COMMAND_STATUS_LEN;
}

int demo_command_status_decode(const uint8_t *buffer, size_t length,
			       struct demo_command_status *status)
{
	if (buffer == NULL || status == NULL || length != DEMO_COMMAND_STATUS_LEN ||
	    buffer[0] != DEMO_PROTOCOL_VERSION || buffer[1] != DEMO_MSG_COMMAND_STATUS ||
	    !valid_result(buffer[2]) || buffer[3] != 0u || sys_get_be32(&buffer[12]) != 0u) {
		return -EINVAL;
	}
	status->result = (enum demo_command_result)buffer[2];
	status->request_id = sys_get_be32(&buffer[4]);
	status->responding_entity_id = sys_get_be32(&buffer[8]);
	return status->request_id != 0u && status->responding_entity_id != 0u ? 0 : -EINVAL;
}

int demo_peer_status_encode(const struct demo_peer_status *status,
			    uint8_t *buffer, size_t capacity)
{
	if (status == NULL || buffer == NULL || capacity < DEMO_PEER_STATUS_LEN ||
	    status->entity_id == 0u || status->expected_peer_entity_id == 0u) {
		return -EINVAL;
	}
	memset(buffer, 0, DEMO_PEER_STATUS_LEN);
	buffer[0] = DEMO_PROTOCOL_VERSION;
	buffer[1] = DEMO_MSG_PEER_STATUS;
	sys_put_be32((uint32_t)status->entity_id, &buffer[4]);
	sys_put_be32((uint32_t)status->expected_peer_entity_id, &buffer[8]);
	sys_put_be32(status->local_ipv4, &buffer[12]);
	sys_put_be32(status->peer_ipv4, &buffer[16]);
	sys_put_be16(status->local_udp_port, &buffer[20]);
	sys_put_be16(status->peer_udp_port, &buffer[22]);
	sys_put_be16(status->cfdp_apid, &buffer[24]);
	sys_put_be16(status->command_apid, &buffer[26]);
	sys_put_be16(status->command_status_apid, &buffer[28]);
	sys_put_be16(status->peer_status_apid, &buffer[30]);
	memcpy(&buffer[32], status->callsign, DEMO_CALLSIGN_LEN);
	sys_put_be32(DEMO_IMAGE_OBJECT_SIZE, &buffer[40]);
	return DEMO_PEER_STATUS_LEN;
}

int demo_peer_status_decode(const uint8_t *buffer, size_t length,
			    struct demo_peer_status *status)
{
	if (buffer == NULL || status == NULL || length != DEMO_PEER_STATUS_LEN ||
	    buffer[0] != DEMO_PROTOCOL_VERSION || buffer[1] != DEMO_MSG_PEER_STATUS ||
	    buffer[2] != 0u || buffer[3] != 0u ||
	    sys_get_be32(&buffer[40]) != DEMO_IMAGE_OBJECT_SIZE) {
		return -EINVAL;
	}
	memset(status, 0, sizeof(*status));
	status->entity_id = sys_get_be32(&buffer[4]);
	status->expected_peer_entity_id = sys_get_be32(&buffer[8]);
	status->local_ipv4 = sys_get_be32(&buffer[12]);
	status->peer_ipv4 = sys_get_be32(&buffer[16]);
	status->local_udp_port = sys_get_be16(&buffer[20]);
	status->peer_udp_port = sys_get_be16(&buffer[22]);
	status->cfdp_apid = sys_get_be16(&buffer[24]);
	status->command_apid = sys_get_be16(&buffer[26]);
	status->command_status_apid = sys_get_be16(&buffer[28]);
	status->peer_status_apid = sys_get_be16(&buffer[30]);
	memcpy(status->callsign, &buffer[32], DEMO_CALLSIGN_LEN);
	return status->entity_id != 0u && status->expected_peer_entity_id != 0u ? 0 : -EINVAL;
}

enum demo_peer_validation
demo_peer_status_validate(const struct demo_peer_status *status,
			  const struct demo_peer_expectation *expected)
{
	if (status->entity_id == expected->local_entity_id) {
		return DEMO_PEER_DUPLICATE_ENTITY;
	}
	if (status->entity_id != expected->peer_entity_id ||
	    status->expected_peer_entity_id != expected->local_entity_id) {
		return DEMO_PEER_WRONG_ENTITY;
	}
	if (status->local_ipv4 != expected->peer_ipv4 ||
	    status->peer_ipv4 != expected->local_ipv4 ||
	    status->local_udp_port != expected->peer_udp_port ||
	    status->peer_udp_port != expected->local_udp_port ||
	    status->cfdp_apid != expected->cfdp_apid ||
	    status->command_apid != expected->command_apid ||
	    status->command_status_apid != expected->command_status_apid ||
	    status->peer_status_apid != expected->peer_status_apid ||
	    memcmp(status->callsign, expected->peer_callsign, DEMO_CALLSIGN_LEN) != 0) {
		return DEMO_PEER_CONFIG_MISMATCH;
	}
	return DEMO_PEER_VALID;
}

bool demo_dedup_check_and_record(struct demo_dedup_cache *cache,
				 uint64_t entity_id, uint32_t request_id,
				 uint64_t now_ms, uint64_t retention_ms)
{
	for (size_t i = 0; i < DEMO_DEDUP_CAPACITY; ++i) {
		if (cache->entries[i].active && cache->entries[i].expires_ms <= now_ms) {
			cache->entries[i].active = false;
		}
		if (cache->entries[i].active && cache->entries[i].entity_id == entity_id &&
		    cache->entries[i].request_id == request_id) {
			return true;
		}
	}
	cache->entries[cache->next] = (struct demo_dedup_entry){
		.entity_id = entity_id,
		.request_id = request_id,
		.expires_ms = now_ms + retention_ms,
		.active = true,
	};
	cache->next = (cache->next + 1u) % DEMO_DEDUP_CAPACITY;
	return false;
}

uint8_t demo_transfer_percent(uint32_t bytes_transferred, uint32_t file_size)
{
	if (file_size == 0u) {
		return 0u;
	}
	return (uint8_t)(((uint64_t)MIN(bytes_transferred, file_size) * 100u) / file_size);
}
