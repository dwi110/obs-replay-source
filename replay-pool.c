#include "replay-pool.h"
#include <util/bmem.h>
#include <string.h>

#define ALIGN_SIZE(size, align) size = (((size) + (align - 1)) & (~(align - 1)))

struct replay_pool {
	enum video_format format;
	uint32_t width;
	uint32_t height;
	size_t frame_bytes;

	pthread_mutex_t mutex;
	struct replay_pool_frame *free_list;

	uint32_t allocated_frames;
	uint32_t in_use_frames;
	uint32_t peak_frames;
	uint64_t allocated_bytes;

	uint32_t target_frames;
	uint32_t ref_count;
	bool closed;

	pthread_t warmup_thread;
	bool warmup_active;
	bool warmup_thread_valid;
	bool warmup_joining;
	bool stop_warmup;
};

size_t replay_frame_byte_size(enum video_format format, uint32_t width, uint32_t height)
{
	size_t size = 0;
	const size_t alignment = (size_t)base_get_alignment();

	if (width == 0 || height == 0)
		return 0;

	switch (format) {
	case VIDEO_FORMAT_NONE:
		return 0;

	case VIDEO_FORMAT_I420: {
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		const size_t half_width = (width + 1) / 2;
		const size_t half_height = (height + 1) / 2;
		const size_t quarter_area = half_width * half_height;
		size += quarter_area;
		ALIGN_SIZE(size, alignment);
		size += quarter_area;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_NV12: {
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		const size_t cbcr_width = (width + 1) & (UINT32_MAX - 1);
		size += cbcr_width * ((height + 1) / 2);
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_Y800:
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		return size;

	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY: {
		const size_t double_width = ((size_t)((width + 1) & (UINT32_MAX - 1))) * 2;
		size = double_width * height;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_AYUV:
	case VIDEO_FORMAT_R10L:
		size = (size_t)width * height * 4;
		ALIGN_SIZE(size, alignment);
		return size;

	case VIDEO_FORMAT_I444:
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		return size * 3;

	case VIDEO_FORMAT_I412:
		size = (size_t)width * height * 2;
		ALIGN_SIZE(size, alignment);
		return size * 3;

	case VIDEO_FORMAT_BGR3:
		size = (size_t)width * height * 3;
		ALIGN_SIZE(size, alignment);
		return size;

	case VIDEO_FORMAT_I422: {
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		const size_t half_width = (width + 1) / 2;
		const size_t half_area = half_width * height;
		size += half_area;
		ALIGN_SIZE(size, alignment);
		size += half_area;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_I210: {
		size = (size_t)width * height * 2;
		ALIGN_SIZE(size, alignment);
		const size_t half_width = (width + 1) / 2;
		const size_t half_area_size = 2 * half_width * height;
		size += half_area_size;
		ALIGN_SIZE(size, alignment);
		size += half_area_size;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_I40A: {
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		const size_t half_width = (width + 1) / 2;
		const size_t half_height = (height + 1) / 2;
		const size_t quarter_area = half_width * half_height;
		size += quarter_area;
		ALIGN_SIZE(size, alignment);
		size += quarter_area;
		ALIGN_SIZE(size, alignment);
		size += (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_I42A: {
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		const size_t half_width = (width + 1) / 2;
		const size_t half_area = half_width * height;
		size += half_area;
		ALIGN_SIZE(size, alignment);
		size += half_area;
		ALIGN_SIZE(size, alignment);
		size += (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_YUVA:
		size = (size_t)width * height;
		ALIGN_SIZE(size, alignment);
		return size * 4;

	case VIDEO_FORMAT_YA2L:
		size = (size_t)width * 2 * height;
		ALIGN_SIZE(size, alignment);
		return size * 4;

	case VIDEO_FORMAT_I010: {
		size = (size_t)width * height * 2;
		ALIGN_SIZE(size, alignment);
		const size_t half_width = (width + 1) / 2;
		const size_t half_height = (height + 1) / 2;
		const size_t quarter_area = half_width * half_height * 2;
		size += quarter_area;
		ALIGN_SIZE(size, alignment);
		size += quarter_area;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_P010: {
		size = (size_t)width * height * 2;
		ALIGN_SIZE(size, alignment);
		const size_t cbcr_width = (width + 1) & (UINT32_MAX - 1);
		size += cbcr_width * ((height + 1) / 2) * 2;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_P216: {
		size = (size_t)width * height * 2;
		ALIGN_SIZE(size, alignment);
		const size_t cbcr_width = (width + 1) & (UINT32_MAX - 1);
		size += cbcr_width * height * 2;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_P416: {
		size = (size_t)width * height * 2;
		ALIGN_SIZE(size, alignment);
		size += (size_t)width * height * 4;
		ALIGN_SIZE(size, alignment);
		return size;
	}

	case VIDEO_FORMAT_V210: {
		const size_t adjusted_width = ((width + 5) / 6) * 16;
		size = adjusted_width * height;
		ALIGN_SIZE(size, alignment);
		return size;
	}
	}

	return 0;
}

static void prefault_pages(uint8_t *base, size_t bytes)
{
	if (!base || bytes == 0)
		return;
	volatile uint8_t *p = base;
	for (size_t offset = 0; offset < bytes; offset += 4096)
		p[offset] = 0;
	p[bytes - 1] = 0;
}

static struct replay_pool_frame *alloc_and_prefault_frame(struct replay_pool *pool)
{
	struct replay_pool_frame *pf = bzalloc(sizeof(struct replay_pool_frame));
	if (!pf)
		return NULL;

	obs_source_frame_init(&pf->frame, pool->format, pool->width, pool->height);
	if (!pf->frame.data[0]) {
		bfree(pf);
		return NULL;
	}

	pf->pool = pool;
	pf->next_free = NULL;
	pf->alloc_bytes = pool->frame_bytes;
	pf->magic = REPLAY_POOL_FRAME_MAGIC;

	prefault_pages(pf->frame.data[0], pf->alloc_bytes);
	return pf;
}

static void destroy_pool_frame(struct replay_pool_frame *pf)
{
	if (!pf)
		return;
	pf->magic = 0;
	pf->pool = NULL;
	obs_source_frame_free(&pf->frame);
	bfree(pf);
}

static void *warmup_thread_fn(void *arg)
{
	struct replay_pool *pool = arg;
	os_set_thread_name("replay-pool-warmup");

	for (;;) {
		pthread_mutex_lock(&pool->mutex);
		if (pool->stop_warmup || pool->closed || pool->allocated_frames >= pool->target_frames) {
			pool->warmup_active = false;
			pthread_mutex_unlock(&pool->mutex);
			break;
		}
		pthread_mutex_unlock(&pool->mutex);

		struct replay_pool_frame *pf = alloc_and_prefault_frame(pool);
		if (!pf) {
			pthread_mutex_lock(&pool->mutex);
			pool->warmup_active = false;
			pthread_mutex_unlock(&pool->mutex);
			break;
		}

		pthread_mutex_lock(&pool->mutex);
		if (pool->closed) {
			pthread_mutex_unlock(&pool->mutex);
			destroy_pool_frame(pf);
			break;
		}
		pf->next_free = pool->free_list;
		pool->free_list = pf;
		pool->allocated_frames++;
		pool->allocated_bytes += pf->alloc_bytes;
		pthread_mutex_unlock(&pool->mutex);
	}

	return NULL;
}

static void start_warmup_locked(struct replay_pool *pool, uint32_t target_frames)
{
	if (pool->closed)
		return;
	if (target_frames > pool->target_frames)
		pool->target_frames = target_frames;
	if (pool->allocated_frames >= pool->target_frames || pool->warmup_active)
		return;

	if (pool->warmup_thread_valid && !pool->warmup_joining) {
		pool->warmup_joining = true;
		pthread_t prev = pool->warmup_thread;
		pthread_mutex_unlock(&pool->mutex);
		pthread_join(prev, NULL);
		pthread_mutex_lock(&pool->mutex);
		pool->warmup_thread_valid = false;
		pool->warmup_joining = false;
		if (pool->closed || pool->allocated_frames >= pool->target_frames || pool->warmup_active)
			return;
	}

	pool->stop_warmup = false;
	pool->warmup_active = true;
	if (pthread_create(&pool->warmup_thread, NULL, warmup_thread_fn, pool) == 0) {
		pool->warmup_thread_valid = true;
	} else {
		pool->warmup_active = false;
	}
}

struct replay_pool *replay_pool_create(enum video_format format, uint32_t width, uint32_t height,
				       uint32_t target_frames)
{
	if (format == VIDEO_FORMAT_NONE || width == 0 || height == 0)
		return NULL;

	struct replay_pool *pool = bzalloc(sizeof(struct replay_pool));
	if (!pool)
		return NULL;

	pool->format = format;
	pool->width = width;
	pool->height = height;
	pool->frame_bytes = replay_frame_byte_size(format, width, height);
	pool->ref_count = 1;
	pthread_mutex_init(&pool->mutex, NULL);

	if (target_frames > 0) {
		pthread_mutex_lock(&pool->mutex);
		start_warmup_locked(pool, target_frames);
		pthread_mutex_unlock(&pool->mutex);
	}

	return pool;
}

void replay_pool_warmup(struct replay_pool *pool, uint32_t target_frames)
{
	if (!pool || target_frames == 0)
		return;
	pthread_mutex_lock(&pool->mutex);
	start_warmup_locked(pool, target_frames);
	pthread_mutex_unlock(&pool->mutex);
}

void replay_pool_wait_warmup(struct replay_pool *pool)
{
	if (!pool)
		return;
	pthread_mutex_lock(&pool->mutex);
	if (pool->warmup_thread_valid && !pool->warmup_joining) {
		pool->warmup_joining = true;
		pthread_t th = pool->warmup_thread;
		pthread_mutex_unlock(&pool->mutex);
		pthread_join(th, NULL);
		pthread_mutex_lock(&pool->mutex);
		pool->warmup_thread_valid = false;
		pool->warmup_joining = false;
	}
	pthread_mutex_unlock(&pool->mutex);
}

void replay_pool_close(struct replay_pool *pool)
{
	if (!pool)
		return;

	pthread_mutex_lock(&pool->mutex);
	if (pool->closed) {
		pthread_mutex_unlock(&pool->mutex);
		return;
	}
	pool->closed = true;
	pool->stop_warmup = true;
	if (pool->warmup_thread_valid && !pool->warmup_joining) {
		pool->warmup_joining = true;
		pthread_t th = pool->warmup_thread;
		pthread_mutex_unlock(&pool->mutex);
		pthread_join(th, NULL);
		pthread_mutex_lock(&pool->mutex);
		pool->warmup_thread_valid = false;
		pool->warmup_joining = false;
	}

	struct replay_pool_frame *free_head = pool->free_list;
	pool->free_list = NULL;
	while (free_head) {
		struct replay_pool_frame *next = free_head->next_free;
		if (pool->allocated_frames > 0)
			pool->allocated_frames--;
		if (pool->allocated_bytes >= free_head->alloc_bytes)
			pool->allocated_bytes -= free_head->alloc_bytes;
		else
			pool->allocated_bytes = 0;
		destroy_pool_frame(free_head);
		free_head = next;
	}

	pool->ref_count--;
	const bool destroy_now = (pool->ref_count == 0);
	pthread_mutex_unlock(&pool->mutex);

	if (destroy_now) {
		pthread_mutex_destroy(&pool->mutex);
		bfree(pool);
	}
}

static void reset_frame_metadata(struct obs_source_frame *frame)
{
	frame->timestamp = 0;
	memset(frame->color_matrix, 0, sizeof(frame->color_matrix));
	frame->full_range = false;
	frame->max_luminance = 0;
	memset(frame->color_range_min, 0, sizeof(frame->color_range_min));
	memset(frame->color_range_max, 0, sizeof(frame->color_range_max));
	frame->flip = false;
	frame->flags = 0;
	frame->trc = VIDEO_TRC_DEFAULT;
	frame->prev_frame = false;
	os_atomic_set_long(&frame->refs, 1);
}

struct obs_source_frame *replay_pool_acquire(struct replay_pool *pool)
{
	if (!pool)
		return NULL;

	pthread_mutex_lock(&pool->mutex);
	if (pool->closed) {
		pthread_mutex_unlock(&pool->mutex);
		return NULL;
	}

	struct replay_pool_frame *pf = pool->free_list;
	if (pf) {
		pool->free_list = pf->next_free;
		pf->next_free = NULL;
		pool->in_use_frames++;
		if (pool->in_use_frames > pool->peak_frames)
			pool->peak_frames = pool->in_use_frames;
		pool->ref_count++;
		pthread_mutex_unlock(&pool->mutex);

		reset_frame_metadata(&pf->frame);
		return &pf->frame;
	}

	/* Free list empty: fallback to on-demand allocation outside the pool lock */
	pool->in_use_frames++;
	if (pool->in_use_frames > pool->peak_frames)
		pool->peak_frames = pool->in_use_frames;
	pool->ref_count++;
	pthread_mutex_unlock(&pool->mutex);

	pf = alloc_and_prefault_frame(pool);
	if (!pf) {
		pthread_mutex_lock(&pool->mutex);
		if (pool->in_use_frames > 0)
			pool->in_use_frames--;
		pool->ref_count--;
		pthread_mutex_unlock(&pool->mutex);
		return NULL;
	}

	pthread_mutex_lock(&pool->mutex);
	pool->allocated_frames++;
	pool->allocated_bytes += pf->alloc_bytes;
	pthread_mutex_unlock(&pool->mutex);

	reset_frame_metadata(&pf->frame);
	return &pf->frame;
}

void replay_pool_release_frame(struct obs_source_frame *frame)
{
	if (!frame)
		return;

	struct replay_pool_frame *pf = (struct replay_pool_frame *)frame;
	if (pf->magic != REPLAY_POOL_FRAME_MAGIC || !pf->pool) {
		obs_source_frame_free(frame);
		bfree(frame);
		return;
	}

	struct replay_pool *pool = pf->pool;
	bool destroy_pool = false;

	pthread_mutex_lock(&pool->mutex);
	if (pool->in_use_frames > 0)
		pool->in_use_frames--;

	if (pool->closed) {
		if (pool->allocated_frames > 0)
			pool->allocated_frames--;
		if (pool->allocated_bytes >= pf->alloc_bytes)
			pool->allocated_bytes -= pf->alloc_bytes;
		else
			pool->allocated_bytes = 0;
		pool->ref_count--;
		destroy_pool = (pool->ref_count == 0);
		pthread_mutex_unlock(&pool->mutex);

		destroy_pool_frame(pf);
		if (destroy_pool) {
			pthread_mutex_destroy(&pool->mutex);
			bfree(pool);
		}
		return;
	}

	pf->next_free = pool->free_list;
	pool->free_list = pf;
	pool->ref_count--;
	pthread_mutex_unlock(&pool->mutex);
}

void replay_pool_get_stats(struct replay_pool *pool, struct replay_pool_stats *stats)
{
	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));
	if (!pool)
		return;

	pthread_mutex_lock(&pool->mutex);
	stats->allocated_frames = pool->allocated_frames;
	stats->in_use_frames = pool->in_use_frames;
	stats->peak_frames = pool->peak_frames;
	stats->allocated_bytes = pool->allocated_bytes;
	pthread_mutex_unlock(&pool->mutex);
}

enum video_format replay_pool_get_format(const struct replay_pool *pool)
{
	return pool ? pool->format : VIDEO_FORMAT_NONE;
}

uint32_t replay_pool_get_width(const struct replay_pool *pool)
{
	return pool ? pool->width : 0;
}

uint32_t replay_pool_get_height(const struct replay_pool *pool)
{
	return pool ? pool->height : 0;
}
