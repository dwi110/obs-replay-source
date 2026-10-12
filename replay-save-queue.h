#pragma once

#include <obs-module.h>
#include "replay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTING_SAVE_ENCODER "save_encoder"
#define SETTING_SAVE_CONTAINER "save_container"

#define SAVE_ENCODER_AUTO "auto"
#define SAVE_ENCODER_HEVC_NVENC "hevc_nvenc"
#define SAVE_ENCODER_H264_NVENC "h264_nvenc"
#define SAVE_ENCODER_X264 "x264"

#define SAVE_CONTAINER_MP4 "mp4"
#define SAVE_CONTAINER_MKV "mkv"

struct replay_source;
void replay_save(struct replay_source *context);

struct replay_save_job_params {
	const char *source_name;
	const char *directory;
	const char *file_format;
	const char *save_encoder;
	const char *save_container;
	bool lossless;

	struct obs_source_frame **video_frames;
	uint64_t video_frame_count;

	const struct obs_audio_data *audio_frames;
	uint64_t audio_frame_count;
	struct audio_convert_info oai;

	uint64_t first_frame_timestamp;
	uint64_t last_frame_timestamp;
	uint64_t duration;
	int64_t trim_front;
	int64_t trim_end;
};

void replay_save_queue_init(void);
void replay_save_queue_shutdown(void);

bool replay_save_queue_enqueue(const struct replay_save_job_params *params);
size_t replay_save_all_loaded_replays(void);

size_t replay_save_queue_pending_count(void);
size_t replay_save_queue_pinned_bytes(void);
uint64_t replay_save_queue_completed_count(void);
long replay_save_queue_max_concurrent_encoders(void);
bool replay_save_queue_wait_idle(uint32_t timeout_ms);
const char *replay_save_select_encoder(const char *save_encoder);

#ifdef __cplusplus
}
#endif
