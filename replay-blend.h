#pragma once
#include <obs.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct replay_blend_worker;

/* Callback hook for tests or inspection of blended output frames */
typedef void (*replay_blend_output_cb)(void *param, obs_source_t *source, const struct obs_source_frame *frame);

/* Pure function item for scheduling */
struct replay_blend_schedule_item {
	uint64_t output_timestamp;
	size_t frame_a_index;
	size_t frame_b_index;
	uint32_t weight_b_256; /* [0, 256] */
};

/* Computes a schedule of output frames for an interval or sequence of source frame timestamps.
 * Returns the number of items written to out_items.
 * out_items must hold up to max_items. */
size_t replay_blend_compute_schedule(const uint64_t *source_timestamps, size_t num_source_frames,
				    float speed_percent, bool backward, uint64_t output_interval_ns,
				    uint64_t start_timestamp,
				    struct replay_blend_schedule_item *out_items, size_t max_items);

/* Per-plane blending: scalar fallback */
void replay_blend_plane_scalar(uint8_t *dst, const uint8_t *src_a, const uint8_t *src_b,
			       size_t width, size_t height,
			       size_t dst_linesize, size_t a_linesize, size_t b_linesize,
			       uint32_t w8);

/* Per-plane blending: SSE2 implementation (bit-identical to scalar) */
void replay_blend_plane_sse2(uint8_t *dst, const uint8_t *src_a, const uint8_t *src_b,
			     size_t width, size_t height,
			     size_t dst_linesize, size_t a_linesize, size_t b_linesize,
			     uint32_t w8);

/* Blend two frames (NV12, UYVY, or I420). w8 is in [0, 256].
 * If frames cannot be blended (format mismatch, dimensions mismatch, or unsupported format),
 * returns false without modifying dst. */
bool replay_blend_frames(struct obs_source_frame *dst,
			 const struct obs_source_frame *src_a,
			 const struct obs_source_frame *src_b,
			 uint32_t weight_b_256);

/* Blend worker thread API */
struct replay_blend_worker *replay_blend_worker_create(obs_source_t *source);
void replay_blend_worker_destroy(struct replay_blend_worker *worker);
/* half: emit at half resolution (replay_convert_half_preview) for previews outside program. */
bool replay_blend_worker_push(struct replay_blend_worker *worker,
			      struct obs_source_frame *frame_a,
			      struct obs_source_frame *frame_b,
			      uint32_t weight_b_256,
			      uint64_t output_timestamp,
			      bool half);
void replay_blend_worker_flush(struct replay_blend_worker *worker);
void replay_blend_worker_set_output_hook(struct replay_blend_worker *worker,
					 replay_blend_output_cb cb, void *param);
void replay_source_set_blend_output_hook(obs_source_t *source,
					 replay_blend_output_cb cb, void *param);

#ifdef __cplusplus
}
#endif
