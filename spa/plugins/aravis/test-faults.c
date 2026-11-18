/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"

/* Linked only into the fault-injection plugin used by the loopback test. */
static bool fault(const char *name)
{
	const char *selected = getenv("PIPEWIREAO_TEST_ARAVIS_FAULT");
	return selected != NULL && strcmp(selected, name) == 0;
}

ArvStream *__real_arv_camera_create_stream(ArvCamera *, ArvStreamCallback,
		void *, GDestroyNotify, GError **);
ArvStream *__wrap_arv_camera_create_stream(ArvCamera *camera,
		ArvStreamCallback callback, void *data, GDestroyNotify destroy, GError **error)
{
	return fault("stream") ? NULL :
			__real_arv_camera_create_stream(camera, callback, data, destroy, error);
}

guint __real_arv_camera_get_payload(ArvCamera *, GError **);
guint __wrap_arv_camera_get_payload(ArvCamera *camera, GError **error)
{
	return fault("payload") ? 0 : __real_arv_camera_get_payload(camera, error);
}

void __real_arv_camera_get_region(ArvCamera *, gint *, gint *, gint *, gint *, GError **);
void __wrap_arv_camera_get_region(ArvCamera *camera, gint *x, gint *y, gint *w, gint *h,
		GError **error)
{
	__real_arv_camera_get_region(camera, x, y, w, h, error);
	if (fault("region"))
		*w = 0;
}

const char *__real_arv_camera_get_pixel_format_as_string(ArvCamera *, GError **);
const char *__wrap_arv_camera_get_pixel_format_as_string(ArvCamera *camera, GError **error)
{
	return fault("pixel-format") ? "RGB8" :
			__real_arv_camera_get_pixel_format_as_string(camera, error);
}

const char *__real_arv_camera_get_model_name(ArvCamera *, GError **);
const char *__wrap_arv_camera_get_model_name(ArvCamera *camera, GError **error)
{
	if (fault("model")) {
		g_set_error_literal(error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
				"fixture model failure");
		return NULL;
	}
	return __real_arv_camera_get_model_name(camera, error);
}

double __real_arv_camera_get_frame_rate(ArvCamera *, GError **);
double __wrap_arv_camera_get_frame_rate(ArvCamera *camera, GError **error)
{
	if (fault("rate")) {
		g_set_error_literal(error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
				"fixture rate failure");
		return 0;
	}
	if (fault("tiny-rate"))
		return 0.0001;
	if (fault("large-rate"))
		return 4000000.001;
	return __real_arv_camera_get_frame_rate(camera, error);
}

gboolean __real_arv_camera_start_acquisition(ArvCamera *, GError **);
gboolean __wrap_arv_camera_start_acquisition(ArvCamera *camera, GError **error)
{
	return fault("start") ? FALSE : __real_arv_camera_start_acquisition(camera, error);
}

gboolean __real_arv_camera_stop_acquisition(ArvCamera *, GError **);
gboolean __wrap_arv_camera_stop_acquisition(ArvCamera *camera, GError **error)
{
	static bool failed;
	if (!failed && fault("stop")) {
		failed = true;
		return FALSE;
	}
	return __real_arv_camera_stop_acquisition(camera, error);
}

int __real_aravis_camera_announce(struct aravis_camera *, void *, uint64_t,
		void *, ArvBuffer **);
int __wrap_aravis_camera_announce(struct aravis_camera *camera, void *memory,
		uint64_t size, void *user_data, ArvBuffer **buffer)
{
	static unsigned int announcements;
	if (fault("announce") && ++announcements == 2) {
		announcements = 0;
		return -EIO;
	}
	return __real_aravis_camera_announce(camera, memory, size, user_data, buffer);
}

int __real_aravis_camera_queue(struct aravis_camera *, ArvBuffer *);
int __wrap_aravis_camera_queue(struct aravis_camera *camera, ArvBuffer *buffer)
{
	return fault("queue") ? -EIO : __real_aravis_camera_queue(camera, buffer);
}

int __real_aravis_camera_refresh_info(struct aravis_camera *);
int __wrap_aravis_camera_refresh_info(struct aravis_camera *camera)
{
	return fault("refresh") ? -EIO : __real_aravis_camera_refresh_info(camera);
}

ArvBufferStatus __real_arv_buffer_get_status(ArvBuffer *);
ArvBufferStatus __wrap_arv_buffer_get_status(ArvBuffer *buffer)
{
	return fault("incomplete") ? ARV_BUFFER_STATUS_MISSING_PACKETS :
			__real_arv_buffer_get_status(buffer);
}

const void *__real_arv_buffer_get_image_data(ArvBuffer *, size_t *);
const void *__wrap_arv_buffer_get_image_data(ArvBuffer *buffer, size_t *size)
{
	return fault("invalid-image") ? NULL : __real_arv_buffer_get_image_data(buffer, size);
}

int __real_aravis_camera_revoke(struct aravis_camera *, ArvBuffer **);
int __wrap_aravis_camera_revoke(struct aravis_camera *camera, ArvBuffer **buffer)
{
	int result = __real_aravis_camera_revoke(camera, buffer);

	return fault("revoke") ? -EIO : result;
}

const char *__real_arv_camera_get_device_serial_number(ArvCamera *, GError **);
const char *__wrap_arv_camera_get_device_serial_number(ArvCamera *camera, GError **error)
{
	if (fault("serial")) {
		g_set_error_literal(error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
				"fixture serial failure");
		return NULL;
	}
	return __real_arv_camera_get_device_serial_number(camera, error);
}

gint64 __real_arv_gc_integer_get_min(ArvGcInteger *, GError **);
gint64 __wrap_arv_gc_integer_get_min(ArvGcInteger *node, GError **error)
{
	if (fault("range")) {
		g_set_error_literal(error, ARV_DEVICE_ERROR, ARV_DEVICE_ERROR_PROTOCOL_ERROR,
				"fixture integer minimum failure");
		return 0;
	}
	return __real_arv_gc_integer_get_min(node, error);
}
