/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "demo_image.h"
#include "demo_view.h"

static struct demo_image_object image;
static struct demo_image_object incoming_image;
static uint8_t pixels[DEMO_IMAGE_PAYLOAD_SIZE];
static struct demo_image_slot slots[DEMO_IMAGE_SLOT_COUNT];
static struct demo_image_object store_objects[DEMO_IMAGE_SLOT_COUNT];

static void make_valid(struct demo_image_object *object, uint8_t seed)
{
	struct demo_image_metadata metadata;

	for (size_t i = 0; i < sizeof(pixels); ++i) {
		pixels[i] = (uint8_t)(i + seed);
	}
	demo_image_expected_metadata(&metadata);
	zassert_ok(demo_image_encode(object, sizeof(*object), &metadata, pixels, sizeof(pixels)));
}

ZTEST(demo_image, test_valid_image_object_construction_and_validation)
{
	make_valid(&image, 3u);
	zassert_ok(demo_image_validate(&image, sizeof(image)));
	zassert_mem_equal(image.pixels, pixels, sizeof(pixels));
}

ZTEST(demo_image, test_rejects_wrong_metadata_fields)
{
	static const size_t offsets[] = {5u, 9u, 11u, 12u, 13u, 15u, 19u};

	for (size_t i = 0; i < ARRAY_SIZE(offsets); ++i) {
		make_valid(&image, (uint8_t)i);
		image.header[offsets[i]] ^= 1u;
		zassert_equal(demo_image_validate(&image, sizeof(image)), -EINVAL,
			      "metadata offset %zu accepted", offsets[i]);
	}
}

ZTEST(demo_image, test_rejects_truncated_and_oversized_payloads)
{
	struct demo_image_metadata metadata;

	demo_image_expected_metadata(&metadata);
	zassert_equal(
		demo_image_encode(&image, sizeof(image), &metadata, pixels, sizeof(pixels) - 1u),
		-EINVAL);
	zassert_equal(
		demo_image_encode(&image, sizeof(image), &metadata, pixels, sizeof(pixels) + 1u),
		-EINVAL);
	make_valid(&image, 0u);
	zassert_equal(demo_image_validate(&image, sizeof(image) - 1u), -EINVAL);
	zassert_equal(demo_image_validate(&image, sizeof(image) + 1u), -EINVAL);
}

struct fake_camera {
	int result;
	struct demo_camera_frame_info info;
	uint8_t fill;
};

static int fake_capture(void *context, uint8_t *destination, size_t capacity,
			struct demo_camera_frame_info *info)
{
	struct fake_camera *fake = context;

	if (fake->result != 0) {
		return fake->result;
	}
	memset(destination, fake->fill, capacity);
	*info = fake->info;
	return 0;
}

static struct fake_camera valid_fake(void)
{
	return (struct fake_camera){
		.info =
			{
				.width = DEMO_IMAGE_WIDTH,
				.height = DEMO_IMAGE_HEIGHT,
				.stride = DEMO_IMAGE_STRIDE,
				.pixel_format = DEMO_IMAGE_PIXEL_FORMAT_RGB565,
				.byte_order = DEMO_IMAGE_BYTE_ORDER_LITTLE_ENDIAN,
				.buffer_size = DEMO_IMAGE_PAYLOAD_SIZE,
				.bytes_used = DEMO_IMAGE_PAYLOAD_SIZE,
			},
		.fill = 0xa5u,
	};
}

ZTEST(demo_image, test_fake_camera_validates_reported_frame)
{
	struct fake_camera fake = valid_fake();
	struct demo_camera_adapter adapter = {.capture = fake_capture, .context = &fake};

	zassert_ok(demo_camera_acquire_object(&adapter, &image));
	zassert_ok(demo_image_validate(&image, sizeof(image)));
	fake.info.stride--;
	zassert_equal(demo_camera_acquire_object(&adapter, &image), -EINVAL);
	fake = valid_fake();
	fake.info.bytes_used--;
	zassert_equal(demo_camera_acquire_object(&adapter, &image), -EMSGSIZE);
	fake = valid_fake();
	fake.info.buffer_size++;
	zassert_equal(demo_camera_acquire_object(&adapter, &image), -EMSGSIZE);
}

ZTEST(demo_image, test_capture_failure_preserves_latest_and_success_promotes_atomically)
{
	struct demo_image_store store;
	struct demo_image_slot *first;
	struct demo_image_slot *staging;
	struct fake_camera fake = valid_fake();
	struct demo_camera_adapter adapter = {.capture = fake_capture, .context = &fake};

	demo_image_store_init(&store, slots, store_objects, ARRAY_SIZE(slots));
	zassert_ok(demo_image_store_claim_staging(&store, &first));
	make_valid(first->object, 1u);
	zassert_ok(demo_image_store_promote(&store, first));
	zassert_ok(demo_image_store_claim_staging(&store, &staging));
	fake.result = -EIO;
	zassert_equal(demo_camera_acquire_object(&adapter, staging->object), -EIO);
	zassert_ok(demo_image_store_abort_staging(&store, staging));
	zassert_true((first->owners & DEMO_IMAGE_OWNER_LATEST) != 0u);

	zassert_ok(demo_image_store_claim_staging(&store, &staging));
	fake = valid_fake();
	zassert_ok(demo_camera_acquire_object(&adapter, staging->object));
	zassert_ok(demo_image_store_promote(&store, staging));
	zassert_false((first->owners & DEMO_IMAGE_OWNER_LATEST) != 0u);
	zassert_true((staging->owners & DEMO_IMAGE_OWNER_LATEST) != 0u);
	zassert_true(demo_image_store_valid(&store));
}

ZTEST(demo_image, test_ownership_rejects_invalid_transitions_and_bounds_busy)
{
	struct demo_image_store store;
	struct demo_image_slot *latest;
	struct demo_image_slot *display;
	struct demo_image_slot *staging;

	demo_image_store_init(&store, slots, store_objects, ARRAY_SIZE(slots));
	zassert_equal(demo_image_store_acquire_display(&store, &display), -ENOENT);
	zassert_ok(demo_image_store_claim_staging(&store, &latest));
	make_valid(latest->object, 4u);
	zassert_ok(demo_image_store_promote(&store, latest));
	zassert_equal(demo_image_store_promote(&store, latest), -EINVAL);
	zassert_ok(demo_image_store_acquire_display(&store, &display));
	zassert_equal(display, latest);
	zassert_ok(demo_image_store_retain_tx(&store, latest));
	zassert_ok(demo_image_store_claim_staging(&store, &staging));
	zassert_equal(demo_image_store_claim_staging(&store, &staging), -EBUSY);
	zassert_equal(demo_image_store_release_display(&store, staging), -EINVAL);
	zassert_ok(demo_image_store_abort_staging(&store, staging));
	zassert_ok(demo_image_store_release_tx(&store, latest));
	zassert_ok(demo_image_store_release_display(&store, display));

	slots[0].owners = DEMO_IMAGE_OWNER_STAGING | DEMO_IMAGE_OWNER_DISPLAY;
	zassert_false(demo_image_store_valid(&store));
}

ZTEST(demo_image, test_repeated_send_terminal_cycles_recycle_image_slots)
{
	struct demo_image_store store;
	struct demo_image_source source;

	demo_image_store_init(&store, slots, store_objects, ARRAY_SIZE(slots));
	demo_image_source_init(&source);
	for (uint8_t cycle = 0u; cycle < 8u; ++cycle) {
		struct demo_image_slot *staging;
		void *handle;
		uint32_t size;

		zassert_ok(demo_image_store_claim_staging(&store, &staging),
			   "staging unavailable on cycle %u", cycle);
		make_valid(staging->object, cycle);
		zassert_ok(demo_image_store_promote(&store, staging));
		zassert_ok(demo_image_store_retain_tx(&store, staging));
		zassert_ok(demo_image_source_bind(&source, staging));
		zassert_ok(demo_image_source_open(&source, DEMO_IMAGE_SOURCE_PATH, &handle, &size));
		zassert_equal(size, DEMO_IMAGE_OBJECT_SIZE);
		zassert_ok(demo_image_source_close(&source, handle));
		demo_image_source_unbind(&source);
		zassert_ok(demo_image_store_release_tx(&store, staging));
		zassert_true(demo_image_store_valid(&store));
	}
}

ZTEST(demo_image, test_cfdp_source_reads_exact_image_with_offsets_and_partials)
{
	struct demo_image_store store;
	struct demo_image_source source;
	struct demo_image_slot *latest;
	uint8_t buffer[37];
	void *handle;
	uint32_t size;
	size_t read_length;

	demo_image_store_init(&store, slots, store_objects, ARRAY_SIZE(slots));
	zassert_ok(demo_image_store_claim_staging(&store, &latest));
	make_valid(latest->object, 9u);
	zassert_ok(demo_image_store_promote(&store, latest));
	zassert_ok(demo_image_store_retain_tx(&store, latest));
	demo_image_source_init(&source);
	zassert_ok(demo_image_source_bind(&source, latest));
	zassert_equal(demo_image_source_open(&source, "wrong.bin", &handle, &size), -EINVAL);
	zassert_ok(demo_image_source_open(&source, DEMO_IMAGE_SOURCE_PATH, &handle, &size));
	zassert_equal(size, DEMO_IMAGE_OBJECT_SIZE);
	zassert_ok(demo_image_source_read(&source, handle, 29u, buffer, sizeof(buffer),
					  &read_length));
	zassert_equal(read_length, sizeof(buffer));
	zassert_mem_equal(buffer, &((uint8_t *)latest->object)[29], sizeof(buffer));
	zassert_ok(demo_image_source_read(&source, handle, DEMO_IMAGE_OBJECT_SIZE - 11u,
					  buffer, sizeof(buffer), &read_length));
	zassert_equal(read_length, 11u);
	zassert_mem_equal(buffer, &((uint8_t *)latest->object)[DEMO_IMAGE_OBJECT_SIZE - 11u],
			  11u);
	zassert_ok(demo_image_source_read(&source, handle, DEMO_IMAGE_OBJECT_SIZE, buffer,
					  sizeof(buffer), &read_length));
	zassert_equal(read_length, 0u);
	zassert_equal(demo_image_source_read(&source, handle, DEMO_IMAGE_OBJECT_SIZE + 1u,
					     buffer, sizeof(buffer), &read_length),
		      -EINVAL);
	zassert_ok(demo_image_source_close(&source, handle));
	zassert_true((latest->owners & DEMO_IMAGE_OWNER_TX) != 0u,
		     "source close must not release terminal ownership");
	demo_image_source_unbind(&source);
	zassert_ok(demo_image_store_release_tx(&store, latest));
}

ZTEST(demo_image, test_cfdp_receive_bounds_exact_size_and_atomic_promotion)
{
	struct demo_image_store store;
	struct demo_image_receiver receiver;
	struct demo_image_slot *previous;
	struct demo_image_slot *shown;
	void *handle;
	const size_t split = 997u;
	uint8_t checksum_buffer[31];
	size_t read_length;

	demo_image_store_init(&store, slots, store_objects, ARRAY_SIZE(slots));
	zassert_ok(demo_image_store_claim_staging(&store, &previous));
	make_valid(previous->object, 1u);
	zassert_ok(demo_image_store_promote(&store, previous));
	make_valid(&incoming_image, 2u);
	demo_image_receiver_init(&receiver, &store);
	zassert_equal(demo_image_receiver_open(&receiver, "wrong.bin", &handle), -EINVAL);
	zassert_ok(demo_image_receiver_open(&receiver, DEMO_IMAGE_DEST_PATH, &handle));
	zassert_equal(demo_image_receiver_write(&receiver, handle, DEMO_IMAGE_OBJECT_SIZE,
						 incoming_image.header, 1u),
		      -EFBIG);
	zassert_equal(demo_image_receiver_write(&receiver, handle,
						 DEMO_IMAGE_OBJECT_SIZE - 1u,
						 incoming_image.header, 2u),
		      -EFBIG);
	zassert_ok(demo_image_receiver_write(&receiver, handle, split,
					     &((uint8_t *)&incoming_image)[split],
					     DEMO_IMAGE_OBJECT_SIZE - split));
	zassert_ok(demo_image_receiver_write(&receiver, handle, 0u, (uint8_t *)&incoming_image,
					     split));
	zassert_ok(demo_image_receiver_read(&receiver, handle, 29u, checksum_buffer,
					    sizeof(checksum_buffer), &read_length));
	zassert_equal(read_length, sizeof(checksum_buffer));
	zassert_mem_equal(checksum_buffer, &((uint8_t *)&incoming_image)[29],
			  sizeof(checksum_buffer));
	zassert_ok(demo_image_receiver_read(&receiver, handle, DEMO_IMAGE_OBJECT_SIZE,
					    checksum_buffer, sizeof(checksum_buffer), &read_length));
	zassert_equal(read_length, 0u);
	zassert_ok(demo_image_receiver_close(&receiver, handle));
	zassert_ok(demo_image_receiver_validate_complete(&receiver, DEMO_IMAGE_DEST_PATH));
	zassert_ok(demo_image_store_acquire_display(&store, &shown));
	zassert_equal(shown, previous, "validated staging replaced latest before terminal success");
	zassert_ok(demo_image_store_release_display(&store, shown));
	zassert_ok(demo_image_receiver_terminal(&receiver, true));
	zassert_ok(demo_image_store_acquire_display(&store, &shown));
	zassert_not_equal(shown, previous);
	zassert_mem_equal(shown->object, &incoming_image, sizeof(incoming_image));
	zassert_ok(demo_image_store_release_display(&store, shown));
}

ZTEST(demo_image, test_cfdp_receive_corrupt_and_incomplete_preserve_latest)
{
	struct demo_image_store store;
	struct demo_image_receiver receiver;
	struct demo_image_slot *previous;
	struct demo_image_slot *shown;
	void *handle;

	demo_image_store_init(&store, slots, store_objects, ARRAY_SIZE(slots));
	zassert_ok(demo_image_store_claim_staging(&store, &previous));
	make_valid(previous->object, 3u);
	zassert_ok(demo_image_store_promote(&store, previous));
	make_valid(&incoming_image, 4u);
	incoming_image.header[19] ^= 1u;
	demo_image_receiver_init(&receiver, &store);
	zassert_ok(demo_image_receiver_open(&receiver, DEMO_IMAGE_DEST_PATH, &handle));
	zassert_ok(demo_image_receiver_write(&receiver, handle, 0u, (uint8_t *)&incoming_image,
					     sizeof(incoming_image)));
	zassert_ok(demo_image_receiver_close(&receiver, handle));
	zassert_equal(demo_image_receiver_validate_complete(&receiver, DEMO_IMAGE_DEST_PATH),
		      -EBADMSG);
	zassert_ok(demo_image_receiver_terminal(&receiver, false));
	zassert_ok(demo_image_store_acquire_display(&store, &shown));
	zassert_equal(shown, previous);
	zassert_ok(demo_image_store_release_display(&store, shown));

	make_valid(&incoming_image, 5u);
	zassert_ok(demo_image_receiver_open(&receiver, DEMO_IMAGE_DEST_PATH, &handle));
	zassert_ok(demo_image_receiver_write(&receiver, handle, 0u, (uint8_t *)&incoming_image,
					     sizeof(incoming_image) - 1u));
	zassert_ok(demo_image_receiver_close(&receiver, handle));
	zassert_equal(demo_image_receiver_validate_complete(&receiver, DEMO_IMAGE_DEST_PATH),
		      -EBADMSG);
	zassert_ok(demo_image_receiver_terminal(&receiver, false));
	zassert_ok(demo_image_store_acquire_display(&store, &shown));
	zassert_equal(shown, previous);
	zassert_ok(demo_image_store_release_display(&store, shown));
}

ZTEST(demo_image, test_display_and_tx_ownership_coexist_until_explicit_terminal_release)
{
	struct demo_image_store store;
	struct demo_image_slot *latest;
	struct demo_image_slot *display;

	demo_image_store_init(&store, slots, store_objects, ARRAY_SIZE(slots));
	zassert_ok(demo_image_store_claim_staging(&store, &latest));
	make_valid(latest->object, 6u);
	zassert_ok(demo_image_store_promote(&store, latest));
	zassert_ok(demo_image_store_retain_tx(&store, latest));
	zassert_ok(demo_image_store_acquire_display(&store, &display));
	zassert_equal(display, latest);
	zassert_equal(latest->owners,
		      DEMO_IMAGE_OWNER_LATEST | DEMO_IMAGE_OWNER_TX | DEMO_IMAGE_OWNER_DISPLAY);
	zassert_ok(demo_image_store_release_display(&store, display));
	zassert_true((latest->owners & DEMO_IMAGE_OWNER_TX) != 0u);
	zassert_ok(demo_image_store_release_tx(&store, latest));
	zassert_equal(latest->owners, DEMO_IMAGE_OWNER_LATEST);
}

ZTEST(demo_image, test_show_toggle_no_image_and_actions_from_image_view)
{
	struct demo_view_model model;

	demo_view_init(&model);
	zassert_equal(demo_view_toggle_show(&model, false), DEMO_SHOW_NO_IMAGE);
	zassert_equal(model.view, DEMO_VIEW_PROTOCOL);
	zassert_equal(demo_view_toggle_show(&model, true), DEMO_SHOW_IMAGE);
	zassert_equal(model.view, DEMO_VIEW_IMAGE);
	zassert_equal(demo_view_toggle_show(&model, true), DEMO_SHOW_PROTOCOL);
	zassert_equal(model.view, DEMO_VIEW_PROTOCOL);

	/* A and B use the same presentation transition before their distinct queues.
	 */
	zassert_equal(demo_view_toggle_show(&model, true), DEMO_SHOW_IMAGE);
	zassert_true(demo_view_prepare_action(&model));
	zassert_equal(model.view, DEMO_VIEW_PROTOCOL);
	zassert_equal(demo_view_toggle_show(&model, true), DEMO_SHOW_IMAGE);
	zassert_true(demo_view_prepare_action(&model));
	zassert_equal(model.view, DEMO_VIEW_PROTOCOL);
	zassert_false(demo_view_prepare_action(&model));

	zassert_true(demo_view_enter_link(&model));
	zassert_equal(model.view, DEMO_VIEW_LINK);
	zassert_false(demo_view_enter_link(&model));
	zassert_true(demo_view_leave_link(&model));
	zassert_equal(model.view, DEMO_VIEW_PROTOCOL);
	zassert_false(demo_view_leave_link(&model));
}

ZTEST_SUITE(demo_image, NULL, NULL, NULL, NULL, NULL);
