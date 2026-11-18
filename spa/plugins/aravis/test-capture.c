/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>
#include <spa/support/log.h>

#include "aravis.h"

#define REQUESTED_BUFFERS 8u
#define REQUESTED_FRAMES 16u

struct param_result {
	uint32_t expected;
	uint8_t *storage;
	size_t capacity;
	struct spa_pod *param;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	void *payload;
};

static void on_result(void *data, int seq SPA_UNUSED, int res, uint32_t type,
		const void *result)
{
	struct param_result *capture = data;
	const struct spa_result_node_params *params;
	uint32_t size;

	spa_assert_se(res >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	params = result;
	if (params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	if (size > capture->capacity) {
		capture->storage = realloc(capture->storage, size);
		spa_assert_se(capture->storage != NULL);
		capture->capacity = size;
	}
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static bool props_have_values(struct spa_pod *props)
{
	struct spa_pod_object *object = (struct spa_pod_object *)props;
	struct spa_pod_prop *property;

	SPA_POD_OBJECT_FOREACH(object, property)
	{
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;
		const char *name = NULL;
		struct spa_pod *value = NULL;

		if (property->key != SPA_PROP_params)
			continue;
		spa_pod_parser_pod(&parser, &property->value);
		if (spa_pod_parser_push_struct(&parser, &frame) < 0)
			return false;
		return spa_pod_parser_get_string(&parser, &name) == 0 &&
				spa_pod_parser_get_pod(&parser, &value) == 0 &&
				name != NULL && value != NULL;
	}
	return false;
}

static struct spa_pod *find_control_value(
		struct spa_pod *props, const char *requested)
{
	struct spa_pod_object *object = (struct spa_pod_object *)props;
	struct spa_pod_prop *property;

	SPA_POD_OBJECT_FOREACH(object, property)
	{
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;

		if (property->key != SPA_PROP_params)
			continue;
		spa_pod_parser_pod(&parser, &property->value);
		spa_assert_se(spa_pod_parser_push_struct(&parser, &frame) == 0);
		for (;;) {
			const char *name = NULL;
			struct spa_pod *value = NULL;

			if (spa_pod_parser_get_string(&parser, &name) < 0)
				break;
			spa_assert_se(spa_pod_parser_get_pod(&parser, &value) ==
					0);
			if (spa_streq(name, requested))
				return value;
		}
	}
	return NULL;
}

static struct spa_pod *build_control_write(uint8_t *storage, size_t size,
		const char *name, const struct spa_pod *value)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Props,
			SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_string(&builder, name);
	spa_pod_builder_primitive(&builder, value);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

static struct spa_pod *enum_one(struct spa_node *node,
		struct param_result *capture, uint32_t id)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT,
				      0, id, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static struct spa_pod *enum_node_one(struct spa_node *node,
		struct param_result *capture, uint32_t id)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_enum_params(node, 1, id, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static void init_buffer(struct test_buffer *storage, uint32_t size)
{
	storage->payload = calloc(1, size);
	spa_assert_se(storage->payload != NULL);
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.data = storage->payload;
	storage->data.maxsize = size;
	storage->data.fd = -1;
	storage->data.chunk = &storage->chunk;
	storage->metas[0] = (struct spa_meta){
		.type = SPA_META_Header,
		.size = sizeof(storage->header),
		.data = &storage->header,
	};
	storage->metas[1] = (struct spa_meta){
		.type = SPA_META_Acquisition,
		.size = sizeof(storage->acquisition),
		.data = &storage->acquisition,
	};
	storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static void inspect_port(struct spa_handle *handle, struct spa_node *node,
		struct param_result *params, struct spa_io_buffers *io)
{
	static const struct spa_node_callbacks callbacks = { SPA_VERSION_NODE_CALLBACKS };
	const struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	const struct spa_command flush = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Flush);
	struct spa_pod_int invalid = SPA_POD_INIT_Int(0);
	void *unused;
	uint32_t advertised;

	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Log, &unused) == -ENOENT);
	spa_assert_se(spa_node_set_callbacks(node, &callbacks, NULL) == 0);
	spa_assert_se(spa_node_add_port(node, SPA_DIRECTION_OUTPUT, 1, NULL) == -ENOTSUP);
	spa_assert_se(spa_node_remove_port(node, SPA_DIRECTION_OUTPUT, 0) == -ENOTSUP);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Buffers, io, sizeof(*io)) == -ENOTSUP);
	spa_assert_se(spa_node_port_reuse_buffer(node, 0, 0) == -ENOTSUP);
	spa_assert_se(spa_node_send_command(node, &start) == -EIO);
	spa_assert_se(spa_node_send_command(node, &flush) == -ENOTSUP);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, &invalid.pod) == -EINVAL);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, NULL) == 0);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Buffers, 0, NULL) == -ENOENT);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Props, 0, NULL) == -ENOENT);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, &invalid.pod) == -EINVAL);
	spa_assert_se(spa_pod_parse_object(enum_one(node, params, SPA_PARAM_IO),
			SPA_TYPE_OBJECT_ParamIO, NULL, SPA_PARAM_IO_id, SPA_POD_Id(&advertised)) >= 0);
	spa_assert_se(advertised == SPA_IO_Buffers);
	params->expected = SPA_PARAM_Meta;
	for (uint32_t i = 0; i < 2; i++) {
		params->param = NULL;
		spa_assert_se(spa_node_port_enum_params(node, 0, SPA_DIRECTION_OUTPUT, 0,
				SPA_PARAM_Meta, i, 1, NULL) == 0);
		spa_assert_se(params->param != NULL);
		spa_assert_se(spa_pod_parse_object(params->param, SPA_TYPE_OBJECT_ParamMeta,
				NULL, SPA_PARAM_META_type, SPA_POD_Id(&advertised)) >= 0);
		spa_assert_se(advertised == (i == 0 ? SPA_META_Header : SPA_META_Acquisition));
	}
	params->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 0, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Meta, 2, 1, NULL) == 0);
	spa_assert_se(params->param == NULL);
	spa_assert_se(spa_node_port_enum_params(node, 0, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Props, 0, 1, NULL) == -ENOENT);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Clock, io, sizeof(*io)) == -ENOENT);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, io, sizeof(*io) - 1) == -EINVAL);
}

static int write_feature(struct spa_node *node, const char *name, const struct spa_pod *value)
{
	uint8_t storage[256];
	struct spa_pod *write = build_control_write(storage, sizeof(storage), name, value);

	spa_assert_se(write != NULL);
	return spa_node_set_param(node, SPA_PARAM_Props, 0, write);
}

static void check_scalar_writes(struct spa_node *node, struct param_result *params)
{
	const struct spa_pod_bool on = SPA_POD_INIT_Bool(true);
	const struct spa_pod_long one = SPA_POD_INIT_Long(1);
	uint8_t storage[32];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	spa_pod_builder_string(&builder, "Configured");
	const struct spa_pod *label = spa_pod_builder_deref(&builder, 0);
	bool enabled = false;
	const char *saved;
	uint32_t enum_index = 0;
	int32_t hidden = -1;

	for (uint32_t i = 0; i < 1024; i++) {
		const char *name;
		params->expected = SPA_PARAM_PropInfo;
		params->param = NULL;
		spa_assert_se(spa_node_enum_params(node, 0, SPA_PARAM_PropInfo, i, 1, NULL) == 0);
		spa_assert_se(params->param != NULL);
		spa_assert_se(spa_pod_parse_object(params->param, SPA_TYPE_OBJECT_PropInfo, NULL,
				SPA_PROP_INFO_name, SPA_POD_String(&name)) >= 0);
		if (spa_streq(name, "genicam.TestMode")) {
			enum_index = i;
			break;
		}
	}
	spa_assert_se(enum_index > 0);
	const struct spa_pod_prop *labels = spa_pod_find_prop(params->param, NULL, SPA_PROP_INFO_labels);
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	int32_t index;
	const char *entry;

	spa_assert_se(labels != NULL);
	spa_pod_parser_pod(&parser, &labels->value);
	spa_assert_se(spa_pod_parser_push_struct(&parser, &frame) == 0);
	while (spa_pod_parser_get_int(&parser, &index) >= 0) {
		spa_assert_se(spa_pod_parser_get_string(&parser, &entry) >= 0);
		if (spa_streq(entry, "Hidden"))
			hidden = index;
	}
	spa_assert_se(hidden >= 0);
	const struct spa_pod_int hidden_value = SPA_POD_INIT_Int(hidden);
	spa_assert_se(write_feature(node, "genicam.TestMode", &hidden_value.pod) == -ENODATA);

	spa_assert_se(write_feature(node, "genicam.TestBoolean", &on.pod) == 0);
	spa_assert_se(write_feature(node, "genicam.TestLabel", label) == 0);
	spa_assert_se(write_feature(node, "genicam.TestUnavailableLabel", label) == -ENODATA);
	spa_assert_se(write_feature(node, "genicam.TestUnavailableInteger", &one.pod) == -ENODATA);
	spa_assert_se(write_feature(node, "genicam.TestBoolean", &one.pod) == -EINVAL);
	spa_assert_se(write_feature(node, "genicam.TestLabel", &one.pod) == -EINVAL);
	struct spa_pod *props = enum_node_one(node, params, SPA_PARAM_Props);
	spa_assert_se(spa_pod_get_bool(find_control_value(props, "genicam.TestBoolean"), &enabled) == 0 &&
			enabled);
	spa_assert_se(spa_pod_get_string(find_control_value(props, "genicam.TestLabel"), &saved) == 0 &&
			spa_streq(saved, "Configured"));
	spa_assert_se(find_control_value(props, "genicam.TestUnavailableLabel") == NULL);
}

static void inspect_features(struct spa_node *node, struct param_result *params, bool row_blocks)
{
	uint32_t count = 0;
	bool saw_width = false, saw_exposure = false;
	int32_t mono16 = -1, bayer = -1;
	params->expected = SPA_PARAM_PropInfo;
	for (uint32_t i = 0; i < 1024; i++) {
		const char *name;
		params->param = NULL;
		spa_assert_se(spa_node_enum_params(node, 1, SPA_PARAM_PropInfo, i, 1, NULL) == 0);
		if (params->param == NULL)
			break;
		spa_assert_se(spa_pod_parse_object(params->param, SPA_TYPE_OBJECT_PropInfo,
				NULL, SPA_PROP_INFO_name, SPA_POD_String(&name)) >= 0);
		spa_assert_se(strncmp(name, "genicam.", 8) == 0);
		const struct spa_pod_prop *type = spa_pod_find_prop(params->param, NULL, SPA_PROP_INFO_type);
		spa_assert_se(type != NULL);
		if (spa_streq(name, "genicam.Width") || spa_streq(name, "genicam.ExposureTimeAbs")) {
			bool width = spa_streq(name, "genicam.Width");
			spa_assert_se(spa_pod_is_choice(&type->value));
			const struct spa_pod_choice *range = (const struct spa_pod_choice *)&type->value;
			spa_assert_se(SPA_POD_CHOICE_TYPE(range) == SPA_CHOICE_Range);
			spa_assert_se(SPA_POD_CHOICE_N_VALUES(range) == 3);
			spa_assert_se(SPA_POD_CHOICE_CHILD(range)->type == (width ? SPA_TYPE_Long : SPA_TYPE_Double));
			saw_width |= width;
			saw_exposure |= !width;
		}
		if (spa_streq(name, "genicam.PixelFormat")) {
			const struct spa_pod_prop *labels = spa_pod_find_prop(params->param, NULL, SPA_PROP_INFO_labels);
			struct spa_pod_parser parser;
			struct spa_pod_frame frame;
			int32_t index;
			const char *label;

			spa_assert_se(labels != NULL);
			spa_pod_parser_pod(&parser, &labels->value);
			spa_assert_se(spa_pod_parser_push_struct(&parser, &frame) == 0);
			while (spa_pod_parser_get_int(&parser, &index) >= 0) {
				spa_assert_se(spa_pod_parser_get_string(&parser, &label) == 0);
				if (spa_streq(label, "Mono16"))
					mono16 = index;
				if (spa_streq(label, "BayerBG8"))
					bayer = index;
			}
		}
		count++;
	}
	spa_assert_se(saw_width && saw_exposure && mono16 >= 0 && bayer >= 0 && count < 1024);
	struct spa_pod *props = enum_node_one(node, params, SPA_PARAM_Props);
	int32_t pixel_format;
	int64_t height;
	spa_assert_se(spa_pod_get_int(find_control_value(props, "genicam.PixelFormat"), &pixel_format) == 0);
	spa_assert_se(spa_pod_get_long(find_control_value(props, "genicam.Height"), &height) == 0);
	const struct spa_pod_long height_change = SPA_POD_INIT_Long(height - 1);
	const struct spa_pod_int unsupported = SPA_POD_INIT_Int(bayer);
	const struct spa_pod_int invalid = SPA_POD_INIT_Int(-1);
	const struct spa_pod_id gray16 = SPA_POD_INIT_Id((uint32_t)mono16);
	const struct spa_pod_int original = SPA_POD_INIT_Int(pixel_format);

	spa_assert_se(write_feature(node, "genicam.DoesNotExist", &invalid.pod) == -ENOENT);
	spa_assert_se(write_feature(node, "genicam.DeviceVendorName", &invalid.pod) == -EACCES);
	spa_assert_se(write_feature(node, "genicam.Width", &invalid.pod) == -EINVAL);
	spa_assert_se(write_feature(node, "genicam.PixelFormat", &unsupported.pod) == -ENOTSUP);
	spa_assert_se(write_feature(node, "genicam.PixelFormat", &invalid.pod) == -EINVAL);
	spa_assert_se(write_feature(node, "genicam.PixelFormat", &height_change.pod) == -EINVAL);
	spa_assert_se(write_feature(node, "genicam.PixelFormat", &gray16.pod) == 0);
	if (row_blocks) {
		struct spa_ndarray_info format = SPA_NDARRAY_INFO_INIT();
		spa_assert_se(spa_format_ndarray_parse(enum_one(node, params, SPA_PARAM_EnumFormat), &format) >= 0);
		spa_assert_se(format.element_type == SPA_ELEMENT_TYPE_U16_LE);
	} else {
		struct spa_video_info_raw format = { 0 };
		spa_assert_se(spa_format_video_raw_parse(enum_one(node, params, SPA_PARAM_EnumFormat), &format) >= 0);
		spa_assert_se(format.format == SPA_VIDEO_FORMAT_GRAY16_LE);
	}
	spa_assert_se(write_feature(node, "genicam.PixelFormat", &original.pod) == 0);
	if (row_blocks) {
		spa_assert_se(write_feature(node, "genicam.Height", &height_change.pod) == -ENOTSUP);
		props = enum_node_one(node, params, SPA_PARAM_Props);
		int64_t restored;
		spa_assert_se(spa_pod_get_long(find_control_value(props, "genicam.Height"), &restored) == 0);
		spa_assert_se(restored == height);
	}
}

static void check_invalid_configurations(const struct spa_handle_factory *factory, const char *device)
{
	static const struct { const char *key, *value; int error; } cases[] = {
		{ SPA_KEY_API_ARAVIS_TRANSPORT, "invalid", -EINVAL },
		{ SPA_KEY_API_ARAVIS_OUTPUT_MODE, "invalid", -EINVAL },
		{ SPA_KEY_API_ARAVIS_USB_TRANSFER_SIZE, "64", -ERANGE },
		{ SPA_KEY_API_ARAVIS_USB_TRANSFER_SIZE, "4096", -EINVAL },
		{ SPA_KEY_API_ARAVIS_ROW_BLOCK_ROWS, "-1", -EINVAL },
		{ SPA_KEY_API_ARAVIS_ROW_BLOCK_ROWS, "1x", -EINVAL },
		{ SPA_KEY_API_ARAVIS_OUTPUT_MODE, "row-block", -EINVAL },
		{ SPA_KEY_API_ARAVIS_TRANSPORT, "native-uv", -ENODEV },
		{ SPA_KEY_API_ARAVIS_DEVICE, "unavailable-device", -ENODEV },
	};
	struct spa_dict_item items[] = { SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_DEVICE, device), { 0 } };
	const struct spa_dict props = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	struct spa_handle *handle = calloc(1, factory->get_size(factory, &props));

	spa_assert_se(handle != NULL);
	for (size_t i = 0; i < SPA_N_ELEMENTS(cases); i++) {
		bool missing_device = spa_streq(cases[i].key, SPA_KEY_API_ARAVIS_DEVICE);
		items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_DEVICE, missing_device ? cases[i].value : device);
		items[1] = missing_device ? SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_TRANSPORT, "native-gv") :
				SPA_DICT_ITEM_INIT(cases[i].key, cases[i].value);
		int res = factory->init(factory, handle, &props, NULL, 0);
		if (res != cases[i].error)
			fprintf(stderr, "Initialization with %s=%s returned %d\n",
					cases[i].key, cases[i].value, res);
		spa_assert_se(res == cases[i].error);
	}
	free(handle);
}

static void inject_fault(const char *fault)
{
	if (fault == NULL)
		spa_assert_se(unsetenv("PIPEWIREAO_TEST_ARAVIS_FAULT") == 0);
	else
		spa_assert_se(setenv("PIPEWIREAO_TEST_ARAVIS_FAULT", fault, 1) == 0);
}

static void check_camera_failures(const struct spa_handle_factory *factory,
		struct spa_handle *handle, const struct spa_dict *info, bool row_blocks)
{
	static const struct { const char *fault; int status; } cases[] = {
		{ "stream", -EIO }, { "payload", -EIO }, { "region", -EIO },
		{ "model", -EIO }, { "serial", -EIO }, { "pixel-format", -ENOTSUP },
	};

	if (row_blocks) {
		inject_fault("rate");
		spa_assert_se(factory->init(factory, handle, info, NULL, 0) == -ENOTSUP);
		inject_fault("large-rate");
		spa_assert_se(factory->init(factory, handle, info, NULL, 0) == -ENOTSUP);
	} else {
		for (size_t i = 0; i < SPA_N_ELEMENTS(cases); i++) {
			inject_fault(cases[i].fault);
			spa_assert_se(factory->init(factory, handle, info, NULL, 0) == cases[i].status);
		}
	}
	inject_fault(NULL);
}

static void check_control_validation(struct spa_node *node, struct param_result *params)
{
	uint8_t storage[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	struct spa_pod_frame object, values;
	const struct spa_pod *pod;

	spa_assert_se(spa_node_enum_params(node, 0, SPA_PARAM_IO, 0, 1, NULL) == -ENOENT);
	params->expected = SPA_PARAM_Props;
	params->param = NULL;
	spa_assert_se(spa_node_enum_params(node, 0, SPA_PARAM_Props, 1, 1, NULL) == 0);
	spa_assert_se(params->param == NULL);
	pod = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_params, SPA_POD_String("genicam.Width"));
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, pod) == -EINVAL);
	spa_pod_builder_init(&builder, storage, sizeof(storage));
	spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	pod = spa_pod_builder_pop(&builder, &object);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, pod) == 0);
	spa_pod_builder_init(&builder, storage, sizeof(storage));
	spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_string(&builder, "genicam.Width");
	spa_pod_builder_pop(&builder, &values);
	pod = spa_pod_builder_pop(&builder, &object);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, pod) == -EINVAL);

	spa_pod_builder_init(&builder, storage, sizeof(storage));
	pod = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_name, SPA_POD_String("missing"));
	params->expected = SPA_PARAM_PropInfo;
	spa_assert_se(spa_node_enum_params(node, 0, SPA_PARAM_PropInfo, 0, 1, pod) == 0);
	spa_assert_se(params->param == NULL);
}

static void check_rejected_frames(struct spa_node *node, struct spa_io_buffers *io)
{
	uint64_t deadline;
	int status;

	/* Run through a full simulator period while each completion is incomplete. */
	inject_fault("incomplete");
	deadline = monotonic_nsec() + SPA_NSEC_PER_SEC;
	do {
		spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
		spa_assert_se(io->status == SPA_STATUS_NEED_DATA);
		usleep(1000);
	} while (monotonic_nsec() < deadline);

	inject_fault("invalid-image");
	deadline = monotonic_nsec() + 2 * SPA_NSEC_PER_SEC;
	do {
		status = spa_node_process(node);
		spa_assert_se(monotonic_nsec() < deadline);
		usleep(1000);
	} while (status == SPA_STATUS_OK);
	spa_assert_se(status == -EPROTO && io->status == SPA_STATUS_NEED_DATA);
	inject_fault(NULL);
}

static int capture(const struct spa_handle_factory *factory,
		const char *device_id, bool row_blocks, bool native_uv, bool loopback, bool faults)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_DEVICE, device_id),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_TRANSPORT,
				native_uv ? "native-uv"
					  : row_blocks ? "native-gv" : "auto"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_OUTPUT_MODE,
				row_blocks ? "row-block" : "frame"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_ROW_BLOCK_ROWS, "128"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ARAVIS_DETECTOR_PROFILE,
				"test-profile"),
	};
	const struct spa_dict info =
			SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	struct test_buffer storage[REQUESTED_BUFFERS] = { 0 };
	struct spa_buffer *buffers[REQUESTED_BUFFERS];
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct param_result params = { .expected = SPA_ID_INVALID };
	struct spa_hook listener;
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct spa_video_info_raw video = { 0 };
	struct spa_ndarray_info ndarray = SPA_NDARRAY_INFO_INIT();
	struct spa_command start =
			SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause =
			SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_pod *format, *buffers_param, *prop_info, *props;
	struct spa_pod *scalar_value = NULL, *scalar_write, *width_value,
		       *width_write;
	uint8_t scalar_write_storage[512], width_write_storage[512];
	const char *scalar_name = NULL;
	const char *property_name = NULL;
	uint64_t deadline;
	uint64_t row_sequence = 0;
	int64_t last_pts = SPA_TIME_INVALID;
	int32_t payload_size = 0;
	uint32_t frames = 0, i, expected_row = 0, row_markers = 0;
	int init_result;

	handle = calloc(1, factory->get_size(factory, &info));
	spa_assert_se(handle != NULL);
	if (faults)
		check_camera_failures(factory, handle, &info, row_blocks);
	init_result = factory->init(factory, handle, &info, NULL, 0);
	if (init_result < 0) {
		fprintf(stderr, "Aravis source init failed: %s (%d)\n",
				strerror(-init_result), init_result);
		free(handle);
		return 77;
	}
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
				      (void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
				      &params) == 0);
	inspect_port(handle, node, &params, &io);
	check_control_validation(node, &params);
	if (loopback) {
		inspect_features(node, &params, row_blocks);
		check_scalar_writes(node, &params);
	}
	prop_info = enum_node_one(node, &params, SPA_PARAM_PropInfo);
	spa_assert_se(spa_pod_parse_object(prop_info, SPA_TYPE_OBJECT_PropInfo,
				      NULL, SPA_PROP_INFO_name,
				      SPA_POD_String(&property_name)) >= 0);
	spa_assert_se(property_name != NULL &&
			strncmp(property_name, "genicam.", 8) == 0);
	props = enum_node_one(node, &params, SPA_PARAM_Props);
	spa_assert_se(props_have_values(props));
	for (i = 0; i < 5; i++) {
		static const char *const candidates[] = {
			"genicam.ExposureTime",
			"genicam.Gain",
			"genicam.AcquisitionFrameRate",
			"genicam.ExposureTimeAbs",
			"genicam.GainRaw",
		};

		if ((scalar_value = find_control_value(props, candidates[i])) !=
				NULL) {
			scalar_name = candidates[i];
			break;
		}
	}
	spa_assert_se(scalar_name != NULL && scalar_value != NULL);
	scalar_write = build_control_write(scalar_write_storage,
			sizeof(scalar_write_storage), scalar_name,
			scalar_value);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
				      scalar_write) == 0);
	format = enum_one(node, &params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
				      SPA_PARAM_Format, 0, format) == 0);
	if (row_blocks) {
		spa_assert_se(spa_format_ndarray_parse(format, &ndarray) >= 0);
		spa_assert_se(ndarray.n_dimensions == 2 &&
				ndarray.shape[0] == 128 &&
				ndarray.shape[1] > 0);
	} else {
		spa_assert_se(spa_format_video_raw_parse(format, &video) >= 0);
		spa_assert_se(video.size.width > 0 && video.size.height > 0);
	}
	buffers_param = enum_one(node, &params, SPA_PARAM_Buffers);
	spa_assert_se(spa_pod_parse_object(buffers_param,
				      SPA_TYPE_OBJECT_ParamBuffers, NULL,
				      SPA_PARAM_BUFFERS_size,
				      SPA_POD_Int(&payload_size)) >= 0);
	spa_assert_se(payload_size > 0);
	for (i = 0; i < REQUESTED_BUFFERS; i++) {
		init_buffer(&storage[i], (uint32_t)payload_size);
		buffers[i] = &storage[i].buffer;
	}
	spa_assert_se(enum_one(node, &params, SPA_PARAM_Format) != NULL);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, 1) == -EINVAL);
	storage[1].data.maxsize = 0;
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, REQUESTED_BUFFERS) == -EINVAL);
	storage[1].data.maxsize = (uint32_t)payload_size;
	uint32_t meta_count = storage[1].buffer.n_metas;
	storage[1].buffer.n_metas = 1;
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, REQUESTED_BUFFERS) == -EINVAL);
	storage[1].buffer.n_metas = meta_count;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
				      SPA_IO_Buffers, &io, sizeof(io)) == 0);
	if (faults) {
		inject_fault("announce");
		spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
				0, buffers, REQUESTED_BUFFERS) == -EIO);
		inject_fault(NULL);
	}
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
				      0, buffers, REQUESTED_BUFFERS) == 0);
	props = enum_node_one(node, &params, SPA_PARAM_Props);
	width_value = find_control_value(props, "genicam.Width");
	spa_assert_se(width_value != NULL);
	width_write = build_control_write(width_write_storage,
			sizeof(width_write_storage), "genicam.Width",
			width_value);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
				      width_write) == -EBUSY);
	if (faults) {
		inject_fault("queue");
		spa_assert_se(spa_node_send_command(node, &start) == -EIO);
		inject_fault("start");
		spa_assert_se(spa_node_send_command(node, &start) == -EIO);
		inject_fault(NULL);
	}
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
				      scalar_write) == -EBUSY);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, NULL) == -EBUSY);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0, NULL, 0) == -EBUSY);
	io.buffer_id = REQUESTED_BUFFERS;
	spa_assert_se(spa_node_process(node) == -EPROTO);
	/* An in-range buffer ID is still invalid until the source publishes it. */
	io.buffer_id = 0;
	spa_assert_se(spa_node_process(node) == -EPROTO);
	io.buffer_id = SPA_ID_INVALID;
	deadline = monotonic_nsec() + 5 * SPA_NSEC_PER_SEC;
	while (frames < REQUESTED_FRAMES) {
		uint32_t id;
		int res;

		res = spa_node_process(node);
		spa_assert_se(res >= SPA_STATUS_OK);
		if (io.status != SPA_STATUS_HAVE_DATA) {
			spa_assert_se(monotonic_nsec() < deadline);
			continue;
		}
		id = io.buffer_id;
		spa_assert_se(id < REQUESTED_BUFFERS);
		spa_assert_se(storage[id].chunk.size > 0 &&
				storage[id].chunk.size <=
						(uint32_t)payload_size);
		spa_assert_se(storage[id].header.seq > 0);
		if (row_blocks) {
			if (storage[id].header.seq != row_sequence) {
				spa_assert_se(row_sequence == 0 ||
						expected_row == 0);
				spa_assert_se(storage[id].header.offset == 0);
				row_sequence = storage[id].header.seq;
			}
			spa_assert_se(storage[id].header.offset ==
					expected_row);
			expected_row += 128;
			if (SPA_FLAG_IS_SET(storage[id].header.flags,
					    SPA_META_HEADER_FLAG_MARKER)) {
				row_markers++;
				expected_row = 0;
			}
		}
		if (!row_blocks)
			spa_assert_se(storage[id].header.pts !=
					SPA_TIME_INVALID);
		if (storage[id].header.pts != SPA_TIME_INVALID &&
				last_pts != SPA_TIME_INVALID)
			spa_assert_se(storage[id].header.pts > last_pts);
		if (storage[id].header.pts != SPA_TIME_INVALID)
			last_pts = storage[id].header.pts;
		spa_assert_se(spa_meta_acquisition_is_valid(
				&storage[id].metas[1]));
		io.status = SPA_STATUS_NEED_DATA;
		frames++;
	}
	if (row_blocks)
		spa_assert_se(row_markers > 0);
	if (loopback) {
		io.buffer_id = SPA_ID_INVALID;
		for (i = 0; i < REQUESTED_BUFFERS; i++)
			storage[i].data.maxsize = 1;
		deadline = monotonic_nsec() + 2 * SPA_NSEC_PER_SEC;
		int status;
		do {
			status = spa_node_process(node);
			spa_assert_se(monotonic_nsec() < deadline);
		} while (status == SPA_STATUS_OK);
		spa_assert_se(status == -ENOSPC && io.status == SPA_STATUS_NEED_DATA);
	}
	if (faults && !row_blocks)
		check_rejected_frames(node, &io);
	if (faults && row_blocks) {
		inject_fault("stop");
		spa_assert_se(handle->clear(handle) == -EIO);
		inject_fault(NULL);
		spa_hook_remove(&listener);
		goto release;
	}
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
				      SPA_IO_Buffers, NULL, 0) == 0);
	if (faults)
		inject_fault("revoke");
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0,
				      0, NULL, 0) == (faults ? -EIO : 0));
	inject_fault(NULL);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
				      SPA_PARAM_Format, 0, NULL) == 0);
	props = enum_node_one(node, &params, SPA_PARAM_Props);
	width_value = find_control_value(props, "genicam.Width");
	spa_assert_se(width_value != NULL);
	width_write = build_control_write(width_write_storage,
			sizeof(width_write_storage), "genicam.Width",
			width_value);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
				      width_write) == 0);
	format = enum_one(node, &params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
				      SPA_PARAM_Format, 0, format) == 0);
	if (faults) {
		inject_fault("tiny-rate");
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, width_write) == 0);
		inject_fault("range");
		params.expected = SPA_PARAM_PropInfo;
		bool width_seen = false;
		for (uint32_t i = 0; i < 1024; i++) {
			const char *name;
			params.param = NULL;
			spa_assert_se(spa_node_enum_params(node, 0, SPA_PARAM_PropInfo, i, 1, NULL) == 0);
			if (params.param == NULL)
				break;
			spa_assert_se(spa_pod_parse_object(params.param, SPA_TYPE_OBJECT_PropInfo, NULL,
					SPA_PROP_INFO_name, SPA_POD_String(&name)) >= 0);
			if (spa_streq(name, "genicam.Width")) {
				const struct spa_pod_prop *type = spa_pod_find_prop(params.param, NULL, SPA_PROP_INFO_type);
				spa_assert_se(type != NULL && spa_pod_is_long(&type->value));
				width_seen = true;
				break;
			}
		}
		spa_assert_se(width_seen);
		inject_fault("refresh");
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, width_write) == -EIO);
		inject_fault(NULL);
		format = enum_one(node, &params, SPA_PARAM_IO);
		spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
				SPA_PARAM_Format, 0, format) == -EIO);
	}
	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
release:
	free(handle);
	free(params.storage);
	for (i = 0; i < REQUESTED_BUFFERS; i++)
		free(storage[i].payload);
	printf("captured %u Aravis %s\n", frames,
			row_blocks ? "row blocks" : "frames");
	return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	void *library;
	int res;

	spa_assert_se(argc >= 3 && argc <= 5);
	const bool loopback = argc == 5 && spa_streq(argv[4], "loopback");
	const bool faults = argc == 5 && spa_streq(argv[4], "faults");
	spa_assert_se(argc < 5 || loopback || faults);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(
			library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_ARAVIS_SOURCE));
	const struct spa_interface_info *interface;
	uint32_t interface_index = 0;
	spa_assert_se(factory->enum_interface_info(factory, &interface, &interface_index) == 1);
	spa_assert_se(spa_streq(interface->type, SPA_TYPE_INTERFACE_Node));
	spa_assert_se(factory->enum_interface_info(factory, &interface, &interface_index) == 0);
	if (loopback && spa_streq(argv[3], "frame"))
		check_invalid_configurations(factory, argv[2]);
	res = capture(factory, argv[2],
			argc >= 4 &&
					(spa_streq(argv[3], "row-block") ||
					 spa_streq(argv[3], "row-block-uv")),
			argc >= 4 && spa_streq(argv[3], "row-block-uv"), loopback, faults);
	spa_assert_se(dlclose(library) == 0);
	return res;
}
