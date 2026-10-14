#pragma once

#include <obs-module.h>
#include "replay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REPLAY_WS_VENDOR_NAME "ReplaySource"

struct replay_source;

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
};

/* Lifecycle called from replay-module.c */
void replay_control_init(void);
void replay_control_shutdown(void);
void replay_control_register_vendor(void);

/* Global multi-angle actions */
size_t replay_control_load_all(const char *const *filter_names, size_t filter_count, obs_data_t *response_out);
void replay_control_set_all_speed(float speed_percent);
void replay_control_restart_all(void);
void replay_control_pause_all(void);
void replay_control_all_mark_in(void);
void replay_control_all_mark_out(void);
void replay_control_all_mark_reset(void);

/* Event emitters (thread-safe; never call while holding source/queue locks) */
void replay_control_emit_replay_loaded(const char *source_name, int64_t duration_ms);
void replay_control_emit_save_started(const char *source_name, const char *path, const char *trigger);
void replay_control_emit_save_finished(const char *source_name, const char *path, bool ok);

/* Per-source hooks implemented in replay-source.c */
bool replay_source_execute_action(struct replay_source *context, const char *action, bool from_update);
void replay_source_set_speed(struct replay_source *context, float speed_percent);
void replay_source_restart_playback(struct replay_source *context);
void replay_source_toggle_pause(struct replay_source *context);
bool replay_source_get_status(struct replay_source *context, struct replay_source_status *out);
bool replay_source_load_unaligned(struct replay_source *context, uint64_t *first_ts_out, uint64_t *last_ts_out);
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

#ifdef __cplusplus
}
#endif
