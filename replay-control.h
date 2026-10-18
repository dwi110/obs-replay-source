#pragma once

#include <obs-module.h>
#include "replay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REPLAY_WS_VENDOR_NAME "ReplaySource"

struct replay_source;
struct replay;

struct replay_source_status {
	const char *name;
	const char *video_source;
	int loaded_replays;
	bool active;
	bool playing;
	float speed_percent;
	bool backward;
	int64_t duration_ms;
	int64_t buffered_ms;
	uint64_t effective_start_ns;
	uint64_t effective_end_ns;
	uint64_t effective_duration_ns;
	int64_t mark_in_ms;
	int64_t mark_out_ms;
	int slowmo_blend;
};

/* Lifecycle called from replay-module.c */
void replay_control_init(void);
void replay_control_shutdown(void);
void replay_control_register_vendor(void);

/* Global multi-angle actions */
size_t replay_control_load_all(const char *const *filter_names, size_t filter_count, obs_data_t *response_out);
void replay_control_set_all_speed(float speed_percent);
void replay_control_set_all_slowmo_blend(int mode);
void replay_control_restart_all(void);
void replay_control_pause_all(void);
void replay_control_all_mark_in(void);
void replay_control_all_mark_out(void);
void replay_control_all_mark_reset(void);

/* Event emitters (thread-safe; never call while holding source/queue locks) */
void replay_control_emit_replay_loaded(const char *source_name, int64_t duration_ms);
void replay_control_emit_save_started(const char *source_name, const char *path, const char *trigger);
void replay_control_emit_save_finished(const char *source_name, const char *path, bool ok);
/* Optional hook so the contract layer learns about save jobs. trigger carries the contract job id. */
void replay_control_set_save_hook(void (*hook)(const char *trigger, const char *path, bool started, bool ok));

/* Per-source hooks implemented in replay-source.c */
bool replay_source_execute_action(struct replay_source *context, const char *action, bool from_update);
void replay_source_set_speed(struct replay_source *context, float speed_percent);
/* Sets SLOWMO_BLEND_* at runtime and stores it in the source settings (persisted with the scene collection). */
void replay_source_set_slowmo_blend(struct replay_source *context, int mode);
void replay_source_restart_playback(struct replay_source *context);
void replay_source_toggle_pause(struct replay_source *context);
bool replay_source_get_status(struct replay_source *context, struct replay_source_status *out);
/* Loads the full buffer as a new replay without trimming. last_arrival_ns_out: receive-clock time
 * (os_gettime_ns) of the newest frame, comparable across sources unlike the frame timestamps. */
bool replay_source_load_unaligned(struct replay_source *context, uint64_t *first_ts_out, uint64_t *last_ts_out,
				  uint64_t *last_arrival_ns_out);
/* Removes the replay added by replay_source_load_unaligned (the newest one). */
void replay_source_unload_newest(struct replay_source *context);
void replay_source_align_loaded_replay(struct replay_source *context, uint64_t common_start_ns, uint64_t common_end_ns,
				       uint64_t *start_ns_out, uint64_t *duration_ns_out, int64_t *trim_front_ns_out,
				       int64_t *trim_end_ns_out);
bool replay_source_get_active_window_frames(struct replay_source *context, uint64_t *first_frame_ts_out,
					    uint64_t *last_frame_ts_out);
bool replay_source_get_current_playback_time(struct replay_source *context, uint64_t *abs_ts_out);
bool replay_source_set_mark_in(struct replay_source *context, uint64_t abs_ts);
bool replay_source_set_mark_out(struct replay_source *context, uint64_t abs_ts);
void replay_source_mark_reset(struct replay_source *context);
void replay_source_get_marks_ms(struct replay_source *context, int64_t *in_ms_out, int64_t *out_ms_out,
				int64_t *dur_ms_out);
void replay_source_auto_save_loaded(struct replay_source *context, const char *trigger);
void replay_source_test_set_playback_pos(struct replay_source *context, uint64_t pause_ts, uint64_t start_ts);

/* ---- Channel primitives (MulticamReplay contract, one clip across every replay source) ---- */
void replay_source_set_channel(struct replay_source *context, bool member, bool loop);
/* Copies the capture filter's buffer bounds without consuming them. Returns false if the source has no video buffer. */
bool replay_source_peek_capture(struct replay_source *context, uint64_t *first_ts_out, uint64_t *last_ts_out,
				uint64_t *last_arrival_ns_out, uint64_t *late_ns_out);
bool replay_source_install_replay(struct replay_source *context, const struct replay *replay);
/* Builds a replay from decoded frames (addref'd) and installs it. */
bool replay_source_install_decoded(struct replay_source *context, struct obs_source_frame **frames, uint64_t frame_count,
				   struct obs_audio_data *audio, uint64_t audio_count, const struct audio_convert_info *oai,
				   uint64_t first_ts, uint64_t last_ts, uint64_t duration);
/* Position on the clip axis (0 = first frame of the loaded replay, marks clamp the caller). Paused seeks emit the
 * frame once. */
bool replay_source_seek_ns(struct replay_source *context, uint64_t offset_ns);
void replay_source_set_playing(struct replay_source *context, bool playing);
void replay_source_set_backward(struct replay_source *context, bool backward);
void replay_source_channel_step(struct replay_source *context, int64_t delta_ns);
bool replay_source_channel_status(struct replay_source *context, bool *playing_out, bool *ended_out, bool *backward_out,
				  uint64_t *abs_ts_out, float *speed_out, uint64_t *first_ts_out,
				  uint64_t *duration_ns_out);
/* Returns an addref'd copy of the loaded replay's audio. Caller owns the array and each packet. */
bool replay_source_copy_replay_audio(struct replay_source *context, struct obs_audio_data **packets_out,
				     uint64_t *count_out, struct audio_convert_info *oai_out);
void replay_source_set_replay_audio(struct replay_source *context, struct obs_audio_data *packets, uint64_t count,
				    const struct audio_convert_info *oai);
void replay_source_clear_replays(struct replay_source *context);
const char *replay_source_directory(struct replay_source *context);

#ifdef __cplusplus
}
#endif
