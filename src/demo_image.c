/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_image.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(sizeof(struct demo_image_object) == DEMO_IMAGE_OBJECT_SIZE,
	     "image object layout must remain fixed");

enum image_header_offset {
	IMAGE_MAGIC = 0,
	IMAGE_VERSION = 4,
	IMAGE_HEADER_LENGTH = 6,
	IMAGE_WIDTH = 8,
	IMAGE_HEIGHT = 10,
	IMAGE_PIXEL_FORMAT = 12,
	IMAGE_BYTE_ORDER = 13,
	IMAGE_STRIDE = 14,
	IMAGE_PAYLOAD_SIZE = 16,
};

static bool metadata_valid(const struct demo_image_metadata *metadata)
{
	return metadata != NULL && metadata->width == DEMO_IMAGE_WIDTH &&
	       metadata->height == DEMO_IMAGE_HEIGHT && metadata->stride == DEMO_IMAGE_STRIDE &&
	       metadata->pixel_format == DEMO_IMAGE_PIXEL_FORMAT_RGB565 &&
	       metadata->byte_order == DEMO_IMAGE_BYTE_ORDER_LITTLE_ENDIAN &&
	       metadata->payload_size == DEMO_IMAGE_PAYLOAD_SIZE;
}

void demo_image_expected_metadata(struct demo_image_metadata *metadata)
{
	__ASSERT_NO_MSG(metadata != NULL);
	*metadata = (struct demo_image_metadata){
		.width = DEMO_IMAGE_WIDTH,
		.height = DEMO_IMAGE_HEIGHT,
		.stride = DEMO_IMAGE_STRIDE,
		.pixel_format = DEMO_IMAGE_PIXEL_FORMAT_RGB565,
		.byte_order = DEMO_IMAGE_BYTE_ORDER_LITTLE_ENDIAN,
		.payload_size = DEMO_IMAGE_PAYLOAD_SIZE,
	};
}

static int encode_header(struct demo_image_object *object, size_t capacity,
			 const struct demo_image_metadata *metadata, size_t pixel_size)
{
	if (object == NULL || capacity < sizeof(*object) || !metadata_valid(metadata) ||
	    pixel_size != DEMO_IMAGE_PAYLOAD_SIZE) {
		return -EINVAL;
	}

	memset(object->header, 0, sizeof(object->header));
	sys_put_be32(DEMO_IMAGE_MAGIC, &object->header[IMAGE_MAGIC]);
	sys_put_be16(DEMO_IMAGE_VERSION, &object->header[IMAGE_VERSION]);
	sys_put_be16(DEMO_IMAGE_HEADER_SIZE, &object->header[IMAGE_HEADER_LENGTH]);
	sys_put_be16(metadata->width, &object->header[IMAGE_WIDTH]);
	sys_put_be16(metadata->height, &object->header[IMAGE_HEIGHT]);
	object->header[IMAGE_PIXEL_FORMAT] = (uint8_t)metadata->pixel_format;
	object->header[IMAGE_BYTE_ORDER] = (uint8_t)metadata->byte_order;
	sys_put_be16(metadata->stride, &object->header[IMAGE_STRIDE]);
	sys_put_be32(metadata->payload_size, &object->header[IMAGE_PAYLOAD_SIZE]);
	return 0;
}

int demo_image_encode(struct demo_image_object *object, size_t capacity,
		      const struct demo_image_metadata *metadata, const uint8_t *pixels,
		      size_t pixel_size)
{
	int rc = encode_header(object, capacity, metadata, pixel_size);

	if (rc != 0 || pixels == NULL) {
		return -EINVAL;
	}
	memcpy(object->pixels, pixels, pixel_size);
	return demo_image_validate(object, sizeof(*object));
}

int demo_image_encode_in_place(struct demo_image_object *object, size_t capacity,
			       const struct demo_image_metadata *metadata, size_t pixel_size)
{
	int rc = encode_header(object, capacity, metadata, pixel_size);

	return rc == 0 ? demo_image_validate(object, sizeof(*object)) : rc;
}

int demo_image_validate(const struct demo_image_object *object, size_t object_size)
{
	if (object == NULL || object_size != sizeof(*object) ||
	    sys_get_be32(&object->header[IMAGE_MAGIC]) != DEMO_IMAGE_MAGIC ||
	    sys_get_be16(&object->header[IMAGE_VERSION]) != DEMO_IMAGE_VERSION ||
	    sys_get_be16(&object->header[IMAGE_HEADER_LENGTH]) != DEMO_IMAGE_HEADER_SIZE ||
	    sys_get_be16(&object->header[IMAGE_WIDTH]) != DEMO_IMAGE_WIDTH ||
	    sys_get_be16(&object->header[IMAGE_HEIGHT]) != DEMO_IMAGE_HEIGHT ||
	    object->header[IMAGE_PIXEL_FORMAT] != DEMO_IMAGE_PIXEL_FORMAT_RGB565 ||
	    object->header[IMAGE_BYTE_ORDER] != DEMO_IMAGE_BYTE_ORDER_LITTLE_ENDIAN ||
	    sys_get_be16(&object->header[IMAGE_STRIDE]) != DEMO_IMAGE_STRIDE ||
	    sys_get_be32(&object->header[IMAGE_PAYLOAD_SIZE]) != DEMO_IMAGE_PAYLOAD_SIZE) {
		return -EINVAL;
	}

	for (size_t i = IMAGE_PAYLOAD_SIZE + sizeof(uint32_t); i < DEMO_IMAGE_HEADER_SIZE; ++i) {
		if (object->header[i] != 0u) {
			return -EINVAL;
		}
	}
	return 0;
}

int demo_camera_acquire_object(const struct demo_camera_adapter *adapter,
			       struct demo_image_object *object)
{
	struct demo_camera_frame_info info = {0};
	struct demo_image_metadata metadata;
	int rc;

	if (adapter == NULL || adapter->capture == NULL || object == NULL) {
		return -EINVAL;
	}
	rc = adapter->capture(adapter->context, object->pixels, sizeof(object->pixels), &info);
	if (rc != 0) {
		return rc;
	}
	metadata = (struct demo_image_metadata){
		.width = info.width,
		.height = info.height,
		.stride = info.stride,
		.pixel_format = info.pixel_format,
		.byte_order = info.byte_order,
		.payload_size = (uint32_t)info.bytes_used,
	};
	if (info.buffer_size != DEMO_IMAGE_PAYLOAD_SIZE ||
	    info.bytes_used != DEMO_IMAGE_PAYLOAD_SIZE) {
		return -EMSGSIZE;
	}
	return demo_image_encode_in_place(object, sizeof(*object), &metadata, info.bytes_used);
}

void demo_image_store_init(struct demo_image_store *store, struct demo_image_slot *slots,
			   struct demo_image_object *objects, size_t slot_count)
{
	__ASSERT_NO_MSG(store != NULL);
	__ASSERT_NO_MSG(slots != NULL);
	__ASSERT_NO_MSG(objects != NULL);
	__ASSERT_NO_MSG(slot_count >= DEMO_IMAGE_SLOT_COUNT);
	memset(slots, 0, sizeof(*slots) * slot_count);
	for (size_t i = 0; i < slot_count; ++i) {
		slots[i].object = &objects[i];
	}
	store->slots = slots;
	store->slot_count = slot_count;
}

static bool slot_belongs(const struct demo_image_store *store, const struct demo_image_slot *slot)
{
	return store != NULL && store->slots != NULL && slot >= store->slots &&
	       slot < store->slots + store->slot_count;
}

bool demo_image_store_valid(const struct demo_image_store *store)
{
	size_t latest = 0u;
	size_t staging = 0u;

	if (store == NULL || store->slots == NULL || store->slot_count < DEMO_IMAGE_SLOT_COUNT) {
		return false;
	}
	for (size_t i = 0; i < store->slot_count; ++i) {
		const uint8_t owners = store->slots[i].owners;

		if (store->slots[i].object == NULL ||
		    (owners & ~(DEMO_IMAGE_OWNER_STAGING | DEMO_IMAGE_OWNER_LATEST |
				DEMO_IMAGE_OWNER_DISPLAY | DEMO_IMAGE_OWNER_TX)) != 0u ||
		    ((owners & DEMO_IMAGE_OWNER_STAGING) != 0u &&
		     owners != DEMO_IMAGE_OWNER_STAGING)) {
			return false;
		}
		latest += (owners & DEMO_IMAGE_OWNER_LATEST) != 0u;
		staging += (owners & DEMO_IMAGE_OWNER_STAGING) != 0u;
	}
	return latest <= 1u && staging <= 1u;
}

static void assert_store(const struct demo_image_store *store)
{
	__ASSERT_NO_MSG(demo_image_store_valid(store));
}

int demo_image_store_claim_staging(struct demo_image_store *store, struct demo_image_slot **slot)
{
	if (slot == NULL || !demo_image_store_valid(store)) {
		return -EINVAL;
	}
	for (size_t i = 0; i < store->slot_count; ++i) {
		if ((store->slots[i].owners & DEMO_IMAGE_OWNER_STAGING) != 0u) {
			return -EBUSY;
		}
	}
	for (size_t i = 0; i < store->slot_count; ++i) {
		if (store->slots[i].owners == DEMO_IMAGE_OWNER_NONE) {
			store->slots[i].owners = DEMO_IMAGE_OWNER_STAGING;
			*slot = &store->slots[i];
			assert_store(store);
			return 0;
		}
	}
	return -EBUSY;
}

int demo_image_store_abort_staging(struct demo_image_store *store, struct demo_image_slot *slot)
{
	if (!slot_belongs(store, slot) || slot->owners != DEMO_IMAGE_OWNER_STAGING) {
		return -EINVAL;
	}
	slot->owners = DEMO_IMAGE_OWNER_NONE;
	assert_store(store);
	return 0;
}

int demo_image_store_promote(struct demo_image_store *store, struct demo_image_slot *slot)
{
	if (!slot_belongs(store, slot) || slot->owners != DEMO_IMAGE_OWNER_STAGING ||
	    demo_image_validate(slot->object, sizeof(*slot->object)) != 0) {
		return -EINVAL;
	}
	for (size_t i = 0; i < store->slot_count; ++i) {
		store->slots[i].owners &= (uint8_t)~DEMO_IMAGE_OWNER_LATEST;
	}
	slot->owners = DEMO_IMAGE_OWNER_LATEST;
	assert_store(store);
	return 0;
}

int demo_image_store_acquire_display(struct demo_image_store *store, struct demo_image_slot **slot)
{
	if (slot == NULL || !demo_image_store_valid(store)) {
		return -EINVAL;
	}
	for (size_t i = 0; i < store->slot_count; ++i) {
		if ((store->slots[i].owners & DEMO_IMAGE_OWNER_DISPLAY) != 0u) {
			return -EBUSY;
		}
	}
	for (size_t i = 0; i < store->slot_count; ++i) {
		if ((store->slots[i].owners & DEMO_IMAGE_OWNER_LATEST) != 0u) {
			store->slots[i].owners |= DEMO_IMAGE_OWNER_DISPLAY;
			*slot = &store->slots[i];
			assert_store(store);
			return 0;
		}
	}
	return -ENOENT;
}

int demo_image_store_release_display(struct demo_image_store *store, struct demo_image_slot *slot)
{
	if (!slot_belongs(store, slot) || (slot->owners & DEMO_IMAGE_OWNER_DISPLAY) == 0u) {
		return -EINVAL;
	}
	slot->owners &= (uint8_t)~DEMO_IMAGE_OWNER_DISPLAY;
	assert_store(store);
	return 0;
}

int demo_image_store_retain_tx(struct demo_image_store *store, struct demo_image_slot *slot)
{
	if (!slot_belongs(store, slot) || slot->owners == DEMO_IMAGE_OWNER_NONE ||
	    (slot->owners & DEMO_IMAGE_OWNER_STAGING) != 0u ||
	    (slot->owners & DEMO_IMAGE_OWNER_TX) != 0u) {
		return -EINVAL;
	}
	slot->owners |= DEMO_IMAGE_OWNER_TX;
	assert_store(store);
	return 0;
}

int demo_image_store_release_tx(struct demo_image_store *store, struct demo_image_slot *slot)
{
	if (!slot_belongs(store, slot) || (slot->owners & DEMO_IMAGE_OWNER_TX) == 0u) {
		return -EINVAL;
	}
	slot->owners &= (uint8_t)~DEMO_IMAGE_OWNER_TX;
	assert_store(store);
	return 0;
}
