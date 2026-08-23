/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_camera.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <soc/soc_caps.h>
#include <hal/cache_hal.h>

LOG_MODULE_REGISTER(eye_camera, CONFIG_LOG_DEFAULT_LEVEL);

#define CAMERA_BUFFER_COUNT  2u
/* Let automatic exposure, gain, and white balance settle before saving a still. */
#define CAMERA_SETTLE_FRAMES 30u
#define CAMERA_TIMEOUT       K_SECONDS(2)

BUILD_ASSERT(DT_HAS_CHOSEN(zephyr_camera), "ESP32-S3-EYE camera must be chosen");
BUILD_ASSERT(!DT_PROP(DT_CHOSEN(zephyr_camera), invert_byte_order),
	     "image object requires VIDEO_PIX_FMT_RGB565 little-endian byte order");
BUILD_ASSERT(CONFIG_VIDEO_BUFFER_POOL_ALIGN >= CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE,
	     "camera buffers must be aligned to an ESP32-S3 data-cache line");
BUILD_ASSERT(IS_ALIGNED(DEMO_IMAGE_PAYLOAD_SIZE, CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE),
	     "camera frame size must contain complete ESP32-S3 data-cache lines");

struct zephyr_camera {
	const struct device *device;
	struct video_format format;
	struct video_buffer *buffers[CAMERA_BUFFER_COUNT];
	struct k_mutex capture_lock;
	bool initialized;
};

static struct zephyr_camera camera;

static void synchronize_dma_frame(void *buffer, size_t size)
{
	unsigned int key = irq_lock();

	/* This is a single-core build.  With interrupts locked and camera DMA
	 * stopped, no other execution context can touch the PSRAM cache while its
	 * stale lines are discarded.
	 */
	cache_hal_freeze(CACHE_TYPE_DATA);
	cache_hal_invalidate_addr((uint32_t)buffer, (uint32_t)size);
	cache_hal_unfreeze(CACHE_TYPE_DATA);
	irq_unlock(key);
}

static int rebuild_buffer_queue(struct zephyr_camera *context)
{
	struct video_buffer *returned = &(struct video_buffer){
		.type = VIDEO_BUF_TYPE_OUTPUT,
	};

	while (video_dequeue(context->device, &returned, K_NO_WAIT) == 0) {
		/* Drain completed or cancelled references before rebuilding the queue. */
	}
	for (size_t i = 0; i < ARRAY_SIZE(context->buffers); ++i) {
		int rc;

		__ASSERT_NO_MSG(context->buffers[i] != NULL);
		context->buffers[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		context->buffers[i]->index = (uint8_t)i;
		context->buffers[i]->bytesused = 0u;
		context->buffers[i]->line_offset = 0u;
		rc = video_enqueue(context->device, context->buffers[i]);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int validate_frame(const struct zephyr_camera *context,
			  const struct video_buffer *frame)
{
	if (frame == NULL || frame->buffer == NULL || frame->size != context->format.size ||
	    frame->bytesused != context->format.size || frame->line_offset != 0u) {
		return -EMSGSIZE;
	}
	return 0;
}

static int stop_stream(struct zephyr_camera *context, bool *streaming, int rc)
{
	if (*streaming) {
		int stop_rc = video_stream_stop(context->device, VIDEO_BUF_TYPE_OUTPUT);

		*streaming = false;
		if (rc == 0) {
			rc = stop_rc;
		}
	}
	return rc;
}

static int zephyr_capture(void *opaque, uint8_t *pixels, size_t capacity,
			  struct demo_camera_frame_info *info)
{
	struct zephyr_camera *context = opaque;
	struct video_buffer *frame = NULL;
	bool streaming = false;
	int rc;

	if (context == NULL || !context->initialized || pixels == NULL || info == NULL ||
	    capacity != DEMO_IMAGE_PAYLOAD_SIZE) {
		return -EINVAL;
	}
	k_mutex_lock(&context->capture_lock, K_FOREVER);
	rc = rebuild_buffer_queue(context);
	if (rc == 0) {
		rc = video_stream_start(context->device, VIDEO_BUF_TYPE_OUTPUT);
		streaming = rc == 0;
	}
	for (uint32_t frame_number = 0u; rc == 0 && frame_number < CAMERA_SETTLE_FRAMES;
	     ++frame_number) {
		frame = &(struct video_buffer){
			.type = VIDEO_BUF_TYPE_OUTPUT,
		};
		rc = video_dequeue(context->device, &frame, CAMERA_TIMEOUT);
		if (rc == 0) {
			rc = validate_frame(context, frame);
		}
		if (rc == 0) {
			rc = video_enqueue(context->device, frame);
		}
	}
	rc = stop_stream(context, &streaming, rc);
	if (rc == 0) {
		rc = rebuild_buffer_queue(context);
	}
	if (rc == 0) {
		rc = video_stream_start(context->device, VIDEO_BUF_TYPE_OUTPUT);
		streaming = rc == 0;
	}
	if (rc == 0) {
		frame = &(struct video_buffer){
			.type = VIDEO_BUF_TYPE_OUTPUT,
		};
		rc = video_dequeue(context->device, &frame, CAMERA_TIMEOUT);
	}
	if (rc == 0) {
		rc = validate_frame(context, frame);
	}
	rc = stop_stream(context, &streaming, rc);
	if (rc == 0) {
		/* Camera GDMA writes the external-RAM buffer without updating the CPU
		 * cache.  The selected image is the first frame after freshly priming
		 * DMA, avoiding the continuous-stream handover that can lose pixels
		 * when Wi-Fi delays the frame-completion callback.
		 */
		synchronize_dma_frame(frame->buffer, frame->bytesused);
	}
	if (rc == 0) {
		memcpy(pixels, frame->buffer, frame->bytesused);
		*info = (struct demo_camera_frame_info){
			.width = context->format.width,
			.height = context->format.height,
			.stride = context->format.pitch,
			.pixel_format = DEMO_IMAGE_PIXEL_FORMAT_RGB565,
			.byte_order = DEMO_IMAGE_BYTE_ORDER_LITTLE_ENDIAN,
			.buffer_size = frame->size,
			.bytes_used = frame->bytesused,
		};
		LOG_INF("captured still after %u settling frames", CAMERA_SETTLE_FRAMES);
	}
	k_mutex_unlock(&context->capture_lock);
	return rc;
}

int demo_camera_init(struct demo_camera_adapter *adapter)
{
	struct video_caps caps = {.type = VIDEO_BUF_TYPE_OUTPUT};
	struct video_format actual = {.type = VIDEO_BUF_TYPE_OUTPUT};
	struct video_control flip = {
		.id = VIDEO_CID_VFLIP,
		.val = 1,
	};
	int rc;

	if (adapter == NULL) {
		return -EINVAL;
	}
	camera.device = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));
	if (!device_is_ready(camera.device)) {
		return -ENODEV;
	}
	rc = video_get_caps(camera.device, &caps);
	if (rc != 0 || caps.min_vbuf_count > CAMERA_BUFFER_COUNT) {
		return rc != 0 ? rc : -ENOBUFS;
	}
	camera.format = (struct video_format){
		.type = VIDEO_BUF_TYPE_OUTPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width = DEMO_IMAGE_WIDTH,
		.height = DEMO_IMAGE_HEIGHT,
		.pitch = DEMO_IMAGE_STRIDE,
	};
	rc = video_set_compose_format(camera.device, &camera.format);
	if (rc != 0) {
		return rc;
	}
	rc = video_get_format(camera.device, &actual);
	if (rc != 0 || actual.pixelformat != VIDEO_PIX_FMT_RGB565 ||
	    actual.width != DEMO_IMAGE_WIDTH || actual.height != DEMO_IMAGE_HEIGHT ||
	    actual.pitch != DEMO_IMAGE_STRIDE || actual.size != DEMO_IMAGE_PAYLOAD_SIZE) {
		return rc != 0 ? rc : -ENOTSUP;
	}
	camera.format = actual;
	rc = video_set_ctrl(camera.device, &flip);
	if (rc != 0) {
		LOG_ERR("failed to set ESP32-S3-EYE vertical flip: %d", rc);
		return rc;
	}
	for (size_t i = 0; i < ARRAY_SIZE(camera.buffers); ++i) {
		camera.buffers[i] = video_buffer_aligned_alloc(
			DEMO_IMAGE_PAYLOAD_SIZE, CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_NO_WAIT);
		if (camera.buffers[i] == NULL) {
			LOG_ERR("failed to allocate external video buffer %u", (unsigned int)i);
			return -ENOMEM;
		}
	}
	k_mutex_init(&camera.capture_lock);
	camera.initialized = true;
	*adapter = (struct demo_camera_adapter){
		.capture = zephyr_capture,
		.context = &camera,
	};
	LOG_INF("camera ready one-shot RGB565-LE %ux%u stride=%u bytes=%u settle=%u",
		camera.format.width, camera.format.height, camera.format.pitch, camera.format.size,
		CAMERA_SETTLE_FRAMES);
	return 0;
}
