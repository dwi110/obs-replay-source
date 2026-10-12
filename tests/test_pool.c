#include "replay.h"
#include "replay-pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal stubs for symbols referenced by replay.c */
struct obs_source_info replay_source_info = {.id = REPLAY_SOURCE_ID, .type = OBS_SOURCE_TYPE_INPUT};
struct obs_source_info replay_filter_info = {.id = REPLAY_FILTER_ID, .type = OBS_SOURCE_TYPE_FILTER};
struct obs_source_info replay_filter_audio_info = {.id = REPLAY_FILTER_AUDIO_ID, .type = OBS_SOURCE_TYPE_FILTER};
struct obs_source_info replay_filter_async_info = {.id = REPLAY_FILTER_ASYNC_ID, .type = OBS_SOURCE_TYPE_FILTER};
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

static int test_acquire_release_reuse(void)
{
	struct replay_pool *pool = replay_pool_create(VIDEO_FORMAT_NV12, 320, 180, 0);
	CHECK(pool != NULL, "pool creation failed");

	struct replay_pool_stats stats;
	replay_pool_get_stats(pool, &stats);
	CHECK(stats.allocated_frames == 0 && stats.in_use_frames == 0, "initial stats non-zero");

	struct obs_source_frame *f1 = replay_pool_acquire(pool);
	CHECK(f1 != NULL, "acquire f1 failed");
	CHECK(f1->refs == 1, "expected refs == 1, got %ld", f1->refs);
	CHECK(f1->format == VIDEO_FORMAT_NV12 && f1->width == 320 && f1->height == 180, "frame geometry mismatch");
	CHECK(f1->data[0] != NULL && f1->data[1] != NULL, "NV12 planes NULL");

	uint8_t *orig_data0 = f1->data[0];
	f1->timestamp = 123456789ULL;
	f1->full_range = true;

	replay_pool_get_stats(pool, &stats);
	CHECK(stats.allocated_frames == 1 && stats.in_use_frames == 1 && stats.peak_frames == 1,
	      "stats after 1 acquire wrong: alloc=%u in_use=%u peak=%u", stats.allocated_frames, stats.in_use_frames,
	      stats.peak_frames);
	CHECK(stats.allocated_bytes == replay_frame_byte_size(VIDEO_FORMAT_NV12, 320, 180), "allocated_bytes mismatch");

	replay_frame_release(f1);
	replay_pool_get_stats(pool, &stats);
	CHECK(stats.allocated_frames == 1 && stats.in_use_frames == 0 && stats.peak_frames == 1,
	      "stats after release wrong: alloc=%u in_use=%u peak=%u", stats.allocated_frames, stats.in_use_frames,
	      stats.peak_frames);

	/* Re-acquiring must reuse the exact same frame from the free list and reset metadata */
	struct obs_source_frame *f2 = replay_pool_acquire(pool);
	CHECK(f2 == f1, "expected frame pointer reuse from pool free list");
	CHECK(f2->data[0] == orig_data0, "expected plane data reuse");
	CHECK(f2->refs == 1 && f2->timestamp == 0 && f2->full_range == false, "metadata not reset on re-acquire");

	replay_frame_release(f2);
	replay_pool_close(pool);
	return 0;
}

static int test_addref_sharing(void)
{
	struct replay_pool *pool = replay_pool_create(VIDEO_FORMAT_NV12, 160, 120, 0);
	CHECK(pool != NULL, "pool creation failed");

	struct obs_source_frame *f = replay_pool_acquire(pool);
	CHECK(f != NULL, "acquire failed");

	/* Simulate filter + loaded replay + save queue holding the same frame */
	replay_frame_addref(f);
	replay_frame_addref(f);
	CHECK(f->refs == 3, "expected refs == 3, got %ld", f->refs);

	struct replay_pool_stats stats;
	replay_frame_release(f);
	replay_pool_get_stats(pool, &stats);
	CHECK(f->refs == 2 && stats.in_use_frames == 1, "frame returned too early after 1st release");

	replay_frame_release(f);
	replay_pool_get_stats(pool, &stats);
	CHECK(f->refs == 1 && stats.in_use_frames == 1, "frame returned too early after 2nd release");

	/* Acquiring another frame while f is still held must allocate a 2nd frame */
	struct obs_source_frame *other = replay_pool_acquire(pool);
	CHECK(other != NULL && other != f, "expected distinct second frame while first is shared");
	replay_pool_get_stats(pool, &stats);
	CHECK(stats.allocated_frames == 2 && stats.in_use_frames == 2 && stats.peak_frames == 2,
	      "stats mismatch with 2 frames in use");

	replay_frame_release(other);
	replay_frame_release(f);
	replay_pool_get_stats(pool, &stats);
	CHECK(stats.allocated_frames == 2 && stats.in_use_frames == 0 && stats.peak_frames == 2,
	      "stats mismatch after releasing both frames");

	replay_pool_close(pool);
	return 0;
}

static int test_warmup_and_close_with_outstanding(void)
{
	struct replay_pool *pool = replay_pool_create(VIDEO_FORMAT_NV12, 640, 360, 12);
	CHECK(pool != NULL, "pool creation failed");

	replay_pool_wait_warmup(pool);

	struct replay_pool_stats stats;
	replay_pool_get_stats(pool, &stats);
	CHECK(stats.allocated_frames == 12 && stats.in_use_frames == 0,
	      "warmup expected 12 allocated frames, got %u", stats.allocated_frames);

	struct obs_source_frame *held[4];
	for (int i = 0; i < 4; i++) {
		held[i] = replay_pool_acquire(pool);
		CHECK(held[i] != NULL, "acquire %d failed", i);
	}
	replay_frame_addref(held[0]);

	replay_pool_get_stats(pool, &stats);
	CHECK(stats.allocated_frames == 12 && stats.in_use_frames == 4 && stats.peak_frames == 4,
	      "stats before close mismatch");

	/* Close the pool while 4 frames (one with refs=2) are still lent out */
	replay_pool_close(pool);

	/* Write to lent-out frame memory after pool close to prove frames remain valid */
	for (int i = 0; i < 4; i++) {
		memset(held[i]->data[0], 0x42, 640 * 360);
	}

	/* Drop extra ref on held[0], then release all 4 frames */
	replay_frame_release(held[0]);
	for (int i = 0; i < 4; i++) {
		replay_frame_release(held[i]);
	}

	return 0;
}

int main(void)
{
	if (test_acquire_release_reuse() != 0)
		return 1;
	if (test_addref_sharing() != 0)
		return 1;
	if (test_warmup_and_close_with_outstanding() != 0)
		return 1;

	printf("test_pool: ALL PASSED\n");
	return 0;
}
