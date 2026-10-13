#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-frame.h>

#include "obs-frame-tap.h"
#include "replay.h"
#include "replay-control.h"

extern bool obs_module_load(void);
extern void obs_module_post_load(void);
extern void obs_module_unload(void);

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			return 1;                                              \
		}                                                              \
	} while (0)

struct fake_tap_cam_data {
	obs_source_t *source;
	pthread_mutex_t mutex;
	frame_tap_cb cb;
	void *cb_param;
	int add_calls;
	int remove_calls;
};

static void fake_cam_add_tap(void *data, calldata_t *cd)
{
	struct fake_tap_cam_data *cam = data;
	frame_tap_cb cb = (frame_tap_cb)(uintptr_t)calldata_ptr(cd, "callback");
	void *param = calldata_ptr(cd, "param");

	pthread_mutex_lock(&cam->mutex);
	cam->cb = cb;
	cam->cb_param = param;
	cam->add_calls++;
	pthread_mutex_unlock(&cam->mutex);
}

static void fake_cam_remove_tap(void *data, calldata_t *cd)
{
	struct fake_tap_cam_data *cam = data;
	frame_tap_cb cb = (frame_tap_cb)(uintptr_t)calldata_ptr(cd, "callback");
	void *param = calldata_ptr(cd, "param");

	pthread_mutex_lock(&cam->mutex);
	if (cam->cb == cb && cam->cb_param == param) {
		cam->cb = NULL;
		cam->cb_param = NULL;
	}
	cam->remove_calls++;
	pthread_mutex_unlock(&cam->mutex);
}

struct find_filter_ctx {
	obs_source_t *filter_src;
	struct replay_filter *rf;
};

static void find_replay_filter_cb(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	struct find_filter_ctx *ctx = param;
	const char *id = obs_source_get_unversioned_id(filter);
	if (id && strcmp(id, REPLAY_FILTER_ASYNC_ID) == 0) {
		ctx->filter_src = filter;
		ctx->rf = obs_obj_get_data(filter);
	}
}

static const char *fake_cam_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Fake NDI Cam Source";
}

static void *fake_cam_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct fake_tap_cam_data *cam = bzalloc(sizeof(struct fake_tap_cam_data));
	cam->source = source;
	pthread_mutex_init(&cam->mutex, NULL);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	if (ph) {
		proc_handler_add(ph, FRAME_TAP_PROC_ADD_DECL, fake_cam_add_tap, cam);
		proc_handler_add(ph, FRAME_TAP_PROC_REMOVE_DECL, fake_cam_remove_tap, cam);
	}

	return cam;
}

static void fake_cam_destroy(void *data)
{
	struct fake_tap_cam_data *cam = data;
	pthread_mutex_destroy(&cam->mutex);
	bfree(cam);
}

static struct obs_source_info fake_cam_info = {
	.id = "fake_tap_cam",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = fake_cam_get_name,
	.create = fake_cam_create,
	.destroy = fake_cam_destroy,
};

int main(int argc, char **argv)
{
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("[test-tap-hidden] No DISPLAY set, re-executing under xvfb-run -a...\n");
		char **new_argv = calloc((size_t)argc + 3, sizeof(char *));
		new_argv[0] = "xvfb-run";
		new_argv[1] = "-a";
		for (int i = 0; i < argc; i++)
			new_argv[i + 2] = argv[i];
		new_argv[argc + 2] = NULL;
		execvp("xvfb-run", new_argv);
		perror("execvp xvfb-run");
		return 1;
	}

	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);

	CHECK(obs_startup("en-US", NULL, NULL), "obs_startup failed");
	obs_add_data_path("/usr/share/obs/libobs/");

	struct obs_video_info ovi = {
		.graphics_module = "libobs-opengl.so.30",
		.fps_num = 60,
		.fps_den = 1,
		.base_width = 320,
		.base_height = 180,
		.output_width = 320,
		.output_height = 180,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = 0,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BILINEAR,
	};
	int vret = obs_reset_video(&ovi);
	if (vret != OBS_VIDEO_SUCCESS) {
		ovi.graphics_module = "libobs-opengl";
		vret = obs_reset_video(&ovi);
	}
	CHECK(vret == OBS_VIDEO_SUCCESS, "obs_reset_video failed: %d", vret);

	struct obs_audio_info oai = {
		.samples_per_sec = 48000,
		.speakers = SPEAKERS_STEREO,
	};
	CHECK(obs_reset_audio(&oai), "obs_reset_audio failed");

	obs_register_source(&fake_cam_info);

	CHECK(obs_module_load(), "obs_module_load failed");
	obs_module_post_load();

	/* Create the fake camera source (NOT added to any scene, completely hidden) */
	obs_source_t *cam = obs_source_create("fake_tap_cam", "Cam Hidden", NULL, NULL);
	CHECK(cam != NULL, "failed to create fake camera");
	struct fake_tap_cam_data *cam_data = obs_obj_get_data(cam);
	CHECK(cam_data != NULL, "failed to get cam_data");

	CHECK(!obs_source_showing(cam), "camera must not be showing in any scene");
	CHECK(!obs_source_active(cam), "camera must not be active");
	CHECK(cam_data->add_calls == 0, "tap should not be added before replay source connects");

	/* Create replay source and point it at the hidden camera */
	obs_source_t *rs = obs_source_create(REPLAY_SOURCE_ID, "Replay Hidden", NULL, NULL);
	CHECK(rs != NULL, "failed to create replay source");

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, SETTING_SOURCE, "Cam Hidden");
	obs_data_set_int(s, SETTING_DURATION, 5000);
	obs_data_set_int(s, SETTING_STORAGE_FORMAT, STORAGE_FORMAT_NV12);
	obs_source_update(rs, s);
	obs_data_release(s);

	/*
	 * Verify that video_tick runs for the private filter of the hidden/non-showing parent:
	 * Wait up to 500 ms (about 30 frames at 60 fps) for the graphics thread to tick the filter.
	 */
	int attached = 0;
	for (int i = 0; i < 30; i++) {
		pthread_mutex_lock(&cam_data->mutex);
		attached = (cam_data->add_calls > 0 && cam_data->cb != NULL);
		pthread_mutex_unlock(&cam_data->mutex);
		if (attached)
			break;
		os_sleep_ms(16);
	}

	CHECK(attached, "filter failed to register frame tap on hidden/non-showing camera");
	CHECK(!obs_source_showing(cam), "camera must remain non-showing during tap test");

	/*
	 * Invoke the tap callback with synthetic NV12 frames and assert the replay buffer fills.
	 */
	struct obs_source_frame frame;
	memset(&frame, 0, sizeof(frame));
	video_frame_init((struct video_frame *)&frame, VIDEO_FORMAT_NV12, 320, 180);
	frame.width = 320;
	frame.height = 180;
	frame.format = VIDEO_FORMAT_NV12;
	memset(frame.data[0], 0x50, (size_t)frame.linesize[0] * 180);
	memset(frame.data[1], 0x80, (size_t)frame.linesize[1] * 90);

	const uint64_t interval_ns = 1000000000ULL / 60ULL;
	const uint64_t base_ts = os_gettime_ns();
	const int num_frames = 10;
	for (int i = 0; i < num_frames; i++) {
		frame.timestamp = base_ts + (uint64_t)i * interval_ns;
		pthread_mutex_lock(&cam_data->mutex);
		frame_tap_cb cb = cam_data->cb;
		void *cb_param = cam_data->cb_param;
		if (cb) {
			cb(cb_param, cam, &frame);
		}
		pthread_mutex_unlock(&cam_data->mutex);
	}
	video_frame_free((struct video_frame *)&frame);

	/* Check buffer status via replay_source_get_status */
	struct replay_source *rs_ctx = obs_obj_get_data(rs);
	CHECK(rs_ctx != NULL, "failed to get replay_source context");

	struct replay_source_status status = {0};
	CHECK(replay_source_get_status(rs_ctx, &status), "replay_source_get_status failed");
	CHECK(status.buffered_ms > 0, "replay buffer did not fill: buffered_ms=%" PRId64, status.buffered_ms);

	/* Verify filter buffer directly */
	struct find_filter_ctx fctx = {0};
	obs_source_enum_filters(cam, find_replay_filter_cb, &fctx);
	CHECK(fctx.rf != NULL, "replay_filter not found on camera source");
	size_t filter_buffered = fctx.rf->video_frames.size / sizeof(struct obs_source_frame *);
	CHECK(filter_buffered >= (size_t)num_frames,
	      "filter video_frames buffer size mismatch: expected at least %d, got %zu frames",
	      num_frames, filter_buffered);

	/* Clean up: removing the filter from the camera source must detach the tap */
	obs_source_filter_remove(cam, fctx.filter_src);
	int detached = 0;
	for (int i = 0; i < 30; i++) {
		pthread_mutex_lock(&cam_data->mutex);
		detached = (cam_data->remove_calls > 0 && cam_data->cb == NULL);
		pthread_mutex_unlock(&cam_data->mutex);
		if (detached)
			break;
		os_sleep_ms(16);
	}
	CHECK(detached, "frame tap was not removed after filter was removed");

	obs_source_release(rs);
	obs_source_release(cam);

	obs_module_unload();
	obs_shutdown();

	printf("test-tap-hidden: ALL ASSERTIONS PASSED (tap attached and buffered on hidden parent)\n");
	return 0;
}
