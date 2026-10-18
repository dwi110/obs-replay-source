#pragma once

#include <obs-module.h>
#include <media-io/audio-io.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Paced save: encodes and muxes a clip directly with libavcodec/libavformat instead of OBS outputs, at real time or
 * faster. OBS outputs take audio from an audio_t whose thread runs on the wall clock, so they can neither run faster
 * than real time nor pause for the live output without breaking audio/video alignment. */

struct replay_save_fast_input {
	const char *output_path;
	const char *save_container; /* SAVE_CONTAINER_*; lossless always writes AVI */
	const char *save_encoder;   /* SAVE_ENCODER_* */
	int save_quality;
	bool lossless;
	bool nvenc_present;
	/* Upper bound as a multiple of real time (1 = real time); 0 = as fast as the hardware allows. */
	int max_speed;

	struct obs_source_frame *const *video_frames;
	uint64_t video_frame_count;
	const struct obs_audio_data *audio_frames;
	uint64_t audio_frame_count;
	struct audio_convert_info oai;
	uint64_t first_frame_timestamp;
	uint64_t duration_ns;

	bool (*should_abort)(void);
	/* Monotonic count of lost program frames; any increase drops to real-time pace for 2 s. */
	uint64_t (*live_lag_counter)(void);
	/* Current camera delivery lateness (replay_filter_live_snapshot). Above 150 ms the save pauses until it is
	 * below 50 ms again and then runs at real-time pace for 2 s; pauses end after 60 s in total per clip, after
	 * which the clip finishes at real-time pace. */
	uint64_t (*live_late_ns)(void);
};

enum replay_save_fast_result {
	REPLAY_SAVE_FAST_OK,
	/* No FFmpeg libraries or no encoder could be opened; nothing was written. */
	REPLAY_SAVE_FAST_UNAVAILABLE,
	REPLAY_SAVE_FAST_FAILED,
	REPLAY_SAVE_FAST_ABORTED,
};

struct replay_save_fast_stats {
	const char *codec;
	uint64_t encode_ns;
	uint32_t slowdowns;
	uint32_t live_pauses;
	uint64_t live_paused_ns;
	uint64_t live_late_max_ns;
};

/* Compiled with FFmpeg support (property list offers faster speeds). */
bool replay_save_fast_supported(void);
/* FFmpeg libraries matching the build are loadable in this process. */
bool replay_save_fast_available(void);
enum replay_save_fast_result replay_save_fast_run(const struct replay_save_fast_input *in,
						 struct replay_save_fast_stats *stats);

#ifdef __cplusplus
}
#endif
