/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "demo_image.h"
#include "demo_view.h"

static struct demo_image_object image;
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
}

ZTEST_SUITE(demo_image, NULL, NULL, NULL, NULL, NULL);
