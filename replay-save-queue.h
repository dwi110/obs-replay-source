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

/* PC-wide save-encoder slot shared by every OBS instance running this plugin: at most one replay save encodes at a
 * time on the PC. Crash-safe: the OS releases it when the owning process exits (Windows named mutex, POSIX flock). */
#define REPLAY_SAVE_SLOT_WIN_NAME L"Local\\ReplaySource.SaveEncoderSlot"
#define REPLAY_SAVE_SLOT_POSIX_PATH "/tmp/obs-replay-source-save-encoder.lock"

struct replay_source;
void replay_save(struct replay_source *context);
void replay_save_with_trigger(struct replay_source *context, const char *trigger);

struct replay_save_job_params {
	const char *source_name;
	const char *directory;
	const char *file_format;
	const char *save_encoder;
	const char *save_container;
	int save_quality;
	bool lossless;
	const char *trigger;
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
/* "idle", "waitingLive" (own OBS render/encode lagging), "waitingSlot" (other instance encoding) or "encoding". */
const char *replay_save_queue_state(void);

#ifdef __cplusplus
}
#endif
