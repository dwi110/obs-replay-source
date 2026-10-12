#pragma once

#include <obs.h>
#include <util/threading.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REPLAY_POOL_FRAME_MAGIC 0x52504652U /* 'RPFR' */

struct replay_pool;

struct replay_pool_frame {
	struct obs_source_frame frame; /* MUST be first member */
	struct replay_pool *pool;
	struct replay_pool_frame *next_free;
	size_t alloc_bytes;
	uint32_t magic;
};

struct replay_pool_stats {
	uint32_t allocated_frames;
	uint32_t in_use_frames;
	uint32_t peak_frames;
	uint64_t allocated_bytes;
};

size_t replay_frame_byte_size(enum video_format format, uint32_t width, uint32_t height);

struct replay_pool *replay_pool_create(enum video_format format, uint32_t width, uint32_t height,
				       uint32_t target_frames);
void replay_pool_warmup(struct replay_pool *pool, uint32_t target_frames);
void replay_pool_wait_warmup(struct replay_pool *pool);
void replay_pool_close(struct replay_pool *pool);

struct obs_source_frame *replay_pool_acquire(struct replay_pool *pool);
void replay_pool_release_frame(struct obs_source_frame *frame);

void replay_pool_get_stats(struct replay_pool *pool, struct replay_pool_stats *stats);
enum video_format replay_pool_get_format(const struct replay_pool *pool);
uint32_t replay_pool_get_width(const struct replay_pool *pool);
uint32_t replay_pool_get_height(const struct replay_pool *pool);

#ifdef __cplusplus
}
#endif
