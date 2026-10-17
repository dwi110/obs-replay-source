#pragma once
#include <obs-module.h>
#include <util/threading.h>

#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
#include <util/deque.h>
#define circlebuf_peek_front deque_peek_front
#define circlebuf_peek_back deque_peek_back
#define circlebuf_push_front deque_push_front
#define circlebuf_push_back deque_push_back
#define circlebuf_pop_front deque_pop_front
#define circlebuf_pop_back deque_pop_back
#define circlebuf_init deque_init
#define circlebuf_free deque_free
#define circlebuf_data deque_data
#else
#include <util/circlebuf.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct replay_pool;

struct replay_filter {

#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	/* contains struct obs_source_frame* */
	struct deque video_frames;

	/* stores the audio data */
	struct deque audio_frames;
#else
	/* contains struct obs_source_frame* */
	struct circlebuf video_frames;

	/* stores the audio data */
	struct circlebuf audio_frames;
#endif

	struct obs_video_info ovi;
	struct audio_convert_info oai;

	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;

	uint32_t known_width;
	uint32_t known_height;

	uint8_t *video_data;
	uint32_t video_linesize;
	video_t *video_output;

	uint64_t duration;
	obs_source_t *src;
	pthread_mutex_t mutex;
	int64_t timing_adjust;
	bool internal_frames;
	float threshold;
	void (*trigger_threshold)(void *data);
	void *threshold_data;
	uint64_t last_check;
	size_t target_offset;

	struct replay_pool *pool;
	int storage_format;
	int downscale;
	int reserve_mode;
	int max_replays;
	int logged_reserve_mode;
	uint32_t logged_reserve_target;
	obs_weak_source_t *tap_parent;
	volatile bool tap_active;
	bool tap_checked;
	/* Serializes tap attach (graphics thread) with detach (filter removal/destroy). After removal the
	 * filter never attaches again, so no tap callback can outlive the filter. */
	pthread_mutex_t tap_mutex;
	bool tap_closed;

	uint32_t capture_count;
	uint64_t capture_sum_ns;
	uint64_t capture_max_ns;
	uint64_t capture_log_ts;
	/* os_gettime_ns() when the newest frame or audio packet reached the filter. Unlike the stored
	 * timestamps (the source's clock, only re-based beyond MAX_TS_VAR) this is the same clock for every
	 * camera, so LoadAll can tell a stalled angle from one whose clock runs ahead or behind. */
	uint64_t last_arrival_ns;
	/* Range of (stored timestamp - arrival) since the last capture log line. */
	int64_t ts_arrival_min_ns;
	int64_t ts_arrival_max_ns;
};

/* Audio packet ownership: every struct obs_audio_data stored by a replay filter, a loaded replay or a
 * pending save points into one refcounted block holding all planes. replay_audio_packet_copy creates a
 * block with one reference, replay_audio_packet_addref shares it (loading a replay copies no samples) and
 * free_audio_packet drops one reference. Never bfree the plane pointers directly. */
void replay_audio_packet_copy(struct obs_audio_data *dst, const struct obs_audio_data *src);
void replay_audio_packet_addref(struct obs_audio_data *audio);
void free_audio_packet(struct obs_audio_data *audio);
struct obs_audio_data *replay_filter_audio(void *data, struct obs_audio_data *audio);
void free_video_data(struct replay_filter *filter);
void free_audio_data(struct replay_filter *filter);

/* Frame ownership: every struct obs_source_frame stored by a replay filter, a loaded
 * replay or a pending save comes from replay_pool_acquire and is referenced through these
 * helpers only. A stored frame starts with refs == 1; each additional holder calls
 * replay_frame_addref and every holder drops its reference with replay_frame_release. */
void replay_frame_addref(struct obs_source_frame *frame);
void replay_frame_release(struct obs_source_frame *frame);
void replay_trigger_threshold(void *data);
void replay_filter_check(void *data);

#define REPLAY_FILTER_ID "replay_filter"
#define REPLAY_FILTER_AUDIO_ID "replay_filter_audio"
//#define TEXT_FILTER_AUDIO_NAME "Replay filter audio"
#define REPLAY_FILTER_ASYNC_ID "replay_filter_async"
//#define TEXT_FILTER_ASYNC_NAME "Replay filter async"
#define REPLAY_SOURCE_ID "replay_source"
#define SETTING_DURATION "duration"
#define SETTING_DURATION_MIN 1
#define SETTING_DURATION_MAX 200000
//#define TEXT_DURATION "Duration (ms)"
#define SETTING_RETRIEVE_DELAY "retrieve_delay"
//#define TEXT_RETRIEVE_DELAY "Load delay (ms)"
#define SETTING_REPLAYS "replays"
//#define TEXT_REPLAYS "Maximum replays"
#define SETTING_SPEED "speed_percent"
#define SETTING_SPEED_MIN 0.01f
#define SETTING_SPEED_MAX 400.0f
#define SETTING_BACKWARD "backward"
#define SETTING_SLOWMO_BLEND "slowmo_blend"
#define SLOWMO_BLEND_OFF 0
/* Crossfade only while the replay source is active (program output, incl. transitions). */
#define SLOWMO_BLEND_PROGRAM 1
/* Crossfade wherever the replay plays, also Multiview/preview: one CPU blend per source and output frame. */
#define SLOWMO_BLEND_ALWAYS 2
/* Outside program (Multiview, studio preview, projectors) emit sources >= 1280 px wide at half resolution: OBS copies
 * and uploads every async frame in its graphics thread, which a full-size replay per Multiview tile pays for. */
#define SETTING_PREVIEW_HALF "preview_half"
#define SETTING_VISIBILITY_ACTION "visibility_action"
#define SETTING_START_DELAY "start_delay"
#define SETTING_FRAME_STEP_COUNT "frame_step_count"
//#define TEXT_START_DELAY "Start delay (ms)"
#define SETTING_END_ACTION "end_action"
#define SETTING_SOURCE "source"
//#define TEXT_SOURCE "Video source"
#define SETTING_SOURCE_AUDIO "source_audio"
//#define TEXT_SOURCE_AUDIO "Audio source"
#define SETTING_NEXT_SCENE "next_scene"
//#define TEXT_NEXT_SCENE "Next scene"
#define SETTING_DIRECTORY "directory"
#define SETTING_FILE_FORMAT "file_format"
#define SETTING_LOSSLESS "lossless"
#define SETTING_PROGRESS_SOURCE "progress_source"
#define SETTING_TEXT_SOURCE "text_source"
#define SETTING_TEXT "text"
#define SETTING_INTERNAL_FRAMES "internal_frames"
#define SETTING_SOUND_TRIGGER "sound_trigger"
#define SETTING_AUDIO_THRESHOLD "threshold"
#define SETTING_AUDIO_THRESHOLD_MIN -60.0
#define SETTING_AUDIO_THRESHOLD_MAX 0.0f
#define SETTING_LOAD_SWITCH_SCENE "load_switch_scene"
#define SETTING_EXECUTE_ACTION "execute_action"
#define SETTING_STORAGE_FORMAT "storage_format"
#define STORAGE_FORMAT_NV12 0
#define STORAGE_FORMAT_ORIGINAL 1
#define STORAGE_FORMAT_UYVY 2
#define SETTING_DOWNSCALE "downscale"
#define DOWNSCALE_NONE 0
#define DOWNSCALE_MAX_1080P 1
#define SETTING_KEEP_BUFFER "keep_buffer"
#define SETTING_AUTO_RELEASE "auto_release"
#define AUTO_RELEASE_NEVER 0
#define AUTO_RELEASE_DEACTIVATE_HIDE 1
#define SETTING_RESERVE_MODE "reserve_mode"
#define RESERVE_BUFFER 0
#define RESERVE_BUFFER_PLUS_ONE 1
#define RESERVE_BUFFER_PLUS_MAX 2
#define SETTING_SAVE_QUALITY "save_quality"
#define SETTING_SAVE_QUALITY_MIN 10
#define SETTING_SAVE_QUALITY_MAX 40
#define SETTING_SAVE_QUALITY_DEFAULT 18
#define SETTING_AUTO_SAVE "auto_save"
#define AUTO_SAVE_OFF 0
#define AUTO_SAVE_ON_LOAD 1
#define AUTO_SAVE_ON_RELEASE 2
#ifndef SEC_TO_NSEC
#define SEC_TO_NSEC 1000000000ULL
#endif
#ifndef MSEC_TO_NSEC
#define MSEC_TO_NSEC 1000000ULL
#endif
#ifndef MAX_TS_VAR
#define MAX_TS_VAR 2000000000ULL
#endif

#ifdef __cplusplus
}
#endif
