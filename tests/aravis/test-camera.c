/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/utils/defs.h>

#define N_BUFFERS 8u
#define N_FRAMES 16u

struct test_buffer {
	ArvBuffer *buffer;
	void *memory;
};

static int64_t monotonic_nsec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	return (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
}

static int capture_frames(struct aravis_camera *camera, uint32_t requested,
		uint64_t *empty_polls)
{
	int64_t deadline = monotonic_nsec() + 10 * 1000000000ll;
	uint32_t completed = 0;

	while (completed < requested) {
		struct aravis_camera_completion completion;
		int status = aravis_camera_try_get_completion(
				camera, &completion);

		if (status < 0) {
			fprintf(stderr, "Completion polling failed: %d\n", status);
			return status;
		}
		if (status == 0) {
			(*empty_polls)++;
			if (((*empty_polls) & 0xffff) == 0 &&
					monotonic_nsec() >= deadline) {
				fprintf(stderr, "Timed out after receiving %u of %u frames\n", completed, requested);
				return -1;
			}
			continue;
		}
		if (completion.result < 0 || completion.user_data == NULL ||
				aravis_camera_queue(camera, completion.buffer) < 0) {
			fprintf(stderr, "Invalid camera completion: %d\n", completion.result);
			return -1;
		}
		completed++;
	}
	return 0;
}

static enum aravis_transport parse_transport(const char *name)
{
	if (name == NULL)
		return ARAVIS_TRANSPORT_AUTO;
	if (strcmp(name, "gentl") == 0)
		return ARAVIS_TRANSPORT_GENTL;
	if (strcmp(name, "native-gv") == 0)
		return ARAVIS_TRANSPORT_NATIVE_GV;
	if (strcmp(name, "native-uv") == 0)
		return ARAVIS_TRANSPORT_NATIVE_UV;
	return ARAVIS_TRANSPORT_AUTO;
}

static const char *transport_name(enum aravis_transport transport)
{
	switch (transport) {
	case ARAVIS_TRANSPORT_GENTL:
		return "GenTL";
	case ARAVIS_TRANSPORT_NATIVE_GV:
		return "native GV";
	case ARAVIS_TRANSPORT_NATIVE_UV:
		return "native UV";
	case ARAVIS_TRANSPORT_AUTO:
		return "auto";
	}
	return "unknown";
}

static void check_camera_contract(struct aravis_camera *camera, uint32_t *feature_index)
{
	ArvBuffer *buffer = NULL;
	struct aravis_camera_completion completion;
	struct aravis_feature_info feature;
	struct aravis_feature_value value;
	struct aravis_buffer_progress progress;
	uint32_t index;

	spa_assert_se(aravis_camera_try_get_completion(camera, &completion) == -EINVAL);
	spa_assert_se(aravis_camera_get_buffer_progress(camera, NULL, &progress) == -EINVAL);
	spa_assert_se(aravis_camera_get_feature_info(camera, UINT32_MAX, &feature) == -EINVAL);
	spa_assert_se(aravis_camera_get_feature_value(camera, UINT32_MAX, &value) == -EINVAL);
	spa_assert_se(aravis_camera_find_feature(camera, "genicam.Width", &index) == 0);
	spa_assert_se(aravis_camera_get_feature_info(camera, index, &feature) == 0);
	spa_assert_se(feature.kind == ARAVIS_FEATURE_INTEGER);
	spa_assert_se(aravis_camera_get_feature_value(camera, index, &value) == 0);
	spa_assert_se(aravis_camera_get_feature_enum_entry(camera, index, 0) == NULL);
	int64_t minimum, maximum;
	double lower, upper;
	spa_assert_se(aravis_camera_get_feature_integer_range(camera, index, &minimum, &maximum) == 0);
	spa_assert_se(minimum <= value.integer && value.integer <= maximum);
	spa_assert_se(aravis_camera_get_feature_integer_range(camera, index, NULL, &maximum) == -EINVAL);
	spa_assert_se(aravis_camera_get_feature_float_range(camera, index, &lower, &upper) == -EINVAL);
	spa_assert_se(aravis_camera_set_feature_value(camera, index, NULL) == -EINVAL);
	spa_assert_se(aravis_camera_queue(camera, NULL) == -EINVAL);
	spa_assert_se(aravis_camera_announce(camera, NULL, 0, NULL, &buffer) == -EINVAL);
	*feature_index = index;
}

int main(int argc, char **argv)
{
	struct aravis_camera_options options = {
		.device_id = argc > 1 ? argv[1] : NULL,
		.transport = parse_transport(argc > 2 ? argv[2] : NULL),
	};
	struct test_buffer buffers[N_BUFFERS] = { 0 };
	struct aravis_camera *camera = NULL;
	const struct aravis_camera_info *info;
	uint64_t empty_polls = 0;
	enum aravis_transport transport = ARAVIS_TRANSPORT_AUTO;
	int res = EXIT_FAILURE;

	int status = aravis_camera_open(&camera, &options);
	if (status < 0) {
		fprintf(stderr, "Could not open camera: %d\n", status);
		goto out;
	}
	info = aravis_camera_get_info(camera);
	if (info == NULL)
		goto out;
	transport = aravis_camera_get_transport(camera);
	uint32_t feature_index = 0;
	const bool loopback = argc == 4 && strcmp(argv[3], "loopback") == 0;
	if (loopback)
		check_camera_contract(camera, &feature_index);
	for (uint32_t i = 0; i < N_BUFFERS; i++) {
		buffers[i].memory = malloc(info->payload_size);
		if (buffers[i].memory == NULL ||
				aravis_camera_announce(camera,
						buffers[i].memory,
						info->payload_size, &buffers[i],
						&buffers[i].buffer) < 0 ||
				aravis_camera_queue(camera, buffers[i].buffer) <
						0)
			goto out;
	}
	if (aravis_camera_start(camera) < 0)
		goto out;
	spa_assert_se(aravis_camera_refresh_info(camera) == -EBUSY);
	if (loopback) {
		struct aravis_feature_value width = { .kind = ARAVIS_FEATURE_INTEGER };
		spa_assert_se(aravis_camera_set_feature_value(camera, feature_index, &width) == -EBUSY);
	}
	spa_assert_se(aravis_camera_revoke(camera, &buffers[0].buffer) == -EINVAL);
	spa_assert_se(aravis_camera_start(camera) == 0);
	if (capture_frames(camera, N_FRAMES, &empty_polls) < 0 ||
			aravis_camera_stop(camera) < 0)
		goto out;
	for (uint32_t i = 0; i < N_BUFFERS; i++)
		if (aravis_camera_queue(camera, buffers[i].buffer) < 0)
			goto out;
	if (aravis_camera_start(camera) < 0 ||
			capture_frames(camera, 1, &empty_polls) < 0)
		goto out;
	res = EXIT_SUCCESS;

out:
	if (camera != NULL)
		(void)aravis_camera_stop(camera);
	for (uint32_t i = 0; i < N_BUFFERS; i++) {
		if (camera != NULL && buffers[i].buffer != NULL)
			(void)aravis_camera_revoke(camera, &buffers[i].buffer);
		free(buffers[i].memory);
	}
	aravis_camera_close(camera);
	if (res == EXIT_SUCCESS)
		printf("Received %u %s frames after %" PRIu64 " empty polls\n",
				N_FRAMES + 1, transport_name(transport),
				empty_polls);
	return res;
}
