#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <media-io/video-frame.h>

#include "replay.h"
#include "replay-blend.h"
#include "replay-control.h"

extern bool obs_module_load(void);
extern void obs_module_unload(void);
extern struct obs_source_info replay_filter_async_info;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			return 1;                                              \
		}                                                              \
	} while (0)

/* =========================================================================
 * 1. Unit SIMD == Scalar bit-exactness and hand-computed checks
 * ========================================================================= */

static int test_simd_vs_scalar(void)
{
	printf("[test-blend] Running SIMD vs scalar unit tests...\n");

	enum video_format formats[] = {VIDEO_FORMAT_NV12, VIDEO_FORMAT_UYVY, VIDEO_FORMAT_I420};
	struct {
		uint32_t width;
		uint32_t height;
	} dims[] = {
		{1920, 1080},
		{1366, 768},  /* odd width */
		{854, 480},   /* odd width */
		{17, 19},     /* prime odd dimensions */
	};

	uint32_t weights[] = {0, 1, 32, 64, 128, 192, 255, 256};

	for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		enum video_format fmt = formats[f];
		for (size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); d++) {
			uint32_t w = dims[d].width;
			uint32_t h = dims[d].height;

			struct obs_source_frame *fa = obs_source_frame_create(fmt, w, h);
			struct obs_source_frame *fb = obs_source_frame_create(fmt, w, h);
			struct obs_source_frame *fd_simd = obs_source_frame_create(fmt, w, h);
			struct obs_source_frame *fd_scalar = obs_source_frame_create(fmt, w, h);

			CHECK(fa && fb && fd_simd && fd_scalar, "frame allocation failed");

			/* Populate pseudo-random but deterministic data */
			for (size_t p = 0; p < MAX_AV_PLANES && fa->data[p]; p++) {
				size_t plane_h = h;
				if (fmt == VIDEO_FORMAT_NV12 && p == 1)
					plane_h = (h + 1) / 2;
				else if (fmt == VIDEO_FORMAT_I420 && p > 0)
					plane_h = (h + 1) / 2;
				size_t plane_bytes = (size_t)fa->linesize[p] * plane_h;
				for (size_t i = 0; i < plane_bytes; i++) {
					fa->data[p][i] = (uint8_t)((i * 17 + p * 31 + 7) & 0xFF);
					fb->data[p][i] = (uint8_t)((i * 29 + p * 53 + 13) & 0xFF);
				}
			}

			for (size_t wi = 0; wi < sizeof(weights) / sizeof(weights[0]); wi++) {
				uint32_t w8 = weights[wi];

				/* Blend via SSE2 helper */
				bool ok_simd = replay_blend_frames(fd_simd, fa, fb, w8);
				CHECK(ok_simd, "replay_blend_frames (SIMD) returned false for fmt %d (%ux%u)", fmt, w, h);

				/* Blend via scalar fallback manually on planes */
				if (fmt == VIDEO_FORMAT_NV12) {
					replay_blend_plane_scalar(fd_scalar->data[0], fa->data[0], fb->data[0],
								  w, h, fd_scalar->linesize[0], fa->linesize[0],
								  fb->linesize[0], w8);
					replay_blend_plane_scalar(fd_scalar->data[1], fa->data[1], fb->data[1],
								  w, (h + 1) / 2, fd_scalar->linesize[1], fa->linesize[1],
								  fb->linesize[1], w8);
				} else if (fmt == VIDEO_FORMAT_UYVY) {
					replay_blend_plane_scalar(fd_scalar->data[0], fa->data[0], fb->data[0],
								  (size_t)w * 2, h, fd_scalar->linesize[0], fa->linesize[0],
								  fb->linesize[0], w8);
				} else if (fmt == VIDEO_FORMAT_I420) {
					replay_blend_plane_scalar(fd_scalar->data[0], fa->data[0], fb->data[0],
								  w, h, fd_scalar->linesize[0], fa->linesize[0],
								  fb->linesize[0], w8);
					replay_blend_plane_scalar(fd_scalar->data[1], fa->data[1], fb->data[1],
								  (w + 1) / 2, (h + 1) / 2, fd_scalar->linesize[1],
								  fa->linesize[1], fb->linesize[1], w8);
					replay_blend_plane_scalar(fd_scalar->data[2], fa->data[2], fb->data[2],
								  (w + 1) / 2, (h + 1) / 2, fd_scalar->linesize[2],
								  fa->linesize[2], fb->linesize[2], w8);
				}

				/* Verify byte-exact match between SIMD and scalar */
				for (size_t p = 0; p < MAX_AV_PLANES && fa->data[p]; p++) {
					size_t plane_h = h;
					size_t plane_w = w;
					if (fmt == VIDEO_FORMAT_NV12 && p == 1) {
						plane_h = (h + 1) / 2;
						plane_w = w;
					} else if (fmt == VIDEO_FORMAT_UYVY && p == 0) {
						plane_w = (size_t)w * 2;
					} else if (fmt == VIDEO_FORMAT_I420 && p > 0) {
						plane_h = (h + 1) / 2;
						plane_w = (w + 1) / 2;
					}

					for (size_t y = 0; y < plane_h; y++) {
						const uint8_t *row_simd = fd_simd->data[p] + y * fd_simd->linesize[p];
						const uint8_t *row_sc = fd_scalar->data[p] + y * fd_scalar->linesize[p];
						CHECK(memcmp(row_simd, row_sc, plane_w) == 0,
						      "SIMD != scalar mismatch at fmt %d, dim %ux%u, plane %zu, row %zu, w8=%u",
						      fmt, w, h, p, y, w8);

						if (w8 == 0) {
							const uint8_t *row_a = fa->data[p] + y * fa->linesize[p];
							CHECK(memcmp(row_simd, row_a, plane_w) == 0,
							      "w=0 did not return A exactly at fmt %d, row %zu", fmt, y);
						}
						if (w8 == 256) {
							const uint8_t *row_b = fb->data[p] + y * fb->linesize[p];
							CHECK(memcmp(row_simd, row_b, plane_w) == 0,
							      "w=256 did not return B exactly at fmt %d, row %zu", fmt, y);
						}
					}
				}
			}

			obs_source_frame_destroy(fa);
			obs_source_frame_destroy(fb);
			obs_source_frame_destroy(fd_simd);
			obs_source_frame_destroy(fd_scalar);
		}
	}

	/* Hand-computed values test at w8 = 128 */
	struct {
		uint8_t a;
		uint8_t b;
		uint8_t expected;
	} hand_tests[] = {
		{100, 200, 150},
		{0, 255, 128},
		{10, 20, 15},
		{10, 11, 11},
		{0, 0, 0},
		{255, 255, 255},
	};

	struct obs_source_frame *ha = obs_source_frame_create(VIDEO_FORMAT_NV12, 16, 2);
	struct obs_source_frame *hb = obs_source_frame_create(VIDEO_FORMAT_NV12, 16, 2);
	struct obs_source_frame *hd = obs_source_frame_create(VIDEO_FORMAT_NV12, 16, 2);
	CHECK(ha && hb && hd, "hand frame alloc failed");

	for (size_t i = 0; i < sizeof(hand_tests) / sizeof(hand_tests[0]); i++) {
		memset(ha->data[0], hand_tests[i].a, 16);
		memset(hb->data[0], hand_tests[i].b, 16);
		replay_blend_frames(hd, ha, hb, 128);
		CHECK(hd->data[0][0] == hand_tests[i].expected,
		      "Hand-computed blend mismatch: a=%u, b=%u, expected=%u, got=%u",
		      hand_tests[i].a, hand_tests[i].b, hand_tests[i].expected, hd->data[0][0]);
	}

	obs_source_frame_destroy(ha);
	obs_source_frame_destroy(hb);
	obs_source_frame_destroy(hd);

	printf("[test-blend] SIMD vs scalar unit tests and hand-computed checks: PASSED\n");
	return 0;
}

/* =========================================================================
 * 2. Throughput benchmark (1080p NV12 <= 0.5 ms)
 * ========================================================================= */

static int test_throughput_benchmark(void)
{
	struct obs_source_frame *fa = obs_source_frame_create(VIDEO_FORMAT_NV12, 1920, 1080);
	struct obs_source_frame *fb = obs_source_frame_create(VIDEO_FORMAT_NV12, 1920, 1080);
	struct obs_source_frame *fd = obs_source_frame_create(VIDEO_FORMAT_NV12, 1920, 1080);
	CHECK(fa && fb && fd, "1080p frame alloc failed");

	memset(fa->data[0], 100, (size_t)fa->linesize[0] * 1080);
	memset(fa->data[1], 128, (size_t)fa->linesize[1] * 540);
	memset(fb->data[0], 200, (size_t)fb->linesize[0] * 1080);
	memset(fb->data[1], 128, (size_t)fb->linesize[1] * 540);

	/* Warmup */
	for (int i = 0; i < 20; i++) {
		replay_blend_frames(fd, fa, fb, 128);
	}

	const int iters = 200;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < iters; i++) {
		replay_blend_frames(fd, fa, fb, 128);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
	double avg_ms = elapsed_ms / (double)iters;

	/* Informational only: wall-clock limits are not deterministic on a shared machine. */
	printf("[BENCH] 1080p NV12 blend single-threaded: %.3f ms (release target <= 0.5 ms)\n", avg_ms);

	obs_source_frame_destroy(fa);
	obs_source_frame_destroy(fb);
	obs_source_frame_destroy(fd);
	return 0;
}

/* =========================================================================
 * 3. Weight / timestamp schedule (pure function)
 * ========================================================================= */

static size_t unblended_frame_at_time(const uint64_t *src_ts, size_t num_frames, float spd, bool bwd,
				      uint64_t start_ts, uint64_t t)
{
	const uint64_t first_ts = src_ts[0];
	const uint64_t last_ts = src_ts[num_frames - 1];
	size_t cur_pos = 0;
	while (cur_pos + 1 < num_frames) {
		size_t next_idx = bwd ? (num_frames - 1 - (cur_pos + 1)) : (cur_pos + 1);
		uint64_t next_src_ts = src_ts[next_idx];
		uint64_t next_rel = bwd ? (last_ts - next_src_ts) : (next_src_ts - first_ts);
		uint64_t next_t_out = start_ts + (uint64_t)((double)next_rel * 100.0 / (double)spd);
		if (t < next_t_out)
			break;
		cur_pos++;
	}
	return bwd ? (num_frames - 1 - cur_pos) : cur_pos;
}

static int test_weight_timestamp_schedule(void)
{
	printf("[test-blend] Running weight/timestamp schedule pure function tests...\n");

	const uint64_t interval_ns = 1000000000ULL / 60ULL; /* 16666666 ns */
	const size_t num_frames = 10;
	uint64_t src_ts[10];
	for (size_t i = 0; i < num_frames; i++) {
		src_ts[i] = i * interval_ns;
	}

	float speeds[] = {50.0f, 25.0f, 75.0f, 33.0f};

	for (size_t s = 0; s < sizeof(speeds) / sizeof(speeds[0]); s++) {
		float spd = speeds[s];
		for (int bwd_i = 0; bwd_i <= 1; bwd_i++) {
			bool bwd = (bwd_i != 0);
			struct replay_blend_schedule_item items[256];
			size_t count = replay_blend_compute_schedule(src_ts, num_frames, spd, bwd,
								     interval_ns, 1000000000ULL,
								     items, 256);

			CHECK(count > 0, "Schedule returned 0 items for speed %.1f bwd=%d", spd, bwd);

			for (size_t i = 0; i < count; i++) {
				CHECK(items[i].weight_b_256 <= 256,
				      "Weight out of bounds: %u at index %zu", items[i].weight_b_256, i);
				if (i + 1 < count) {
					CHECK(items[i].output_timestamp < items[i + 1].output_timestamp,
					      "Timestamps not strictly increasing at index %zu: %" PRIu64 " >= %" PRIu64,
					      i, items[i].output_timestamp, items[i + 1].output_timestamp);
					CHECK(items[i + 1].output_timestamp - items[i].output_timestamp == interval_ns,
					      "Timestamp spacing mismatch at index %zu: diff %" PRIu64 " != %" PRIu64,
					      i, items[i + 1].output_timestamp - items[i].output_timestamp, interval_ns);
				}

				/* At weight 0, source frame position must match the non-blended path */
				if (items[i].weight_b_256 == 0) {
					size_t expected_frame = unblended_frame_at_time(src_ts, num_frames, spd, bwd,
											1000000000ULL,
											items[i].output_timestamp);
					CHECK(items[i].frame_a_index == expected_frame,
					      "Source frame position does not match non-blended path at weight 0: got %zu, expected %zu",
					      items[i].frame_a_index, expected_frame);
				}
			}

			/* Specific pattern checks */
			if (spd == 50.0f) {
				/* For 50%, weights alternate: 0, 128, 0, 128... */
				for (size_t i = 0; i + 1 < count; i += 2) {
					CHECK(items[i].weight_b_256 == 0,
					      "Expected weight 0 at %zu, got %u", i, items[i].weight_b_256);
					CHECK(items[i + 1].weight_b_256 == 128,
					      "Expected weight 128 at %zu, got %u", i + 1, items[i + 1].weight_b_256);
				}
			} else if (spd == 25.0f) {
				/* For 25%, weights follow 0, 64, 128, 192, 0, 64, 128, 192... */
				uint32_t expected_25[] = {0, 64, 128, 192};
				for (size_t i = 0; i < count && i < 16; i++) {
					CHECK(items[i].weight_b_256 == expected_25[i % 4],
					      "Expected weight %u at %zu, got %u", expected_25[i % 4], i, items[i].weight_b_256);
				}
			}
		}
	}

	printf("[test-blend] Weight/timestamp schedule pure function tests: PASSED\n");
	return 0;
}

/* =========================================================================
 * 4. Headless integration test (synthetic frames, luma verification, Clear flush)
 * ========================================================================= */

static const char *dummy_cam_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Dummy Async Camera";
}

static struct obs_source_info dummy_cam_info = {
	.id = "dummy_blend_cam",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = dummy_cam_get_name,
};

static void feed_async_filter_cb(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	struct obs_source_frame *frame = param;
	const char *id = obs_source_get_unversioned_id(filter);
	if (id && strcmp(id, REPLAY_FILTER_ASYNC_ID) == 0) {
		void *data = obs_obj_get_data(filter);
		if (data && replay_filter_async_info.filter_video)
			replay_filter_async_info.filter_video(data, frame);
	}
}

static void feed_synthetic_frames(obs_source_t *cam, uint32_t width, uint32_t height,
				 uint64_t count, uint64_t start_ts, uint64_t interval_ns, uint8_t first_luma)
{
	struct obs_source_frame frame;
	memset(&frame, 0, sizeof(frame));
	video_frame_init((struct video_frame *)&frame, VIDEO_FORMAT_NV12, width, height);
	frame.width = width;
	frame.height = height;
	frame.format = VIDEO_FORMAT_NV12;
	video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, frame.color_matrix,
				    frame.color_range_min, frame.color_range_max);

	for (uint64_t i = 0; i < count; i++) {
		frame.timestamp = start_ts + i * interval_ns;
		/* Encode frame index into luma: Frame 0 = 40, Frame 1 = 80, Frame 2 = 120, Frame 3 = 160 */
		memset(frame.data[0], (int)(first_luma + i * 40), (size_t)frame.linesize[0] * height);
		memset(frame.data[1], 128, (size_t)frame.linesize[1] * (height / 2));
		obs_source_enum_filters(cam, feed_async_filter_cb, &frame);
	}

	video_frame_free((struct video_frame *)&frame);
}

struct integration_hook_data {
	pthread_mutex_t lock;
	struct {
		uint64_t timestamp;
		uint8_t luma;
	} frames[64];
	size_t count;
};

static void test_output_hook_cb(void *param, obs_source_t *source, const struct obs_source_frame *frame)
{
	UNUSED_PARAMETER(source);
	struct integration_hook_data *hd = param;
	pthread_mutex_lock(&hd->lock);
	if (hd->count < 64 && frame && frame->data[0]) {
		hd->frames[hd->count].timestamp = frame->timestamp;
		hd->frames[hd->count].luma = frame->data[0][0];
		hd->count++;
	}
	pthread_mutex_unlock(&hd->lock);
}

#define VISIBILITY_ACTION_NONE 3

/* Render the actual source, not just the worker callback: the half-size texture must still cover the original
 * scene rectangle, and a paused resolution switch must preserve its pixels. */
static bool preview_image(obs_source_t *source, uint32_t native_width, uint8_t pixel[4])
{
	bool ok = false;
	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(1280, 720, GS_BGRA);
	if (tr && stage && gs_texrender_begin(tr, 1280, 720)) {
		struct vec4 bg;
		vec4_zero(&bg);
		gs_clear(GS_CLEAR_COLOR, &bg, 0.0f, 0);
		gs_matrix_push();
		gs_matrix_identity();
		gs_ortho(0, 1280, 0, 720, -100, 100);
		obs_source_video_render(source);
		gs_matrix_pop();
		gs_texrender_end(tr);
		gs_stage_texture(stage, gs_texrender_get_texture(tr));
		uint8_t *data = NULL;
		uint32_t pitch = 0;
		if (gs_stagesurface_map(stage, &data, &pitch)) {
			memcpy(pixel, data + 360 * pitch + 640 * 4, 4);
			ok = data[8 * pitch + 8 * 4 + 3] == 255 &&
			     data[712 * pitch + 1272 * 4 + 3] == 255 &&
			     pixel[0] > 8 && obs_source_get_base_width(source) == native_width &&
			     obs_source_get_base_height(source) == native_width * 720 / 1280 &&
			     obs_source_get_width(source) == 1280 && obs_source_get_height(source) == 720;
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
	obs_leave_graphics();
	return ok;
}

static bool await_preview_image(obs_source_t *source, uint32_t native_width, uint8_t pixel[4])
{
	const uint64_t end = os_gettime_ns() + 2000000000ULL;
	do {
		if (preview_image(source, native_width, pixel))
			return true;
		os_sleep_ms(5);
	} while (os_gettime_ns() < end);
	return false;
}

static bool same_preview_pixel(const uint8_t a[4], const uint8_t b[4])
{
	for (size_t i = 0; i < 4; i++) {
		if (abs((int)a[i] - (int)b[i]) > 2)
			return false;
	}
	return true;
}

static int test_preview_half_integration(void)
{
	obs_source_t *cam = obs_source_create("dummy_blend_cam", "Synthetic Half Cam", NULL, NULL);
	obs_source_t *rs = obs_source_create("replay_source", "Synthetic Half Replay", NULL, NULL);
	CHECK(cam && rs, "preview source creation failed");
	obs_data_t *settings = obs_source_get_settings(rs);
	obs_data_set_string(settings, SETTING_SOURCE, "Synthetic Half Cam");
	obs_data_set_int(settings, SETTING_DURATION, 250);
	obs_data_set_int(settings, SETTING_VISIBILITY_ACTION, 3); /* keep paused across activation */
	obs_data_set_bool(settings, SETTING_PREVIEW_HALF, true);
	obs_source_update(rs, settings);
	obs_source_load(rs);
	obs_data_release(settings);

	const uint64_t step = 1000000000ULL / 60;
	const uint64_t base = os_gettime_ns();
	feed_synthetic_frames(cam, 1280, 720, 10, base, step, 40);
	struct replay_source *ctx = obs_obj_get_data(rs);
	uint64_t first, last;
	CHECK(replay_source_load_unaligned(ctx, &first, &last, NULL), "half preview load failed");
	uint8_t original[4], pixel[4];
	CHECK(await_preview_image(rs, 640, original), "half preview lost logical size or corner coverage");

	obs_source_inc_active(rs);
	CHECK(await_preview_image(rs, 1280, pixel), "paused program did not restore full texture");
	CHECK(same_preview_pixel(original, pixel), "paused program cut changed the displayed frame");
	obs_source_dec_active(rs);
	CHECK(await_preview_image(rs, 640, pixel), "paused preview did not halve after leaving program");
	CHECK(same_preview_pixel(original, pixel), "deactivation changed the paused image");

	for (int enabled = 0; enabled < 2; enabled++) {
		settings = obs_source_get_settings(rs);
		obs_data_set_bool(settings, SETTING_PREVIEW_HALF, enabled != 0);
		obs_source_update(rs, settings);
		obs_data_release(settings);
		CHECK(await_preview_image(rs, enabled ? 640 : 1280, pixel), "paused option toggle did not refresh");
		CHECK(same_preview_pixel(original, pixel), "paused option toggle changed the frame");
	}

	CHECK(replay_source_execute_action(ctx, "Clear", false), "Clear failed");
	CHECK(obs_source_get_width(rs) == 0, "Clear left a visible preview frame");
	/* Reload from a fresh capture source with different pixels: Clear must not retain the previous texture. */
	obs_source_t *replacement = obs_source_create("dummy_blend_cam", "Replacement Half Cam", NULL, NULL);
	CHECK(replacement, "replacement camera creation failed");
	settings = obs_source_get_settings(rs);
	obs_data_set_string(settings, SETTING_SOURCE, "Replacement Half Cam");
	obs_source_update(rs, settings);
	obs_source_load(rs);
	obs_data_release(settings);
	feed_synthetic_frames(replacement, 1280, 720, 2, os_gettime_ns(), step, 140);
	CHECK(replay_source_load_unaligned(ctx, &first, &last, NULL), "reload after Clear failed");
	CHECK(await_preview_image(rs, 640, pixel), "reloaded clip did not produce a half preview");
	CHECK(!same_preview_pixel(original, pixel), "reload displayed the cleared clip");

	/* Blend-always still uses a half texture outside program and a full one on program. */
	settings = obs_source_get_settings(rs);
	obs_data_set_int(settings, SETTING_DURATION, 250);
	obs_source_update(rs, settings);
	obs_source_load(rs);
	obs_data_release(settings);
	feed_synthetic_frames(replacement, 1280, 720, 10, os_gettime_ns(), step, 140);
	CHECK(replay_source_load_unaligned(ctx, &first, &last, NULL), "blend preview load failed");
	replay_source_set_slowmo_blend(ctx, SLOWMO_BLEND_ALWAYS);
	replay_source_set_speed(ctx, 25.0f);
	replay_source_restart_playback(ctx);
	CHECK(await_preview_image(rs, 640, pixel), "blend-always preview not reduced");
	replay_source_toggle_pause(ctx);
	/* Let the last requested worker frame arrive before comparing the paused presentation. */
	os_sleep_ms(50);
	CHECK(await_preview_image(rs, 640, original), "paused blended preview missing");
	obs_source_inc_active(rs);
	CHECK(await_preview_image(rs, 1280, pixel), "paused blended program not full resolution");
	CHECK(same_preview_pixel(original, pixel), "program cut changed the paused blend");
	obs_source_dec_active(rs);
	obs_source_release(rs);
	obs_source_release(cam);
	obs_source_release(replacement);
	printf("[test-blend] Half-preview texture/geometry/pause/clear regression: PASSED\n");
	return 0;
}
static int test_headless_integration(void)
{
	printf("[test-blend] Running headless integration test with synthetic frames...\n");

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

	obs_register_source(&dummy_cam_info);
	CHECK(obs_module_load(), "obs_module_load failed");

	/* Create dummy camera and replay source */
	obs_source_t *cam = obs_source_create("dummy_blend_cam", "Synthetic Cam", NULL, NULL);
	obs_source_t *rs = obs_source_create("replay_source", "Synthetic Replay", NULL, NULL);
	CHECK(cam && rs, "source creation failed");

	obs_data_t *settings = obs_source_get_settings(rs);
	obs_data_set_string(settings, SETTING_SOURCE, "Synthetic Cam");
	obs_data_set_int(settings, SETTING_DURATION, 5000);
	obs_data_set_double(settings, SETTING_SPEED, 25.0);
	obs_data_set_int(settings, SETTING_SLOWMO_BLEND, SLOWMO_BLEND_PROGRAM);
	obs_source_update(rs, settings);
	obs_source_load(rs);
	obs_data_release(settings);

	/* Set up integration hook */
	struct integration_hook_data hd;
	memset(&hd, 0, sizeof(hd));
	pthread_mutex_init(&hd.lock, NULL);

	replay_source_set_blend_output_hook(rs, test_output_hook_cb, &hd);

	/* Feed 10 synthetic frames with luma encoding the frame index */
	const uint64_t interval_ns = 1000000000ULL / 60ULL;
	uint64_t base_ts = obs_get_video_frame_time();
	if (!base_ts)
		base_ts = os_gettime_ns();

	feed_synthetic_frames(cam, 320, 180, 10, base_ts, interval_ns, 40);

	/* Retrieve the loaded replay into rs */
	struct replay_source *ctx = obs_obj_get_data(rs);
	uint64_t f_first = 0, f_last = 0;
	CHECK(replay_source_load_unaligned(ctx, &f_first, &f_last, NULL), "replay_source_load_unaligned failed");

	/* Start playback at 25% speed while the source is only loaded, not on program: no blending */
	replay_source_set_speed(ctx, 25.0f);
	replay_source_restart_playback(ctx);
	for (int k = 0; k < 6; k++) {
		usleep(17000);
	}
	pthread_mutex_lock(&hd.lock);
	CHECK(hd.count == 0, "Inactive (non-program) source must not blend, got %zu blended frames", hd.count);
	pthread_mutex_unlock(&hd.lock);

	/* "Always" blends without being on program (Multiview/preview), and the mode is stored in the settings */
	replay_source_set_slowmo_blend(ctx, SLOWMO_BLEND_ALWAYS);
	replay_source_restart_playback(ctx);
	for (int k = 0; k < 6; k++) {
		usleep(17000);
	}
	pthread_mutex_lock(&hd.lock);
	CHECK(hd.count > 0, "SLOWMO_BLEND_ALWAYS must blend while inactive, got 0 blended frames");
	pthread_mutex_unlock(&hd.lock);
	obs_data_t *stored = obs_source_get_settings(rs);
	CHECK(obs_data_get_int(stored, SETTING_SLOWMO_BLEND) == SLOWMO_BLEND_ALWAYS,
	      "SetSlowmoBlend not stored in source settings");
	obs_data_release(stored);
	replay_source_set_slowmo_blend(ctx, SLOWMO_BLEND_PROGRAM);

	/* On program (active): blending runs. Activation reaches the source on the next graphics tick. */
	obs_source_inc_active(rs);
	usleep(50000);
	pthread_mutex_lock(&hd.lock);
	hd.count = 0;
	pthread_mutex_unlock(&hd.lock);
	replay_source_restart_playback(ctx);

	/* Wait ~150 ms so the video clock advances and ~8 ticks are processed */
	for (int k = 0; k < 12; k++) {
		usleep(17000);
	}

	pthread_mutex_lock(&hd.lock);
	CHECK(hd.count >= 5, "Expected at least 5 frames delivered in 25%% slowmo, got %zu", hd.count);

	printf("[test-blend] Collected %zu frames in 25%% slowmo test:\n", hd.count);
	for (size_t i = 0; i < hd.count; i++) {
		printf("  [%zu] timestamp=%" PRIu64 ", luma=%u\n", i, hd.frames[i].timestamp, hd.frames[i].luma);
		if (i + 1 < hd.count) {
			CHECK(hd.frames[i].timestamp < hd.frames[i + 1].timestamp,
			      "Monotonic timestamp guard failed: %" PRIu64 " >= %" PRIu64,
			      hd.frames[i].timestamp, hd.frames[i + 1].timestamp);
		}
	}

	/* First 5 frames at 25% between Frame 0 (luma 40) and Frame 1 (luma 80):
	 * w=0   -> 40
	 * w=64  -> ((256-64)*40 + 64*80 + 128) >> 8 = 50
	 * w=128 -> ((256-128)*40 + 128*80 + 128) >> 8 = 60
	 * w=192 -> ((256-192)*40 + 192*80 + 128) >> 8 = 70
	 * w=0   -> Frame 1 = 80
	 */
	CHECK(hd.frames[0].luma == 40, "Frame 0 luma mismatch: expected 40, got %u", hd.frames[0].luma);
	CHECK(hd.frames[1].luma == 50, "Frame 1 (intermediate 25%%) luma mismatch: expected 50, got %u", hd.frames[1].luma);
	CHECK(hd.frames[2].luma == 60, "Frame 2 (intermediate 50%%) luma mismatch: expected 60, got %u", hd.frames[2].luma);
	CHECK(hd.frames[3].luma == 70, "Frame 3 (intermediate 75%%) luma mismatch: expected 70, got %u", hd.frames[3].luma);
	CHECK(hd.frames[4].luma == 80, "Frame 4 (Frame 1) luma mismatch: expected 80, got %u", hd.frames[4].luma);

	/* Now test: Clear during playback flushes the worker (no frame of the cleared replay after Clear returns + one tick) */
	size_t count_before_clear = hd.count;
	pthread_mutex_unlock(&hd.lock);

	/* Trigger Clear action */
	CHECK(replay_source_execute_action(ctx, "Clear", false), "execute Clear action failed");

	/* Wait 2 ticks (~35 ms) */
	usleep(35000);

	pthread_mutex_lock(&hd.lock);
	size_t count_after_clear = hd.count;
	CHECK(count_after_clear == count_before_clear,
	      "Clear during playback did not flush worker: received %zu frames after Clear!",
	      count_after_clear - count_before_clear);
	pthread_mutex_unlock(&hd.lock);

	printf("[test-blend] Clear during playback cleanly flushed worker: 0 frames delivered after Clear.\n");

	obs_source_dec_active(rs);
	obs_source_release(cam);
	obs_source_release(rs);
	pthread_mutex_destroy(&hd.lock);

	printf("[test-blend] Running preview-half regression integration test...\n");
	if (test_preview_half_integration() != 0) {
		obs_module_unload();
		obs_shutdown();
		return 1;
	}

	obs_module_unload();
	obs_shutdown();

	printf("[test-blend] Headless integration test: PASSED\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("[test-blend] No DISPLAY set, re-executing under xvfb-run -a...\n");
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

	if (test_simd_vs_scalar() != 0)
		return 1;

	if (test_throughput_benchmark() != 0)
		return 1;

	if (test_weight_timestamp_schedule() != 0)
		return 1;

	if (test_headless_integration() != 0)
		return 1;

	printf("[test-blend] ALL SLOWMO BLEND TESTS PASSED!\n");
	return 0;
}
