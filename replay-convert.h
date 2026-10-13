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

#ifdef __cplusplus
}
#endif
