/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_IMAGE_H
#define CCSDS_EYE_DEMO_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEMO_IMAGE_MAGIC        0x45594549u /* "EYEI" */
#define DEMO_IMAGE_VERSION      1u
#define DEMO_IMAGE_WIDTH        240u
#define DEMO_IMAGE_HEIGHT       240u
#define DEMO_IMAGE_STRIDE       480u
#define DEMO_IMAGE_PAYLOAD_SIZE 115200u
#define DEMO_IMAGE_HEADER_SIZE  32u
#define DEMO_IMAGE_OBJECT_SIZE  (DEMO_IMAGE_HEADER_SIZE + DEMO_IMAGE_PAYLOAD_SIZE)
#define DEMO_IMAGE_SLOT_COUNT   3u
#define DEMO_IMAGE_SOURCE_PATH  "eye-image-v1.bin"
#define DEMO_IMAGE_DEST_PATH    "eye-received-image-v1.bin"

enum demo_image_pixel_format {
	DEMO_IMAGE_PIXEL_FORMAT_RGB565 = 1,
};

enum demo_image_byte_order {
	DEMO_IMAGE_BYTE_ORDER_LITTLE_ENDIAN = 1,
};

struct demo_image_metadata {
	uint16_t width;
	uint16_t height;
	uint16_t stride;
	enum demo_image_pixel_format pixel_format;
	enum demo_image_byte_order byte_order;
	uint32_t payload_size;
};

struct demo_image_object {
	uint8_t header[DEMO_IMAGE_HEADER_SIZE];
	uint8_t pixels[DEMO_IMAGE_PAYLOAD_SIZE];
};

enum demo_image_owner {
	DEMO_IMAGE_OWNER_NONE = 0,
	DEMO_IMAGE_OWNER_STAGING = 1u << 0,
	DEMO_IMAGE_OWNER_LATEST = 1u << 1,
	DEMO_IMAGE_OWNER_DISPLAY = 1u << 2,
	DEMO_IMAGE_OWNER_TX = 1u << 3,
};

struct demo_image_slot {
	struct demo_image_object *object;
	uint8_t owners;
};

struct demo_image_store {
	struct demo_image_slot *slots;
	size_t slot_count;
};

struct demo_image_source {
	struct demo_image_slot *slot;
	bool open;
};

struct demo_image_receiver {
	struct demo_image_store *store;
	struct demo_image_slot *staging;
	size_t extent;
	bool open;
	bool validated;
};

struct demo_camera_frame_info {
	uint16_t width;
	uint16_t height;
	uint16_t stride;
	enum demo_image_pixel_format pixel_format;
	enum demo_image_byte_order byte_order;
	size_t buffer_size;
	size_t bytes_used;
};

typedef int (*demo_camera_capture_fn)(void *context, uint8_t *pixels, size_t capacity,
				      struct demo_camera_frame_info *info);

struct demo_camera_adapter {
	demo_camera_capture_fn capture;
	void *context;
};

void demo_image_expected_metadata(struct demo_image_metadata *metadata);
int demo_image_encode(struct demo_image_object *object, size_t capacity,
		      const struct demo_image_metadata *metadata, const uint8_t *pixels,
		      size_t pixel_size);
int demo_image_encode_in_place(struct demo_image_object *object, size_t capacity,
			       const struct demo_image_metadata *metadata, size_t pixel_size);
int demo_image_validate(const struct demo_image_object *object, size_t object_size);
int demo_camera_acquire_object(const struct demo_camera_adapter *adapter,
			       struct demo_image_object *object);

void demo_image_store_init(struct demo_image_store *store, struct demo_image_slot *slots,
			   struct demo_image_object *objects, size_t slot_count);
bool demo_image_store_valid(const struct demo_image_store *store);
int demo_image_store_claim_staging(struct demo_image_store *store, struct demo_image_slot **slot);
int demo_image_store_abort_staging(struct demo_image_store *store, struct demo_image_slot *slot);
int demo_image_store_promote(struct demo_image_store *store, struct demo_image_slot *slot);
int demo_image_store_acquire_display(struct demo_image_store *store, struct demo_image_slot **slot);
int demo_image_store_release_display(struct demo_image_store *store, struct demo_image_slot *slot);
int demo_image_store_retain_tx(struct demo_image_store *store, struct demo_image_slot *slot);
int demo_image_store_release_tx(struct demo_image_store *store, struct demo_image_slot *slot);

void demo_image_source_init(struct demo_image_source *source);
int demo_image_source_bind(struct demo_image_source *source, struct demo_image_slot *slot);
int demo_image_source_open(struct demo_image_source *source, const char *path, void **handle,
			   uint32_t *size);
int demo_image_source_read(struct demo_image_source *source, void *handle, uint32_t offset,
			   uint8_t *buffer, size_t length, size_t *read_length);
int demo_image_source_close(struct demo_image_source *source, void *handle);
void demo_image_source_unbind(struct demo_image_source *source);

void demo_image_receiver_init(struct demo_image_receiver *receiver,
			      struct demo_image_store *store);
int demo_image_receiver_open(struct demo_image_receiver *receiver, const char *path,
			     void **handle);
int demo_image_receiver_write(struct demo_image_receiver *receiver, void *handle,
			      uint32_t offset, const uint8_t *buffer, size_t length);
int demo_image_receiver_read(struct demo_image_receiver *receiver, void *handle, uint32_t offset,
			     uint8_t *buffer, size_t length, size_t *read_length);
int demo_image_receiver_close(struct demo_image_receiver *receiver, void *handle);
int demo_image_receiver_validate_complete(struct demo_image_receiver *receiver,
					 const char *path);
int demo_image_receiver_discard(struct demo_image_receiver *receiver, const char *path);
int demo_image_receiver_terminal(struct demo_image_receiver *receiver, bool success);

#endif /* CCSDS_EYE_DEMO_IMAGE_H */
