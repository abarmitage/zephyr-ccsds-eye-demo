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

int demo_capture_command_encode(const struct demo_capture_command *command, uint8_t *buffer,
				size_t capacity)
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
	    buffer[0] != DEMO_PROTOCOL_VERSION || buffer[1] != DEMO_MSG_CAPTURE_AND_RETURN ||
	    buffer[2] != 0u || buffer[3] != 0u) {
		return -EINVAL;
	}
	command->request_id = sys_get_be32(&buffer[4]);
	command->requesting_entity_id = sys_get_be32(&buffer[8]);
	return command->request_id != 0u && command->requesting_entity_id != 0u ? 0 : -EINVAL;
}

int demo_command_status_encode(const struct demo_command_status *status, uint8_t *buffer,
			       size_t capacity)
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

int demo_peer_status_encode(const struct demo_peer_status *status, uint8_t *buffer, size_t capacity)
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
	sys_put_be16(status->local_spacecraft_id, &buffer[44]);
	sys_put_be16(status->peer_spacecraft_id, &buffer[46]);
	/* Bytes 48-49 are reserved; frame direction is derived from content. */
	buffer[50] = status->transmit_vcid;
	buffer[51] = status->receive_vcid;
	buffer[52] = status->transmit_map_id;
	buffer[53] = status->receive_map_id;
	sys_put_be16(status->maximum_frame_length, &buffer[54]);
	buffer[56] = status->cop1_window_k;
	buffer[57] = status->farm_window_width;
	sys_put_be16(status->minimum_transmit_interval_ms, &buffer[58]);
	sys_put_be16(status->retransmission_timeout_ms, &buffer[60]);
	sys_put_be16(status->feedback_interval_ms, &buffer[62]);
	buffer[64] = status->transmission_limit;
	buffer[65] = status->initial_transmit_sequence;
	buffer[66] = status->initial_receive_sequence;
	return DEMO_PEER_STATUS_LEN;
}

int demo_peer_status_decode(const uint8_t *buffer, size_t length, struct demo_peer_status *status)
{
	if (buffer == NULL || status == NULL || length != DEMO_PEER_STATUS_LEN ||
	    buffer[0] != DEMO_PROTOCOL_VERSION || buffer[1] != DEMO_MSG_PEER_STATUS ||
	    buffer[2] != 0u || buffer[3] != 0u ||
	    sys_get_be32(&buffer[40]) != DEMO_IMAGE_OBJECT_SIZE || buffer[48] != 0u ||
	    buffer[49] != 0u || buffer[67] != 0u) {
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
	status->local_spacecraft_id = sys_get_be16(&buffer[44]);
	status->peer_spacecraft_id = sys_get_be16(&buffer[46]);
	status->transmit_vcid = buffer[50];
	status->receive_vcid = buffer[51];
	status->transmit_map_id = buffer[52];
	status->receive_map_id = buffer[53];
	status->maximum_frame_length = sys_get_be16(&buffer[54]);
	status->cop1_window_k = buffer[56];
	status->farm_window_width = buffer[57];
	status->minimum_transmit_interval_ms = sys_get_be16(&buffer[58]);
	status->retransmission_timeout_ms = sys_get_be16(&buffer[60]);
	status->feedback_interval_ms = sys_get_be16(&buffer[62]);
	status->transmission_limit = buffer[64];
	status->initial_transmit_sequence = buffer[65];
	status->initial_receive_sequence = buffer[66];
	return status->entity_id != 0u && status->expected_peer_entity_id != 0u ? 0 : -EINVAL;
}

enum demo_peer_validation demo_peer_status_validate(const struct demo_peer_status *status,
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
	    memcmp(status->callsign, expected->peer_callsign, DEMO_CALLSIGN_LEN) != 0 ||
	    status->local_spacecraft_id != expected->peer_spacecraft_id ||
	    status->peer_spacecraft_id != expected->local_spacecraft_id ||
	    status->transmit_vcid != expected->receive_vcid ||
	    status->receive_vcid != expected->transmit_vcid ||
	    status->transmit_map_id != expected->receive_map_id ||
	    status->receive_map_id != expected->transmit_map_id ||
	    status->maximum_frame_length != expected->maximum_frame_length ||
	    status->cop1_window_k != expected->cop1_window_k ||
	    status->farm_window_width != expected->farm_window_width ||
	    status->minimum_transmit_interval_ms != expected->minimum_transmit_interval_ms ||
	    status->retransmission_timeout_ms != expected->retransmission_timeout_ms ||
	    status->feedback_interval_ms != expected->feedback_interval_ms ||
	    status->transmission_limit != expected->transmission_limit ||
	    status->initial_transmit_sequence != expected->initial_receive_sequence ||
	    status->initial_receive_sequence != expected->initial_transmit_sequence) {
		return DEMO_PEER_CONFIG_MISMATCH;
	}
	return DEMO_PEER_VALID;
}

bool demo_dedup_check_and_record(struct demo_dedup_cache *cache, uint64_t entity_id,
				 uint32_t request_id, uint64_t now_ms, uint64_t retention_ms)
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

int demo_election_announcement_encode(
	const struct demo_election_candidate *candidate, uint8_t *buffer,
	size_t capacity)
{
	if (candidate == NULL || buffer == NULL ||
	    capacity < DEMO_ELECTION_ANNOUNCEMENT_LEN ||
	    candidate->spacecraft_id == 0u || candidate->attempt_id == 0u) {
		return -EINVAL;
	}
	buffer[0] = DEMO_PROTOCOL_VERSION;
	buffer[1] = DEMO_MSG_ELECTION_CANDIDATE;
	sys_put_be16(candidate->spacecraft_id, &buffer[2]);
	memcpy(&buffer[4], candidate->value, DEMO_ELECTION_VALUE_LEN);
	sys_put_be32(candidate->attempt_id, &buffer[16]);
	return DEMO_ELECTION_ANNOUNCEMENT_LEN;
}

int demo_election_announcement_decode(
	const uint8_t *buffer, size_t length,
	struct demo_election_candidate *candidate)
{
	if (buffer == NULL || candidate == NULL ||
	    length != DEMO_ELECTION_ANNOUNCEMENT_LEN ||
	    buffer[0] != DEMO_PROTOCOL_VERSION ||
	    buffer[1] != DEMO_MSG_ELECTION_CANDIDATE) {
		return -EINVAL;
	}
	memcpy(candidate->value, &buffer[4], DEMO_ELECTION_VALUE_LEN);
	candidate->spacecraft_id = sys_get_be16(&buffer[2]);
	candidate->attempt_id = sys_get_be32(&buffer[16]);
	return candidate->spacecraft_id != 0u && candidate->attempt_id != 0u
		       ? 0
		       : -EINVAL;
}

int demo_recovery_election_start(
	struct demo_recovery_election *election,
	const struct demo_election_candidate *local, uint64_t deadline_ms)
{
	if (election == NULL || local == NULL || local->spacecraft_id == 0u ||
	    local->attempt_id == 0u || deadline_ms == 0u) {
		return -EINVAL;
	}
	memset(election, 0, sizeof(*election));
	election->local = *local;
	election->deadline_ms = deadline_ms;
	election->result = DEMO_ELECTION_WAITING;
	election->active = true;
	return 0;
}

int demo_recovery_election_receive(
	struct demo_recovery_election *election, const uint8_t *buffer,
	size_t length, uint16_t expected_peer_spacecraft_id)
{
	struct demo_election_candidate peer;
	int comparison;
	int rc;

	if (election == NULL || !election->active) {
		return -EAGAIN;
	}
	rc = demo_election_announcement_decode(buffer, length, &peer);
	if (rc != 0 || peer.spacecraft_id != expected_peer_spacecraft_id) {
		return -EINVAL;
	}
	if (election->peer_valid) {
		return peer.spacecraft_id == election->peer.spacecraft_id &&
			       peer.attempt_id == election->peer.attempt_id &&
			       memcmp(peer.value, election->peer.value,
				      sizeof(peer.value)) == 0
			       ? 0
			       : -EALREADY;
	}
	if (election->result != DEMO_ELECTION_WAITING) {
		return -EAGAIN;
	}
	election->peer = peer;
	election->peer_valid = true;
	comparison = memcmp(election->local.value, peer.value,
			    sizeof(election->local.value));
	if (comparison == 0) {
		comparison = election->local.spacecraft_id < peer.spacecraft_id
				     ? -1
				     : 1;
	}
	election->result = comparison < 0 ? DEMO_ELECTION_WON
					: DEMO_ELECTION_LOST;
	return 0;
}

void demo_recovery_election_tick(struct demo_recovery_election *election,
				 uint64_t now_ms)
{
	if (election != NULL && election->active &&
	    election->result == DEMO_ELECTION_WAITING &&
	    now_ms >= election->deadline_ms) {
		election->result = DEMO_ELECTION_TIMED_OUT;
	}
}

bool demo_recovery_application_tx_allowed(
	const struct demo_recovery_election *election, bool confirmed)
{
	return election == NULL || !election->active || confirmed ||
	       election->result == DEMO_ELECTION_TIMED_OUT;
}

bool demo_recovery_status_tx_allowed(
	const struct demo_recovery_election *election, bool cutover,
	bool confirmed)
{
	if (demo_recovery_application_tx_allowed(election, confirmed)) {
		return true;
	}
	return cutover && election->result == DEMO_ELECTION_WON;
}

bool demo_recovery_otar_submission_allowed(
	const struct demo_recovery_election *election, bool pending,
	bool retained_keys, bool cutover)
{
	return election != NULL && election->active &&
	       election->result == DEMO_ELECTION_WON && !pending &&
	       !retained_keys && !cutover;
}

bool demo_recovery_mark_confirmed(bool cutover, bool *confirmed)
{
	if (confirmed == NULL || !cutover || *confirmed) {
		return false;
	}
	*confirmed = true;
	return true;
}

bool demo_clcw_divergence_observe(struct demo_clcw_divergence *state,
				  bool out_of_range, uint8_t report_value,
				  uint64_t now_ms, uint64_t confirmation_ms)
{
	if (state == NULL) {
		return false;
	}
	if (!out_of_range) {
		memset(state, 0, sizeof(*state));
		return false;
	}
	if (!state->observed || state->report_value != report_value) {
		state->first_seen_ms = now_ms;
		state->report_value = report_value;
		state->observed = true;
		return false;
	}
	if (now_ms - state->first_seen_ms < confirmation_ms) {
		return false;
	}
	memset(state, 0, sizeof(*state));
	return true;
}
