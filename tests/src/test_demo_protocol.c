/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <zephyr/ztest.h>

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
	zassert_ok(demo_capture_command_decode(encoded, DEMO_CAPTURE_COMMAND_LEN,
					       &decoded_command));
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

static void valid_peer(struct demo_peer_status *status,
		       struct demo_peer_expectation *expected)
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
	};
	memcpy(status->callsign, "EYE-2", 5u);
	memcpy(expected->peer_callsign, "EYE-2", 5u);
}

ZTEST(demo_protocol, test_peer_validation_diagnoses_identity_and_configuration)
{
	struct demo_peer_status status;
	struct demo_peer_expectation expected;
	uint8_t encoded[DEMO_PEER_STATUS_LEN];
	struct demo_peer_status decoded;

	valid_peer(&status, &expected);
	zassert_equal(demo_peer_status_validate(&status, &expected), DEMO_PEER_VALID);
	zassert_equal(demo_peer_status_encode(&status, encoded, sizeof(encoded)),
		      DEMO_PEER_STATUS_LEN);
	zassert_ok(demo_peer_status_decode(encoded, sizeof(encoded), &decoded));
	zassert_equal(demo_peer_status_validate(&decoded, &expected), DEMO_PEER_VALID);
	encoded[0]++;
	zassert_equal(demo_peer_status_decode(encoded, sizeof(encoded), &decoded), -EINVAL);
	status.entity_id = 1u;
	zassert_equal(demo_peer_status_validate(&status, &expected),
		      DEMO_PEER_DUPLICATE_ENTITY);
	valid_peer(&status, &expected);
	status.expected_peer_entity_id = 9u;
	zassert_equal(demo_peer_status_validate(&status, &expected), DEMO_PEER_WRONG_ENTITY);
	valid_peer(&status, &expected);
	status.peer_udp_port++;
	zassert_equal(demo_peer_status_validate(&status, &expected),
		      DEMO_PEER_CONFIG_MISMATCH);
}

ZTEST(demo_protocol, test_fixed_test_object_version_size_and_bytes)
{
	uint8_t object[DEMO_TEST_OBJECT_SIZE];

	demo_test_object_generate(object);
	zassert_true(demo_test_object_verify(object, sizeof(object)));
	zassert_false(demo_test_object_verify(object, sizeof(object) - 1u));
	object[DEMO_TEST_OBJECT_SIZE / 2u] ^= 0x80u;
	zassert_false(demo_test_object_verify(object, sizeof(object)));
	demo_test_object_generate(object);
	object[4] = 0u;
	object[5] = DEMO_TEST_OBJECT_VERSION + 1u;
	zassert_false(demo_test_object_verify(object, sizeof(object)));
}

ZTEST_SUITE(demo_protocol, NULL, NULL, NULL, NULL, NULL);
