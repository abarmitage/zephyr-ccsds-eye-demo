/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_PROTOCOL_H
#define CCSDS_EYE_DEMO_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEMO_PROTOCOL_VERSION    2u
#define DEMO_CALLSIGN_LEN        8u
#define DEMO_CAPTURE_COMMAND_LEN 12u
#define DEMO_COMMAND_STATUS_LEN  16u
#define DEMO_PEER_STATUS_LEN     68u
#define DEMO_DEDUP_CAPACITY      8u
#define DEMO_ELECTION_VALUE_LEN  12u
#define DEMO_ELECTION_ANNOUNCEMENT_LEN 20u

enum demo_message_type {
	DEMO_MSG_CAPTURE_AND_RETURN = 1,
	DEMO_MSG_COMMAND_STATUS = 2,
	DEMO_MSG_PEER_STATUS = 3,
	DEMO_MSG_ELECTION_CANDIDATE = 4,
};

enum demo_election_result {
	DEMO_ELECTION_WAITING = 0,
	DEMO_ELECTION_WON,
	DEMO_ELECTION_LOST,
	DEMO_ELECTION_TIMED_OUT,
};

struct demo_election_candidate {
	uint8_t value[DEMO_ELECTION_VALUE_LEN];
	uint32_t attempt_id;
	uint16_t spacecraft_id;
};

struct demo_recovery_election {
	struct demo_election_candidate local;
	struct demo_election_candidate peer;
	uint64_t deadline_ms;
	enum demo_election_result result;
	bool active;
	bool peer_valid;
};

struct demo_clcw_divergence {
	uint64_t first_seen_ms;
	uint8_t report_value;
	bool observed;
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
	uint16_t local_spacecraft_id;
	uint16_t peer_spacecraft_id;
	uint8_t transmit_vcid;
	uint8_t receive_vcid;
	uint8_t transmit_map_id;
	uint8_t receive_map_id;
	uint16_t maximum_frame_length;
	uint8_t cop1_window_k;
	uint8_t farm_window_width;
	uint16_t minimum_transmit_interval_ms;
	uint16_t retransmission_timeout_ms;
	uint16_t feedback_interval_ms;
	uint8_t transmission_limit;
	uint8_t initial_transmit_sequence;
	uint8_t initial_receive_sequence;
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
	uint16_t local_spacecraft_id;
	uint16_t peer_spacecraft_id;
	uint8_t transmit_vcid;
	uint8_t receive_vcid;
	uint8_t transmit_map_id;
	uint8_t receive_map_id;
	uint16_t maximum_frame_length;
	uint8_t cop1_window_k;
	uint8_t farm_window_width;
	uint16_t minimum_transmit_interval_ms;
	uint16_t retransmission_timeout_ms;
	uint16_t feedback_interval_ms;
	uint8_t transmission_limit;
	uint8_t initial_transmit_sequence;
	uint8_t initial_receive_sequence;
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

int demo_capture_command_encode(const struct demo_capture_command *command, uint8_t *buffer,
				size_t capacity);
int demo_capture_command_decode(const uint8_t *buffer, size_t length,
				struct demo_capture_command *command);
int demo_command_status_encode(const struct demo_command_status *status, uint8_t *buffer,
			       size_t capacity);
int demo_command_status_decode(const uint8_t *buffer, size_t length,
			       struct demo_command_status *status);
int demo_peer_status_encode(const struct demo_peer_status *status, uint8_t *buffer,
			    size_t capacity);
int demo_peer_status_decode(const uint8_t *buffer, size_t length, struct demo_peer_status *status);
enum demo_peer_validation demo_peer_status_validate(const struct demo_peer_status *status,
						    const struct demo_peer_expectation *expected);
bool demo_dedup_check_and_record(struct demo_dedup_cache *cache, uint64_t entity_id,
				 uint32_t request_id, uint64_t now_ms, uint64_t retention_ms);
uint8_t demo_transfer_percent(uint32_t bytes_transferred, uint32_t file_size);
int demo_election_announcement_encode(
	const struct demo_election_candidate *candidate, uint8_t *buffer,
	size_t capacity);
int demo_election_announcement_decode(
	const uint8_t *buffer, size_t length,
	struct demo_election_candidate *candidate);
int demo_recovery_election_start(
	struct demo_recovery_election *election,
	const struct demo_election_candidate *local, uint64_t deadline_ms);
int demo_recovery_election_receive(
	struct demo_recovery_election *election, const uint8_t *buffer,
	size_t length, uint16_t expected_peer_spacecraft_id);
void demo_recovery_election_tick(struct demo_recovery_election *election,
				 uint64_t now_ms);
/** Ordinary application traffic stays paused until mutual new-key confirmation. */
bool demo_recovery_application_tx_allowed(
	const struct demo_recovery_election *election, bool confirmed);
/** Only the winner may send the first operational confirmation after cutover. */
bool demo_recovery_status_tx_allowed(
	const struct demo_recovery_election *election, bool cutover,
	bool confirmed);
/** A decided election permits exactly one OTAR through successful cutover. */
bool demo_recovery_otar_submission_allowed(
	const struct demo_recovery_election *election, bool pending,
	bool retained_keys, bool cutover);
/** Mark only the first authenticated operational packet after cutover. */
bool demo_recovery_mark_confirmed(bool cutover, bool *confirmed);
/** Confirm a repeated out-of-range CLCW while ignoring one stale datagram. */
bool demo_clcw_divergence_observe(struct demo_clcw_divergence *state,
				  bool out_of_range, uint8_t report_value,
				  uint64_t now_ms, uint64_t confirmation_ms);
#endif /* CCSDS_EYE_DEMO_PROTOCOL_H */
