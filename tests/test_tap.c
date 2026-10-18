#include "obs-frame-tap.h"
#include "replay.h"
#include "replay-pool.h"
#include "replay-convert.h"
#include <util/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal stubs for symbols referenced by replay.c */
struct obs_source_info replay_source_info = {.id = REPLAY_SOURCE_ID, .type = OBS_SOURCE_TYPE_INPUT};
struct obs_source_info replay_filter_info = {.id = REPLAY_FILTER_ID, .type = OBS_SOURCE_TYPE_FILTER};
struct obs_source_info replay_filter_audio_info = {.id = REPLAY_FILTER_AUDIO_ID, .type = OBS_SOURCE_TYPE_FILTER};
extern struct obs_source_info replay_filter_async_info;
void replay_trigger_threshold(void *data)
{
	UNUSED_PARAMETER(data);
}

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			return 1;                                              \
		}                                                              \
	} while (0)

/*
 * Lightweight fake obs_source_t and obs_weak_source_t for headless unit testing
 * without requiring an X11/Wayland display server. Uses real libobs proc_handler_t,
 * calldata_t, and obs_data_t.
 */
struct obs_source {
	const char *name;
	proc_handler_t *procs;
	obs_source_t *parent;
};

struct obs_weak_source {
	obs_source_t *source;
};

obs_source_t *obs_filter_get_parent(const obs_source_t *filter)
{
	return filter ? filter->parent : NULL;
}

proc_handler_t *obs_source_get_proc_handler(const obs_source_t *source)
{
	return source ? source->procs : NULL;
}

const char *obs_source_get_name(const obs_source_t *source)
{
	return source ? source->name : NULL;
}

obs_weak_source_t *obs_source_get_weak_source(obs_source_t *source)
{
	if (!source)
		return NULL;
	obs_weak_source_t *w = bzalloc(sizeof(obs_weak_source_t));
	w->source = source;
	return w;
}

obs_source_t *obs_weak_source_get_source(obs_weak_source_t *weak)
{
	return weak ? weak->source : NULL;
}

void obs_weak_source_release(obs_weak_source_t *weak)
{
	bfree(weak);
}

void obs_source_release(obs_source_t *source)
{
	UNUSED_PARAMETER(source);
}

obs_source_t *obs_get_source_by_name(const char *name)
{
	UNUSED_PARAMETER(name);
	static struct obs_source dummy_owner = {.name = "test_replay_owner"};
	return &dummy_owner;
}

obs_data_t *obs_source_get_settings(const obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	return obs_data_create();
}

struct fake_tap_provider {
	obs_source_t source;
	pthread_mutex_t mutex;
	frame_tap_cb cb;
	void *cb_param;
	int add_calls;
	int remove_calls;
	int emit_calls;
};

static void fake_tap_add_proc(void *data, calldata_t *cd)
{
	struct fake_tap_provider *ftp = data;
	frame_tap_cb cb = (frame_tap_cb)(uintptr_t)calldata_ptr(cd, "callback");
	void *param = calldata_ptr(cd, "param");

	pthread_mutex_lock(&ftp->mutex);
	ftp->cb = cb;
	ftp->cb_param = param;
	ftp->add_calls++;
	pthread_mutex_unlock(&ftp->mutex);
}

static void fake_tap_remove_proc(void *data, calldata_t *cd)
{
	struct fake_tap_provider *ftp = data;
	frame_tap_cb cb = (frame_tap_cb)(uintptr_t)calldata_ptr(cd, "callback");
	void *param = calldata_ptr(cd, "param");

	pthread_mutex_lock(&ftp->mutex);
	if (ftp->cb == cb && ftp->cb_param == param) {
		ftp->cb = NULL;
		ftp->cb_param = NULL;
	}
	ftp->remove_calls++;
	pthread_mutex_unlock(&ftp->mutex);
}

static void fake_tap_emit_frame(struct fake_tap_provider *ftp, const struct obs_source_frame *frame)
{
	pthread_mutex_lock(&ftp->mutex);
	frame_tap_cb cb = ftp->cb;
	void *param = ftp->cb_param;
	if (cb) {
		ftp->emit_calls++;
		cb(param, &ftp->source, frame);
	}
	pthread_mutex_unlock(&ftp->mutex);
}

int main(void)
{
	struct fake_tap_provider cam = {0};
	cam.source.name = "fake_ndi_cam";
	cam.source.procs = proc_handler_create();
	pthread_mutex_init(&cam.mutex, NULL);

	proc_handler_add(cam.source.procs, FRAME_TAP_PROC_ADD_DECL, fake_tap_add_proc, &cam);
	proc_handler_add(cam.source.procs, FRAME_TAP_PROC_REMOVE_DECL, fake_tap_remove_proc, &cam);

	struct obs_source filter_src = {
		.name = "test_replay_filter",
		.procs = NULL,
		.parent = NULL, /* Parent unknown during create, just like obs_source_create_private */
	};

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, SETTING_DURATION, 5000);
	obs_data_set_int(settings, SETTING_STORAGE_FORMAT, STORAGE_FORMAT_NV12);
	obs_data_set_int(settings, SETTING_DOWNSCALE, DOWNSCALE_NONE);

	struct replay_filter *rf = replay_filter_async_info.create(settings, &filter_src);
	CHECK(rf != NULL, "replay_filter_async create failed");
	CHECK(cam.add_calls == 0 && !os_atomic_load_bool(&rf->tap_active),
	      "tap should not attach before parent source is known");

	/* Attach filter to parent camera source and trigger lazy attachment via video_tick */
	filter_src.parent = &cam.source;
	replay_filter_async_info.video_tick(rf, 0.016f);

	CHECK(cam.add_calls == 1 && cam.cb != NULL, "expected frame_tap_add to be called once on parent proc handler");
	CHECK(os_atomic_load_bool(&rf->tap_active), "expected tap_active == true");

	/* Prepare a 64x32 UYVY source frame */
	struct replay_pool *uyvy_pool = replay_pool_create(VIDEO_FORMAT_UYVY, 64, 32, 1);
	struct obs_source_frame *src_frame = replay_pool_acquire(uyvy_pool);
	memset(src_frame->data[0], 0x80, 64 * 32 * 2);

	const uint64_t base_ts = os_gettime_ns();
	for (int i = 0; i < 3; i++) {
		src_frame->timestamp = base_ts + (uint64_t)i * 16666667ULL;
		fake_tap_emit_frame(&cam, src_frame);
	}

	size_t buffered = rf->video_frames.size / sizeof(struct obs_source_frame *);
	CHECK(buffered == 3, "expected 3 frames captured via tap callback, got %zu", buffered);

	/* Verify stored frame was converted into an NV12 pool frame */
	struct obs_source_frame *stored = NULL;
	circlebuf_peek_front(&rf->video_frames, &stored, sizeof(struct obs_source_frame *));
	CHECK(stored != NULL && stored->format == VIDEO_FORMAT_NV12 && stored->width == 64 && stored->height == 32,
	      "stored tap frame format/size wrong");

	/* While tap is active, filter_video must pass through the frame untouched without capturing again */
	src_frame->timestamp = base_ts + 3ULL * 16666667ULL;
	struct obs_source_frame *ret = replay_filter_async_info.filter_video(rf, src_frame);
	CHECK(ret == src_frame, "filter_video did not return input frame untouched");
	buffered = rf->video_frames.size / sizeof(struct obs_source_frame *);
	CHECK(buffered == 3, "filter_video should not duplicate capture while tap is active (got %zu)", buffered);

	/* Live delivery lateness: a frame that arrives 300 ms later than its predecessors relative to its timestamp
	 * reports ~300 ms, an on-time frame clears it, silence beyond 100 ms adds up and a camera silent for 5 s is
	 * offline (0). */
	struct replay_live_entry live[4];
	size_t live_n = 0;
	uint64_t late = replay_filter_live_snapshot(os_gettime_ns(), live, 4, &live_n);
	CHECK(live_n == 1 && strcmp(live[0].name, "test_replay_filter") == 0, "expected one live entry, got %zu",
	      live_n);
	CHECK(late < 50000000ULL, "on-time frames reported %.1f ms late", (double)late / 1e6);

	os_sleep_ms(300);
	src_frame->timestamp = base_ts + 4ULL * 16666667ULL;
	fake_tap_emit_frame(&cam, src_frame);
	late = replay_filter_live_snapshot(os_gettime_ns(), NULL, 0, NULL);
	CHECK(late >= 230000000ULL && late < 400000000ULL, "late frame reported %.1f ms, expected ~283 ms",
	      (double)late / 1e6);

	src_frame->timestamp = os_gettime_ns() + 40000000ULL;
	fake_tap_emit_frame(&cam, src_frame);
	late = replay_filter_live_snapshot(os_gettime_ns(), NULL, 0, NULL);
	CHECK(late < 20000000ULL, "on-time frame after a late one still reports %.1f ms", (double)late / 1e6);

	/* Silence is not lateness (NDI sources may send only on change); a late value older than 2 s is stale. */
	os_sleep_ms(400);
	late = replay_filter_live_snapshot(os_gettime_ns(), NULL, 0, NULL);
	CHECK(late < 20000000ULL, "400 ms silence after an on-time frame reported %.1f ms", (double)late / 1e6);
	os_sleep_ms(300);
	src_frame->timestamp += 16666667ULL;
	fake_tap_emit_frame(&cam, src_frame);
	late = replay_filter_live_snapshot(os_gettime_ns(), NULL, 0, NULL);
	CHECK(late >= 500000000ULL, "frame 700 ms after its predecessor reported %.1f ms", (double)late / 1e6);
	late = replay_filter_live_snapshot(os_gettime_ns() + 3000000000ULL, NULL, 0, NULL);
	CHECK(late == 0, "late value of a camera silent for 3 s should be stale, got %.1f ms", (double)late / 1e6);

	/* Removing the filter must call frame_tap_remove and stop callbacks */
	replay_filter_async_info.filter_remove(rf, &cam.source);
	CHECK(cam.remove_calls == 1 && cam.cb == NULL, "expected frame_tap_remove to unregister callback");
	CHECK(!os_atomic_load_bool(&rf->tap_active), "expected tap_active == false after remove");

	int prev_emits = cam.emit_calls;
	src_frame->timestamp = base_ts + 4ULL * 16666667ULL;
	fake_tap_emit_frame(&cam, src_frame);
	CHECK(cam.emit_calls == prev_emits, "callback was invoked after frame_tap_remove");

	/* libobs clears filter_parent only after filter_remove returns, and the graphics thread can tick the
	 * filter in between. A removed filter must never attach again: its tap would outlive the filter. */
	replay_filter_async_info.video_tick(rf, 0.016f);
	CHECK(cam.add_calls == 1 && cam.cb == NULL, "removed filter re-attached its frame tap (add_calls=%d)",
	      cam.add_calls);
	filter_src.parent = NULL;

	replay_filter_async_info.destroy(rf);
	obs_data_release(settings);

	replay_frame_release(src_frame);
	replay_pool_close(uyvy_pool);

	proc_handler_destroy(cam.source.procs);
	pthread_mutex_destroy(&cam.mutex);

	replay_filter_live_snapshot(os_gettime_ns(), live, 4, &live_n);
	CHECK(live_n == 0, "destroyed filter still listed for live lateness (%zu entries)", live_n);

	printf("test_tap: ALL PASSED\n");
	return 0;
}
