#pragma once

#include <obs.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void replay_convert_target_format(enum video_format src_format, uint32_t src_width, uint32_t src_height,
				  int storage_format, bool downscale_max_1080p, enum video_format *dst_format,
				  uint32_t *dst_width, uint32_t *dst_height);

void replay_copy_frame_metadata(struct obs_source_frame *dst, const struct obs_source_frame *src);

bool replay_convert_frame(struct obs_source_frame *dst, const struct obs_source_frame *src);
bool replay_convert_frame_scalar(struct obs_source_frame *dst, const struct obs_source_frame *src);
bool replay_convert_frame_simd(struct obs_source_frame *dst, const struct obs_source_frame *src);

/* Half-resolution preview of a replay frame (Multiview/studio preview, not program). Only sources at least this wide
 * are halved; the result is NV12 with half width and height. */
#define REPLAY_HALF_PREVIEW_MIN_WIDTH 1280
bool replay_half_preview_supported(const struct obs_source_frame *src);
/* Converts src into *scratch, (re)allocating it as needed. Returns false if src is unsupported. */
bool replay_convert_half_preview(struct obs_source_frame **scratch, const struct obs_source_frame *src);
void replay_half_preview_free(struct obs_source_frame **scratch);

#ifdef __cplusplus
}
#endif
