/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <zephyr/ztest.h>

#include "demo_image.h"
#include "demo_protocol.h"

ZTEST(demo_protocol, test_command_and_status_codecs)
{
	uint8_t encoded[DEMO_COMMAND_STATUS_LEN];
	struct demo_capture_command command = {
		.request_id = 0x10203040u,
		.requesting_entity_id = 2u,
	};
	struct demo_capture_command decoded_command;
	struct demo_command_status status = {
		.request_id = command.request_id,
		.responding_entity_id = 1u,
		.result = DEMO_COMMAND_DUPLICATE,
	};
	struct demo_command_status decoded_status;

	zassert_equal(demo_capture_command_encode(&command, encoded, sizeof(encoded)),
		      DEMO_CAPTURE_COMMAND_LEN);
	zassert_ok(
		demo_capture_command_decode(encoded, DEMO_CAPTURE_COMMAND_LEN, &decoded_command));
	zassert_equal(decoded_command.request_id, command.request_id);
	zassert_equal(decoded_command.requesting_entity_id, command.requesting_entity_id);
	for (int result = DEMO_COMMAND_ACCEPTED; result <= DEMO_COMMAND_TIMED_OUT; ++result) {
		status.result = (enum demo_command_result)result;
		zassert_equal(demo_command_status_encode(&status, encoded, sizeof(encoded)),
			      DEMO_COMMAND_STATUS_LEN);
		zassert_ok(demo_command_status_decode(encoded, sizeof(encoded), &decoded_status));
		zassert_equal(decoded_status.request_id, status.request_id);
		zassert_equal(decoded_status.responding_entity_id, status.responding_entity_id);
		zassert_equal(decoded_status.result, status.result);
	}
	encoded[0]++;
	zassert_equal(demo_command_status_decode(encoded, sizeof(encoded), &decoded_status),
		      -EINVAL);
}

ZTEST(demo_protocol, test_request_deduplication_is_bounded_and_expires)
{
	struct demo_dedup_cache cache = {0};

	zassert_false(demo_dedup_check_and_record(&cache, 2u, 42u, 100u, 1000u));
	zassert_true(demo_dedup_check_and_record(&cache, 2u, 42u, 200u, 1000u));
	zassert_false(demo_dedup_check_and_record(&cache, 2u, 43u, 200u, 1000u));
	zassert_false(demo_dedup_check_and_record(&cache, 2u, 42u, 1100u, 1000u));
	for (uint32_t i = 0; i < DEMO_DEDUP_CAPACITY * 3u; ++i) {
		(void)demo_dedup_check_and_record(&cache, 2u, 100u + i, 1200u, 1000u);
	}
	zassert_true(cache.next < DEMO_DEDUP_CAPACITY);
}

static void valid_peer(struct demo_peer_status *status, struct demo_peer_expectation *expected)
{
	*status = (struct demo_peer_status){
		.entity_id = 2u,
		.expected_peer_entity_id = 1u,
		.local_ipv4 = 0xc000020bu,
		.peer_ipv4 = 0xc000020au,
		.local_udp_port = 46002u,
		.peer_udp_port = 46001u,
		.cfdp_apid = 0x350u,
		.command_apid = 0x351u,
		.command_status_apid = 0x352u,
		.peer_status_apid = 0x353u,
		.local_spacecraft_id = 0x1002u,
		.peer_spacecraft_id = 0x1001u,
		.local_source_or_destination = 0u,
		.peer_source_or_destination = 0u,
		.transmit_vcid = 1u,
		.receive_vcid = 1u,
		.transmit_map_id = 3u,
		.receive_map_id = 3u,
		.maximum_frame_length = 1037u,
		.cop1_window_k = 4u,
		.farm_window_width = 8u,
		.minimum_transmit_interval_ms = 8u,
		.retransmission_timeout_ms = 500u,
		.feedback_interval_ms = 20u,
		.transmission_limit = 12u,
		.initial_transmit_sequence = 9u,
		.initial_receive_sequence = 7u,
	};
	*expected = (struct demo_peer_expectation){
		.local_entity_id = 1u,
		.peer_entity_id = 2u,
		.local_ipv4 = 0xc000020au,
		.peer_ipv4 = 0xc000020bu,
		.local_udp_port = 46001u,
		.peer_udp_port = 46002u,
		.cfdp_apid = 0x350u,
		.command_apid = 0x351u,
		.command_status_apid = 0x352u,
		.peer_status_apid = 0x353u,
		.local_spacecraft_id = 0x1001u,
		.peer_spacecraft_id = 0x1002u,
		.local_source_or_destination = 0u,
		.peer_source_or_destination = 0u,
		.transmit_vcid = 1u,
		.receive_vcid = 1u,
		.transmit_map_id = 3u,
		.receive_map_id = 3u,
		.maximum_frame_length = 1037u,
		.cop1_window_k = 4u,
		.farm_window_width = 8u,
		.minimum_transmit_interval_ms = 8u,
		.retransmission_timeout_ms = 500u,
		.feedback_interval_ms = 20u,
		.transmission_limit = 12u,
		.initial_transmit_sequence = 7u,
		.initial_receive_sequence = 9u,
	};
	memcpy(status->callsign, "EYE-2", 5u);
	memcpy(expected->peer_callsign, "EYE-2", 5u);
}

ZTEST(demo_protocol, test_peer_validation_diagnoses_identity_and_configuration)
{
	static const uint8_t expected_vector[DEMO_PEER_STATUS_LEN] = {
		2,    3,    0,    0,    0,    0,    0,    2,    0,    0,    0, 1,    0xc0, 0,
		2,    0x0b, 0xc0, 0,    2,    0x0a, 0xb3, 0xb2, 0xb3, 0xb1, 3, 0x50, 3,    0x51,
		3,    0x52, 3,    0x53, 'E',  'Y',  'E',  '-',  '2',  0,    0, 0,    0,    1,
		0xc2, 0x20, 0x10, 2,    0x10, 1,    0,    0,    1,    1,    3, 3,    4,    0x0d,
		4,    8,    0,    8,    1,    0xf4, 0,    20,   12,   9,    7, 0,
	};
	struct demo_peer_status status;
	struct demo_peer_expectation expected;
	uint8_t encoded[DEMO_PEER_STATUS_LEN];
	struct demo_peer_status decoded;

	valid_peer(&status, &expected);
	zassert_equal(demo_peer_status_validate(&status, &expected), DEMO_PEER_VALID);
	zassert_equal(demo_peer_status_encode(&status, encoded, sizeof(encoded)),
		      DEMO_PEER_STATUS_LEN);
	zassert_ok(demo_peer_status_decode(encoded, sizeof(encoded), &decoded));
	zassert_mem_equal(encoded, expected_vector, DEMO_PEER_STATUS_LEN);
	zassert_equal(demo_peer_status_validate(&decoded, &expected), DEMO_PEER_VALID);
	encoded[0]++;
	zassert_equal(demo_peer_status_decode(encoded, sizeof(encoded), &decoded), -EINVAL);
	status.entity_id = 1u;
	zassert_equal(demo_peer_status_validate(&status, &expected), DEMO_PEER_DUPLICATE_ENTITY);
	valid_peer(&status, &expected);
	status.expected_peer_entity_id = 9u;
	zassert_equal(demo_peer_status_validate(&status, &expected), DEMO_PEER_WRONG_ENTITY);
	valid_peer(&status, &expected);
	status.peer_udp_port++;
	zassert_equal(demo_peer_status_validate(&status, &expected), DEMO_PEER_CONFIG_MISMATCH);
	valid_peer(&status, &expected);
	status.transmit_vcid++;
	zassert_equal(demo_peer_status_validate(&status, &expected), DEMO_PEER_CONFIG_MISMATCH);
}

ZTEST(demo_protocol, test_image_transfer_progress_uses_exact_object_size)
{
	zassert_equal(DEMO_IMAGE_OBJECT_SIZE, 115232u);
	zassert_equal(demo_transfer_percent(0u, DEMO_IMAGE_OBJECT_SIZE), 0u);
	zassert_equal(demo_transfer_percent(DEMO_IMAGE_OBJECT_SIZE - 1u, DEMO_IMAGE_OBJECT_SIZE),
		      99u);
	zassert_equal(demo_transfer_percent(DEMO_IMAGE_OBJECT_SIZE, DEMO_IMAGE_OBJECT_SIZE), 100u);
	zassert_equal(demo_transfer_percent(DEMO_IMAGE_OBJECT_SIZE + 1u, DEMO_IMAGE_OBJECT_SIZE),
		      100u);
}

ZTEST_SUITE(demo_protocol, NULL, NULL, NULL, NULL, NULL);
