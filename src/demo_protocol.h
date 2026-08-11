/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_PROTOCOL_H
#define CCSDS_EYE_DEMO_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEMO_PROTOCOL_VERSION 1u
#define DEMO_CALLSIGN_LEN 8u
#define DEMO_CAPTURE_COMMAND_LEN 12u
#define DEMO_COMMAND_STATUS_LEN 16u
#define DEMO_PEER_STATUS_LEN 44u
#define DEMO_TEST_OBJECT_VERSION 1u
#define DEMO_TEST_OBJECT_SIZE 1536u
#define DEMO_DEDUP_CAPACITY 8u

enum demo_message_type {
	DEMO_MSG_CAPTURE_AND_RETURN = 1,
	DEMO_MSG_COMMAND_STATUS = 2,
	DEMO_MSG_PEER_STATUS = 3,
};

enum demo_command_result {
	DEMO_COMMAND_ACCEPTED = 0,
	DEMO_COMMAND_BUSY = 1,
	DEMO_COMMAND_INVALID = 2,
	DEMO_COMMAND_DUPLICATE = 3,
	DEMO_COMMAND_TIMED_OUT = 4,
};

enum demo_peer_validation {
	DEMO_PEER_VALID = 0,
	DEMO_PEER_INCOMPATIBLE,
	DEMO_PEER_DUPLICATE_ENTITY,
	DEMO_PEER_WRONG_ENTITY,
	DEMO_PEER_CONFIG_MISMATCH,
};

struct demo_capture_command {
	uint32_t request_id;
	uint64_t requesting_entity_id;
};

struct demo_command_status {
	uint32_t request_id;
	uint64_t responding_entity_id;
	enum demo_command_result result;
};

struct demo_peer_status {
	uint64_t entity_id;
	uint64_t expected_peer_entity_id;
	uint32_t local_ipv4;
	uint32_t peer_ipv4;
	uint16_t local_udp_port;
	uint16_t peer_udp_port;
	uint16_t cfdp_apid;
	uint16_t command_apid;
	uint16_t command_status_apid;
	uint16_t peer_status_apid;
	char callsign[DEMO_CALLSIGN_LEN];
};

struct demo_peer_expectation {
	uint64_t local_entity_id;
	uint64_t peer_entity_id;
	uint32_t local_ipv4;
	uint32_t peer_ipv4;
	uint16_t local_udp_port;
	uint16_t peer_udp_port;
	uint16_t cfdp_apid;
	uint16_t command_apid;
	uint16_t command_status_apid;
	uint16_t peer_status_apid;
	char peer_callsign[DEMO_CALLSIGN_LEN];
};

struct demo_dedup_entry {
	uint64_t entity_id;
	uint32_t request_id;
	uint64_t expires_ms;
	bool active;
};

struct demo_dedup_cache {
	struct demo_dedup_entry entries[DEMO_DEDUP_CAPACITY];
	size_t next;
};

int demo_capture_command_encode(const struct demo_capture_command *command,
				uint8_t *buffer, size_t capacity);
int demo_capture_command_decode(const uint8_t *buffer, size_t length,
				struct demo_capture_command *command);
int demo_command_status_encode(const struct demo_command_status *status,
			       uint8_t *buffer, size_t capacity);
int demo_command_status_decode(const uint8_t *buffer, size_t length,
			       struct demo_command_status *status);
int demo_peer_status_encode(const struct demo_peer_status *status,
			    uint8_t *buffer, size_t capacity);
int demo_peer_status_decode(const uint8_t *buffer, size_t length,
			    struct demo_peer_status *status);
enum demo_peer_validation
demo_peer_status_validate(const struct demo_peer_status *status,
			  const struct demo_peer_expectation *expected);
bool demo_dedup_check_and_record(struct demo_dedup_cache *cache,
				 uint64_t entity_id, uint32_t request_id,
				 uint64_t now_ms, uint64_t retention_ms);
void demo_test_object_generate(uint8_t object[DEMO_TEST_OBJECT_SIZE]);
bool demo_test_object_verify(const uint8_t *object, size_t size);

#endif /* CCSDS_EYE_DEMO_PROTOCOL_H */
