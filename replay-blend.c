#include "replay-blend.h"
#include "replay.h"
#include "replay-convert.h"
#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>
#include <math.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define REPLAY_HAVE_SSE2 1
#else
#define REPLAY_HAVE_SSE2 0
#endif

#define REPLAY_BLEND_QUEUE_CAP 8

static inline uint8_t blend_scalar_byte(uint8_t a, uint8_t b, uint32_t w8)
{
	return (uint8_t)(((uint32_t)a * (256U - w8) + (uint32_t)b * w8 + 128U) >> 8);
}

void replay_blend_plane_scalar(uint8_t *dst, const uint8_t *src_a, const uint8_t *src_b,
			       size_t width, size_t height,
			       size_t dst_linesize, size_t a_linesize, size_t b_linesize,
			       uint32_t w8)
{
	if (w8 == 0) {
		for (size_t y = 0; y < height; y++) {
			memcpy(dst + y * dst_linesize, src_a + y * a_linesize, width);
		}
		return;
	}
	if (w8 == 256) {
		for (size_t y = 0; y < height; y++) {
			memcpy(dst + y * dst_linesize, src_b + y * b_linesize, width);
		}
		return;
	}

	for (size_t y = 0; y < height; y++) {
		const uint8_t *a = src_a + y * a_linesize;
		const uint8_t *b = src_b + y * b_linesize;
		uint8_t *d = dst + y * dst_linesize;
		for (size_t x = 0; x < width; x++) {
			d[x] = blend_scalar_byte(a[x], b[x], w8);
		}
	}
}

void replay_blend_plane_sse2(uint8_t *dst, const uint8_t *src_a, const uint8_t *src_b,
			     size_t width, size_t height,
			     size_t dst_linesize, size_t a_linesize, size_t b_linesize,
			     uint32_t w8)
{
#if REPLAY_HAVE_SSE2
	if (w8 == 0) {
		for (size_t y = 0; y < height; y++) {
			memcpy(dst + y * dst_linesize, src_a + y * a_linesize, width);
		}
		return;
	}
	if (w8 == 256) {
		for (size_t y = 0; y < height; y++) {
			memcpy(dst + y * dst_linesize, src_b + y * b_linesize, width);
		}
		return;
	}

	const __m128i zero = _mm_setzero_si128();
	const __m128i vw_a = _mm_set1_epi16((short)(256U - w8));
	const __m128i vw_b = _mm_set1_epi16((short)w8);
	const __m128i v128 = _mm_set1_epi16(128);

	for (size_t y = 0; y < height; y++) {
		const uint8_t *a = src_a + y * a_linesize;
		const uint8_t *b = src_b + y * b_linesize;
		uint8_t *d = dst + y * dst_linesize;
		size_t x = 0;

		for (; x + 32 <= width; x += 32) {
			__m128i va0 = _mm_loadu_si128((const __m128i *)(a + x));
			__m128i vb0 = _mm_loadu_si128((const __m128i *)(b + x));
			__m128i va1 = _mm_loadu_si128((const __m128i *)(a + x + 16));
			__m128i vb1 = _mm_loadu_si128((const __m128i *)(b + x + 16));

			__m128i a_lo0 = _mm_unpacklo_epi8(va0, zero);
			__m128i a_hi0 = _mm_unpackhi_epi8(va0, zero);
			__m128i b_lo0 = _mm_unpacklo_epi8(vb0, zero);
			__m128i b_hi0 = _mm_unpackhi_epi8(vb0, zero);

			__m128i a_lo1 = _mm_unpacklo_epi8(va1, zero);
			__m128i a_hi1 = _mm_unpackhi_epi8(va1, zero);
			__m128i b_lo1 = _mm_unpacklo_epi8(vb1, zero);
			__m128i b_hi1 = _mm_unpackhi_epi8(vb1, zero);

			__m128i prod_a_lo0 = _mm_mullo_epi16(a_lo0, vw_a);
			__m128i prod_a_hi0 = _mm_mullo_epi16(a_hi0, vw_a);
			__m128i prod_b_lo0 = _mm_mullo_epi16(b_lo0, vw_b);
			__m128i prod_b_hi0 = _mm_mullo_epi16(b_hi0, vw_b);

			__m128i prod_a_lo1 = _mm_mullo_epi16(a_lo1, vw_a);
			__m128i prod_a_hi1 = _mm_mullo_epi16(a_hi1, vw_a);
			__m128i prod_b_lo1 = _mm_mullo_epi16(b_lo1, vw_b);
			__m128i prod_b_hi1 = _mm_mullo_epi16(b_hi1, vw_b);

			__m128i sum_lo0 = _mm_add_epi16(_mm_add_epi16(prod_a_lo0, prod_b_lo0), v128);
			__m128i sum_hi0 = _mm_add_epi16(_mm_add_epi16(prod_a_hi0, prod_b_hi0), v128);
			__m128i sum_lo1 = _mm_add_epi16(_mm_add_epi16(prod_a_lo1, prod_b_lo1), v128);
			__m128i sum_hi1 = _mm_add_epi16(_mm_add_epi16(prod_a_hi1, prod_b_hi1), v128);

			__m128i res_lo0 = _mm_srli_epi16(sum_lo0, 8);
			__m128i res_hi0 = _mm_srli_epi16(sum_hi0, 8);
			__m128i res_lo1 = _mm_srli_epi16(sum_lo1, 8);
			__m128i res_hi1 = _mm_srli_epi16(sum_hi1, 8);

			_mm_storeu_si128((__m128i *)(d + x), _mm_packus_epi16(res_lo0, res_hi0));
			_mm_storeu_si128((__m128i *)(d + x + 16), _mm_packus_epi16(res_lo1, res_hi1));
		}

		for (; x + 16 <= width; x += 16) {
			__m128i va0 = _mm_loadu_si128((const __m128i *)(a + x));
			__m128i vb0 = _mm_loadu_si128((const __m128i *)(b + x));

			__m128i a_lo0 = _mm_unpacklo_epi8(va0, zero);
			__m128i a_hi0 = _mm_unpackhi_epi8(va0, zero);
			__m128i b_lo0 = _mm_unpacklo_epi8(vb0, zero);
			__m128i b_hi0 = _mm_unpackhi_epi8(vb0, zero);

			__m128i prod_a_lo0 = _mm_mullo_epi16(a_lo0, vw_a);
			__m128i prod_a_hi0 = _mm_mullo_epi16(a_hi0, vw_a);
			__m128i prod_b_lo0 = _mm_mullo_epi16(b_lo0, vw_b);
			__m128i prod_b_hi0 = _mm_mullo_epi16(b_hi0, vw_b);

			__m128i sum_lo0 = _mm_add_epi16(_mm_add_epi16(prod_a_lo0, prod_b_lo0), v128);
			__m128i sum_hi0 = _mm_add_epi16(_mm_add_epi16(prod_a_hi0, prod_b_hi0), v128);

			__m128i res_lo0 = _mm_srli_epi16(sum_lo0, 8);
			__m128i res_hi0 = _mm_srli_epi16(sum_hi0, 8);

			_mm_storeu_si128((__m128i *)(d + x), _mm_packus_epi16(res_lo0, res_hi0));
		}

		for (; x < width; x++) {
			d[x] = blend_scalar_byte(a[x], b[x], w8);
		}
	}
#else
	replay_blend_plane_scalar(dst, src_a, src_b, width, height, dst_linesize, a_linesize, b_linesize, w8);
#endif
}

bool replay_blend_frames(struct obs_source_frame *dst,
			 const struct obs_source_frame *src_a,
			 const struct obs_source_frame *src_b,
			 uint32_t weight_b_256)
{
	if (!dst || !src_a || !src_b)
		return false;
	if (src_a->format != src_b->format || src_a->width != src_b->width || src_a->height != src_b->height)
		return false;

	uint32_t w8 = weight_b_256;
	if (w8 > 256)
		w8 = 256;

	const uint32_t width = src_a->width;
	const uint32_t height = src_a->height;

	switch (src_a->format) {
	case VIDEO_FORMAT_NV12: {
		/* Plane 0: Y */
		replay_blend_plane_sse2(dst->data[0], src_a->data[0], src_b->data[0],
					width, height,
					dst->linesize[0], src_a->linesize[0], src_b->linesize[0], w8);
		/* Plane 1: UV interleaved */
		const uint32_t uv_height = (height + 1) / 2;
		replay_blend_plane_sse2(dst->data[1], src_a->data[1], src_b->data[1],
					width, uv_height,
					dst->linesize[1], src_a->linesize[1], src_b->linesize[1], w8);
		return true;
	}
	case VIDEO_FORMAT_UYVY: {
		/* Plane 0: UYVY packed 4:2:2 (2 bytes per pixel) */
		replay_blend_plane_sse2(dst->data[0], src_a->data[0], src_b->data[0],
					(size_t)width * 2, height,
					dst->linesize[0], src_a->linesize[0], src_b->linesize[0], w8);
		return true;
	}
	case VIDEO_FORMAT_I420: {
		/* Plane 0: Y */
		replay_blend_plane_sse2(dst->data[0], src_a->data[0], src_b->data[0],
					width, height,
					dst->linesize[0], src_a->linesize[0], src_b->linesize[0], w8);
		/* Plane 1: U */
		const uint32_t uv_width = (width + 1) / 2;
		const uint32_t uv_height = (height + 1) / 2;
		replay_blend_plane_sse2(dst->data[1], src_a->data[1], src_b->data[1],
					uv_width, uv_height,
					dst->linesize[1], src_a->linesize[1], src_b->linesize[1], w8);
		/* Plane 2: V */
		replay_blend_plane_sse2(dst->data[2], src_a->data[2], src_b->data[2],
					uv_width, uv_height,
					dst->linesize[2], src_a->linesize[2], src_b->linesize[2], w8);
		return true;
	}
	default:
		return false;
	}
}

size_t replay_blend_compute_schedule(const uint64_t *source_timestamps, size_t num_source_frames,
				    float speed_percent, bool backward, uint64_t output_interval_ns,
				    uint64_t start_timestamp,
				    struct replay_blend_schedule_item *out_items, size_t max_items)
{
	if (!source_timestamps || num_source_frames == 0 || !out_items || max_items == 0 ||
	    output_interval_ns == 0 || speed_percent <= 0.0f)
		return 0;

	if (num_source_frames == 1) {
		out_items[0].output_timestamp = start_timestamp;
		out_items[0].frame_a_index = 0;
		out_items[0].frame_b_index = 0;
		out_items[0].weight_b_256 = 0;
		return 1;
	}

	const uint64_t first_ts = source_timestamps[0];
	const uint64_t last_ts = source_timestamps[num_source_frames - 1];

	size_t count = 0;
	size_t cur_pos = 0;

	/* Calculate the total duration in output time */
	uint64_t total_out_dur;
	if (backward) {
		uint64_t rel = last_ts - first_ts;
		total_out_dur = (uint64_t)((double)rel * 100.0 / (double)speed_percent);
	} else {
		uint64_t rel = last_ts - first_ts;
		total_out_dur = (uint64_t)((double)rel * 100.0 / (double)speed_percent);
	}
	const uint64_t max_out_time = start_timestamp + total_out_dur;

	for (uint64_t k = 0; count < max_items; k++) {
		uint64_t t = start_timestamp + k * output_interval_ns;

		/* Advance playback position until the next frame's start time is > t */
		while (cur_pos + 1 < num_source_frames) {
			size_t next_idx = backward ? (num_source_frames - 1 - (cur_pos + 1)) : (cur_pos + 1);
			uint64_t next_src_ts = source_timestamps[next_idx];
			uint64_t next_rel = backward ? (last_ts - next_src_ts) : (next_src_ts - first_ts);
			uint64_t next_t_out = start_timestamp + (uint64_t)((double)next_rel * 100.0 / (double)speed_percent);

			if (t < next_t_out)
				break;
			cur_pos++;
		}

		size_t idx_a = backward ? (num_source_frames - 1 - cur_pos) : cur_pos;
		uint64_t src_a_ts = source_timestamps[idx_a];
		uint64_t rel_a = backward ? (last_ts - src_a_ts) : (src_a_ts - first_ts);
		uint64_t t_a = start_timestamp + (uint64_t)((double)rel_a * 100.0 / (double)speed_percent);

		size_t idx_b = idx_a;
		uint32_t w8 = 0;

		if (cur_pos + 1 < num_source_frames) {
			idx_b = backward ? (num_source_frames - 1 - (cur_pos + 1)) : (cur_pos + 1);
			uint64_t src_b_ts = source_timestamps[idx_b];
			uint64_t rel_b = backward ? (last_ts - src_b_ts) : (src_b_ts - first_ts);
			uint64_t t_b = start_timestamp + (uint64_t)((double)rel_b * 100.0 / (double)speed_percent);

			if (t_b > t_a && t >= t_a) {
				double w = (double)(t - t_a) / (double)(t_b - t_a);
				if (w < 0.0)
					w = 0.0;
				if (w > 1.0)
					w = 1.0;
				w8 = (uint32_t)llround(w * 256.0);
				if (w8 > 256)
					w8 = 256;
			}
		}

		out_items[count].output_timestamp = t;
		out_items[count].frame_a_index = idx_a;
		out_items[count].frame_b_index = idx_b;
		out_items[count].weight_b_256 = w8;
		count++;

		if (t >= max_out_time)
			break;
	}

	return count;
}

/* Blend worker thread structures and functions */

struct replay_blend_item {
	struct obs_source_frame *frame_a;
	struct obs_source_frame *frame_b;
	uint32_t weight_b_256;
	uint64_t output_timestamp;
	bool half;
};

struct replay_blend_worker {
	obs_source_t *source;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond_work;
	pthread_cond_t cond_done;

	bool thread_active;
	bool stop;
	bool busy;

	struct replay_blend_item queue[REPLAY_BLEND_QUEUE_CAP];
	size_t q_head;
	size_t q_tail;
	size_t q_count;

	uint64_t last_output_timestamp;
	struct obs_source_frame *scratch_frame;
	struct obs_source_frame *half_frame;

	replay_blend_output_cb output_hook;
	void *output_hook_param;
};

/* Hands one frame to OBS; half-resolution items are converted first (preview outside program). */
static void replay_blend_emit(struct replay_blend_worker *worker, const struct obs_source_frame *src, uint64_t ts,
			      bool half)
{
	struct obs_source_frame out = *src;
	const struct obs_source_frame *emit = &out;
	out.timestamp = ts;
	if (half && (!worker->source || !obs_source_active(worker->source)) &&
	    replay_convert_half_preview(&worker->half_frame, src)) {
		worker->half_frame->timestamp = ts;
		emit = worker->half_frame;
		/* Same-size async output does not replace a paused texture: OBS keeps the previous
		 * upload until a later tick selects this frame. Set it now so a clip change is visible. */
		if (worker->source)
			obs_source_set_video_frame(worker->source, emit);
	}
	if (worker->output_hook)
		worker->output_hook(worker->output_hook_param, worker->source, emit);
	if (worker->source)
		obs_source_output_video(worker->source, emit);
}

static void replay_blend_process_item(struct replay_blend_worker *worker,
				      const struct replay_blend_item *item)
{
	struct obs_source_frame *fa = item->frame_a;
	struct obs_source_frame *fb = item->frame_b;
	uint32_t w8 = item->weight_b_256;
	uint64_t ts = item->output_timestamp;

	if (ts < worker->last_output_timestamp)
		return;
	worker->last_output_timestamp = ts;

	bool can_blend = fa && fb && (fa->format == fb->format) &&
			 (fa->width == fb->width) && (fa->height == fb->height) &&
			 (fa->format == VIDEO_FORMAT_NV12 || fa->format == VIDEO_FORMAT_UYVY ||
			  fa->format == VIDEO_FORMAT_I420);

	if (w8 == 0 || !can_blend) {
		replay_blend_emit(worker, fa, ts, item->half);
		return;
	}

	if (w8 == 256) {
		replay_blend_emit(worker, fb, ts, item->half);
		return;
	}

	if (!worker->scratch_frame || worker->scratch_frame->format != fa->format ||
	    worker->scratch_frame->width != fa->width || worker->scratch_frame->height != fa->height) {
		if (worker->scratch_frame) {
			obs_source_frame_destroy(worker->scratch_frame);
			worker->scratch_frame = NULL;
		}
		worker->scratch_frame = obs_source_frame_create(fa->format, fa->width, fa->height);
	}

	if (!worker->scratch_frame) {
		replay_blend_emit(worker, fa, ts, item->half);
		return;
	}

	replay_copy_frame_metadata(worker->scratch_frame, fa);
	worker->scratch_frame->timestamp = ts;

	replay_blend_frames(worker->scratch_frame, fa, fb, w8);
	replay_blend_emit(worker, worker->scratch_frame, ts, item->half);
}

static void *replay_blend_worker_thread(void *arg)
{
	struct replay_blend_worker *worker = arg;
	while (true) {
		struct replay_blend_item item;
		pthread_mutex_lock(&worker->mutex);
		while (!worker->stop && worker->q_count == 0) {
			worker->busy = false;
			pthread_cond_broadcast(&worker->cond_done);
			pthread_cond_wait(&worker->cond_work, &worker->mutex);
		}
		if (worker->stop && worker->q_count == 0) {
			worker->busy = false;
			pthread_cond_broadcast(&worker->cond_done);
			pthread_mutex_unlock(&worker->mutex);
			break;
		}

		item = worker->queue[worker->q_head];
		worker->q_head = (worker->q_head + 1) % REPLAY_BLEND_QUEUE_CAP;
		worker->q_count--;
		worker->busy = true;
		pthread_mutex_unlock(&worker->mutex);

		replay_blend_process_item(worker, &item);

		if (item.frame_a)
			replay_frame_release(item.frame_a);
		if (item.frame_b && item.frame_b != item.frame_a)
			replay_frame_release(item.frame_b);
	}
	return NULL;
}

struct replay_blend_worker *replay_blend_worker_create(obs_source_t *source)
{
	struct replay_blend_worker *worker = bzalloc(sizeof(*worker));
	if (!worker)
		return NULL;

	worker->source = source;
	pthread_mutex_init(&worker->mutex, NULL);
	pthread_cond_init(&worker->cond_work, NULL);
	pthread_cond_init(&worker->cond_done, NULL);

	if (pthread_create(&worker->thread, NULL, replay_blend_worker_thread, worker) != 0) {
		pthread_mutex_destroy(&worker->mutex);
		pthread_cond_destroy(&worker->cond_work);
		pthread_cond_destroy(&worker->cond_done);
		bfree(worker);
		return NULL;
	}
	worker->thread_active = true;
	return worker;
}

void replay_blend_worker_destroy(struct replay_blend_worker *worker)
{
	if (!worker)
		return;

	pthread_mutex_lock(&worker->mutex);
	worker->stop = true;
	pthread_cond_signal(&worker->cond_work);
	pthread_mutex_unlock(&worker->mutex);

	if (worker->thread_active) {
		pthread_join(worker->thread, NULL);
		worker->thread_active = false;
	}

	for (size_t i = 0; i < worker->q_count; i++) {
		size_t idx = (worker->q_head + i) % REPLAY_BLEND_QUEUE_CAP;
		if (worker->queue[idx].frame_a)
			replay_frame_release(worker->queue[idx].frame_a);
		if (worker->queue[idx].frame_b && worker->queue[idx].frame_b != worker->queue[idx].frame_a)
			replay_frame_release(worker->queue[idx].frame_b);
	}

	if (worker->scratch_frame) {
		obs_source_frame_destroy(worker->scratch_frame);
		worker->scratch_frame = NULL;
	}
	replay_half_preview_free(&worker->half_frame);

	pthread_mutex_destroy(&worker->mutex);
	pthread_cond_destroy(&worker->cond_work);
	pthread_cond_destroy(&worker->cond_done);
	bfree(worker);
}

bool replay_blend_worker_push(struct replay_blend_worker *worker,
			      struct obs_source_frame *frame_a,
			      struct obs_source_frame *frame_b,
			      uint32_t weight_b_256,
			      uint64_t output_timestamp,
			      bool half)
{
	if (!worker || !frame_a)
		return false;

	pthread_mutex_lock(&worker->mutex);

	if (worker->stop) {
		pthread_mutex_unlock(&worker->mutex);
		return false;
	}

	if (worker->q_count >= REPLAY_BLEND_QUEUE_CAP) {
		/* Queue is full. If intermediate blended frame, skip it without blocking */
		if (weight_b_256 > 0) {
			pthread_mutex_unlock(&worker->mutex);
			return false;
		}

		/* If key frame (weight 0), drop oldest intermediate frame in queue to make room */
		bool dropped_intermediate = false;
		for (size_t i = 0; i < worker->q_count; i++) {
			size_t idx = (worker->q_head + i) % REPLAY_BLEND_QUEUE_CAP;
			if (worker->queue[idx].weight_b_256 > 0) {
				if (worker->queue[idx].frame_a)
					replay_frame_release(worker->queue[idx].frame_a);
				if (worker->queue[idx].frame_b && worker->queue[idx].frame_b != worker->queue[idx].frame_a)
					replay_frame_release(worker->queue[idx].frame_b);

				for (size_t j = i; j + 1 < worker->q_count; j++) {
					size_t cur = (worker->q_head + j) % REPLAY_BLEND_QUEUE_CAP;
					size_t nxt = (worker->q_head + j + 1) % REPLAY_BLEND_QUEUE_CAP;
					worker->queue[cur] = worker->queue[nxt];
				}
				worker->q_count--;
				worker->q_tail = (worker->q_head + worker->q_count) % REPLAY_BLEND_QUEUE_CAP;
				dropped_intermediate = true;
				break;
			}
		}

		if (!dropped_intermediate) {
			/* All are weight 0: drop oldest */
			size_t idx = worker->q_head;
			if (worker->queue[idx].frame_a)
				replay_frame_release(worker->queue[idx].frame_a);
			if (worker->queue[idx].frame_b && worker->queue[idx].frame_b != worker->queue[idx].frame_a)
				replay_frame_release(worker->queue[idx].frame_b);
			worker->q_head = (worker->q_head + 1) % REPLAY_BLEND_QUEUE_CAP;
			worker->q_count--;
		}
	}

	size_t slot = worker->q_tail;
	replay_frame_addref(frame_a);
	worker->queue[slot].frame_a = frame_a;

	if (frame_b && frame_b != frame_a) {
		replay_frame_addref(frame_b);
		worker->queue[slot].frame_b = frame_b;
	} else {
		worker->queue[slot].frame_b = frame_a;
	}

	worker->queue[slot].weight_b_256 = weight_b_256;
	worker->queue[slot].output_timestamp = output_timestamp;
	worker->queue[slot].half = half;

	worker->q_tail = (worker->q_tail + 1) % REPLAY_BLEND_QUEUE_CAP;
	worker->q_count++;

	pthread_cond_signal(&worker->cond_work);
	pthread_mutex_unlock(&worker->mutex);
	return true;
}

void replay_blend_worker_flush(struct replay_blend_worker *worker)
{
	if (!worker)
		return;

	pthread_mutex_lock(&worker->mutex);

	while (worker->q_count > 0) {
		struct replay_blend_item *item = &worker->queue[worker->q_head];
		if (item->frame_a)
			replay_frame_release(item->frame_a);
		if (item->frame_b && item->frame_b != item->frame_a)
			replay_frame_release(item->frame_b);
		worker->q_head = (worker->q_head + 1) % REPLAY_BLEND_QUEUE_CAP;
		worker->q_count--;
	}
	worker->q_head = 0;
	worker->q_tail = 0;

	while (worker->busy) {
		pthread_cond_wait(&worker->cond_done, &worker->mutex);
	}

	worker->last_output_timestamp = 0;
	pthread_mutex_unlock(&worker->mutex);
}

void replay_blend_worker_set_output_hook(struct replay_blend_worker *worker,
					 replay_blend_output_cb cb, void *param)
{
	if (!worker)
		return;
	pthread_mutex_lock(&worker->mutex);
	worker->output_hook = cb;
	worker->output_hook_param = param;
	pthread_mutex_unlock(&worker->mutex);
}
