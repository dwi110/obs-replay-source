#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>
#include <obs-frontend-api.h>
#include "replay.h"
#include "replay-save-queue.h"
#include <inttypes.h>
#include "replay-control.h"
#include "replay-blend.h"
#include <stddef.h>
#include <string.h>

#define VISIBILITY_ACTION_RESTART 0
#define VISIBILITY_ACTION_PAUSE 1
#define VISIBILITY_ACTION_CONTINUE 2
#define VISIBILITY_ACTION_NONE 3

#define END_ACTION_HIDE 0
#define END_ACTION_PAUSE 1
#define END_ACTION_LOOP 2
#define END_ACTION_REVERSE 3
#define END_ACTION_HIDE_ALL 4
#define END_ACTION_PAUSE_ALL 5
#define END_ACTION_LOOP_ALL 6
#define END_ACTION_REVERSE_ALL 7

enum saving_status {
	SAVING_STATUS_NONE = 0,
	SAVING_STATUS_STARTING = 1,
	SAVING_STATUS_STARTING2 = 2,
	SAVING_STATUS_SAVING = 3,
	SAVING_STATUS_STOPPING = 4
};

struct replay {
	struct obs_source_frame **video_frames;
	uint64_t video_frame_count;
	struct obs_audio_data *audio_frames;
	struct audio_convert_info oai;
	uint64_t audio_frame_count;
	uint64_t first_frame_timestamp;
	uint64_t last_frame_timestamp;
	uint64_t duration;
	int64_t trim_front;
	int64_t trim_end;
	int64_t align_front;
	int64_t align_end;
	bool has_align;
	bool auto_saved;
	/* Receive-clock time of the newest frame (audio packet for audio-only replays) at load. */
	uint64_t last_arrival_ns;
};

struct replay_source {
	obs_source_t *source;
	obs_source_t *source_filter;
	obs_source_t *source_audio_filter;
	char *source_name;
	char *source_audio_name;
	float speed_percent;
	bool backward;
	bool backward_start;
	int slowmo_blend;
	struct replay_blend_worker *blend_worker;
	int visibility_action;
	int end_action;
	char *next_scene_name;
	bool next_scene_disabled;
	char *load_switch_scene_name;
	obs_hotkey_id replay_hotkey;
	obs_hotkey_id next_hotkey;
	obs_hotkey_id previous_hotkey;
	obs_hotkey_id first_hotkey;
	obs_hotkey_id last_hotkey;
	obs_hotkey_id remove_hotkey;
	obs_hotkey_id clear_hotkey;
	obs_hotkey_id restart_hotkey;
	obs_hotkey_id pause_hotkey;
	obs_hotkey_id faster_hotkey;
	obs_hotkey_id slower_hotkey;
	obs_hotkey_id faster_by_5_hotkey;
	obs_hotkey_id slower_by_5_hotkey;
	obs_hotkey_id normal_or_faster_hotkey;
	obs_hotkey_id normal_or_slower_hotkey;
	obs_hotkey_id normal_speed_hotkey;
	obs_hotkey_id half_speed_hotkey;
	obs_hotkey_id double_speed_hotkey;
	obs_hotkey_id trim_front_hotkey;
	obs_hotkey_id trim_end_hotkey;
	obs_hotkey_id trim_reset_hotkey;
	obs_hotkey_id reverse_hotkey;
	obs_hotkey_id forward_hotkey;
	obs_hotkey_id backward_hotkey;
	obs_hotkey_id forward_or_faster_hotkey;
	obs_hotkey_id backward_or_faster_hotkey;
	obs_hotkey_id save_hotkey;
	obs_hotkey_id enable_hotkey;
	obs_hotkey_id disable_hotkey;
	obs_hotkey_id enable_next_scene_hotkey;
	obs_hotkey_id disable_next_scene_hotkey;
	obs_hotkey_id next_scene_current_hotkey;
	obs_hotkey_id next_scene_hotkey;
	obs_hotkey_id next_frame_hotkey;
	obs_hotkey_id prev_frame_hotkey;
	obs_hotkey_id next_n_frames_hotkey;
	obs_hotkey_id prev_n_frames_hotkey;
	uint64_t start_timestamp;
	uint64_t previous_frame_timestamp;
	uint64_t pause_timestamp;
	int64_t start_delay;
	int64_t retrieve_delay;
	int64_t frame_step_count;
	uint64_t retrieve_timestamp;
	uint64_t threshold_timestamp;
	struct obs_source_audio audio;

	bool disabled;
	bool play;
	bool restart;
	bool active;
	bool end;
	bool stepped;
	enum saving_status saving_status;

	int replay_position;
	int replay_max;
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	struct deque replays;
#else
	struct circlebuf replays;
#endif
	struct replay current_replay;
	struct replay saving_replay;

	uint64_t video_frame_position;
	uint64_t video_save_position;

	/* stores the audio data */
	uint64_t audio_frame_position;
	struct obs_audio_data audio_output;

	pthread_mutex_t video_mutex;
	pthread_mutex_t audio_mutex;
	pthread_mutex_t replay_mutex;

	bool lossless;
	char *file_format;
	char *directory;
	char *save_encoder;
	char *save_container;
	int save_quality;
	char *progress_source_name;
	char *text_source_name;
	char *text_format;
	bool sound_trigger;
	bool filter_loaded;
	bool free_after_save;
	bool keep_buffer;
	int auto_release;
	int auto_save;
	bool destroying;
};

static void replace_text(struct dstr *str, size_t pos, size_t len, const char *new_text)
{
	struct dstr front = {0};
	struct dstr back = {0};

	dstr_left(&front, str, pos);
	dstr_right(&back, str, pos + len);
	dstr_copy_dstr(str, &front);
	dstr_cat(str, new_text);
	dstr_cat_dstr(str, &back);
	dstr_free(&front);
	dstr_free(&back);
}

static void replay_update_text(struct replay_source *c)
{
	if (!c->text_source_name || !c->text_format)
		return;
	obs_source_t *s = obs_get_source_by_name(c->text_source_name);
	if (!s)
		return;

	struct dstr sf;
	size_t pos = 0;
	struct dstr buffer;
	dstr_init(&buffer);
	dstr_init_copy(&sf, c->text_format);
	while (pos < sf.len) {
		//duration, speed, index, count
		const char *cmp = sf.array + pos;
		if (astrcmp_n(cmp, "%SPEED%", 7) == 0) {
			dstr_printf(&buffer, "%.1f", c->speed_percent * (c->backward ? -1.0f : 1.0f));
			dstr_cat_ch(&buffer, '%');
			replace_text(&sf, pos, 7, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%PROGRESS%", 10) == 0) {
			if (c->current_replay.video_frame_count && c->video_frame_position < c->current_replay.video_frame_count) {
				dstr_printf(&buffer, "%.1f", c->video_frame_position * 100.0 / c->current_replay.video_frame_count);
				dstr_cat_ch(&buffer, '%');
			} else {
				dstr_copy(&buffer, "");
			}
			replace_text(&sf, pos, 10, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%COUNT%", 7) == 0) {
			dstr_printf(&buffer, "%d", (int)(c->replays.size / sizeof c->current_replay));
			replace_text(&sf, pos, 7, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%INDEX%", 7) == 0) {
			if (c->replays.size) {
				dstr_printf(&buffer, "%d", c->replay_position + 1);
			} else {
				dstr_copy(&buffer, "0");
			}
			replace_text(&sf, pos, 7, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%DURATION%", 10) == 0) {
			if (c->replays.size) {
				dstr_printf(&buffer, "%.2f", (double)c->current_replay.duration / (double)1000000000.0);
			} else {
				dstr_copy(&buffer, "");
			}
			replace_text(&sf, pos, 10, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%TIME%", 6) == 0) {
			if (c->replays.size && c->start_timestamp) {
				int64_t time = 0;
				if (c->pause_timestamp > c->start_timestamp) {
					time = c->pause_timestamp - c->start_timestamp;
				} else {
					time = obs_get_video_frame_time() - c->start_timestamp;
				}
				if (c->speed_percent != 100.0f) {
					time = (int64_t)((float)time * c->speed_percent / 100.0f);
				}
				dstr_printf(&buffer, "%.2f", (double)time / (double)1000000000.0);
			} else {
				dstr_copy(&buffer, "");
			}
			replace_text(&sf, pos, 6, buffer.array);
			pos += buffer.len;
		} else if (astrcmp_n(cmp, "%FPS%", 5) == 0) {
			if (c->current_replay.video_frame_count && c->current_replay.duration) {
				dstr_printf(&buffer, "%d",
					    (int)(c->current_replay.video_frame_count * 1000000000U / c->current_replay.duration));
			} else {
				dstr_copy(&buffer, "0");
			}
			replace_text(&sf, pos, 5, buffer.array);
			pos += buffer.len;
		} else {
			pos++;
		}
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", sf.array);
	obs_source_update(s, settings);
	obs_data_release(settings);
	dstr_free(&sf);
	dstr_free(&buffer);
	obs_source_release(s);
}

struct siu {
	uint32_t crop_width;
	obs_source_t *source;
};
static bool EnumSceneItem(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	struct siu *siu = data;
	if (obs_sceneitem_get_source(item) == siu->source) {
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		crop.left = 0;
		crop.right = siu->crop_width;
		obs_sceneitem_set_crop(item, &crop);
	} else if (obs_sceneitem_is_group(item)) {
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), EnumSceneItem, data);
	}
	return true;
}

static bool EnumScenesItems(void *data, obs_source_t *source)
{
	obs_scene_t *scene = obs_scene_from_source(source);
	obs_scene_enum_items(scene, EnumSceneItem, data);
	return true;
}

static void replay_update_progress_crop(struct replay_source *context, uint64_t t)
{
	if (!context->progress_source_name)
		return;
	obs_source_t *s = obs_get_source_by_name(context->progress_source_name);
	if (!s)
		return;
	const uint32_t width = obs_source_get_base_width(s);
	if (width) {
		struct siu siu;
		siu.source = s;
		if (t && context->current_replay.last_frame_timestamp && context->current_replay.duration) {
			siu.crop_width = (uint32_t)((context->current_replay.last_frame_timestamp - t) * width /
						    context->current_replay.duration);
		} else {
			siu.crop_width = width;
		}
		obs_enum_scenes(EnumScenesItems, &siu);
	}
	obs_source_release(s);
}

static const char *replay_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplaySource");
}

static void EnumFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	UNUSED_PARAMETER(source);
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_unversioned_id(filter);
	if ((strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 || strcmp(REPLAY_FILTER_ID, id) == 0) && strcmp(filterName, sourceName) == 0)
		c->source_filter = filter;
}
static void EnumAudioFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	UNUSED_PARAMETER(source);
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_unversioned_id(filter);
	if (strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 && strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;
}
static void EnumAudioVideoFilter(obs_source_t *source, obs_source_t *filter, void *data)
{
	UNUSED_PARAMETER(source);
	struct replay_source *c = data;
	const char *filterName = obs_source_get_name(filter);
	const char *sourceName = obs_source_get_name(c->source);
	const char *id = obs_source_get_unversioned_id(filter);
	if ((strcmp(REPLAY_FILTER_AUDIO_ID, id) == 0 || strcmp(REPLAY_FILTER_ASYNC_ID, id) == 0 ||
	     strcmp(REPLAY_FILTER_ID, id) == 0) &&
	    strcmp(filterName, sourceName) == 0)
		c->source_audio_filter = filter;
}

static inline void obs_source_signal(struct obs_source *source, const char *signal_source)
{
	struct calldata data;
	uint8_t stack[128];

	calldata_init_fixed(&data, stack, sizeof(stack));
	calldata_set_ptr(&data, "source", source);
	if (signal_source)
		signal_handler_signal(obs_source_get_signal_handler(source), signal_source, &data);
}

static void replay_reverse(struct replay_source *c, uint64_t os_timestamp)
{
	c->backward = !c->backward;
	if (c->blend_worker)
		replay_blend_worker_flush(c->blend_worker);
	if (c->end) {
		c->end = false;
		if (c->backward && c->current_replay.video_frame_count) {
			c->video_frame_position = c->current_replay.video_frame_count - 1;
		} else {
			c->video_frame_position = 0;
		}
	}
	const int64_t duration =
		(int64_t)(((int64_t)c->current_replay.last_frame_timestamp - (int64_t)c->current_replay.first_frame_timestamp) *
			  100.0 / c->speed_percent);
	int64_t play_duration = os_timestamp - c->start_timestamp;
	if (play_duration > duration) {
		play_duration = duration;
	}
	c->start_timestamp = os_timestamp - duration + play_duration;
}

static void replay_reverse_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (pressed) {
		const int64_t time = obs_get_video_frame_time();
		if (c->pause_timestamp && !c->stepped) {
			c->start_timestamp += time - c->pause_timestamp;
			c->pause_timestamp = 0;
		}
		replay_reverse(c, time);
		if (!c->play && !c->stepped) {
			c->play = true;
			obs_source_signal(c->source, "media_play");
		}
	}
}

static void replay_forward_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	const int64_t time = obs_get_video_frame_time();
	if (c->pause_timestamp) {
		c->start_timestamp += time - c->pause_timestamp;
		c->pause_timestamp = 0;
	}
	if (!c->play) {
		c->play = true;
		obs_source_signal(c->source, "media_play");
	}
	if (c->end) {
		c->end = false;
		c->video_frame_position = 0;
		c->start_timestamp = obs_get_video_frame_time();
		c->backward = false;
	} else if (c->backward) {
		c->backward = false;
		if (c->blend_worker)
			replay_blend_worker_flush(c->blend_worker);

		const int64_t duration = (int64_t)(((int64_t)c->current_replay.last_frame_timestamp -
						    (int64_t)c->current_replay.first_frame_timestamp) *
						   100.0 / c->speed_percent);
		int64_t play_duration = time - c->start_timestamp;
		if (play_duration > duration) {
			play_duration = duration;
		}
		c->start_timestamp = time - duration + play_duration;
	}
}

static void replay_backward_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	const int64_t time = obs_get_video_frame_time();
	if (c->pause_timestamp) {
		c->start_timestamp += time - c->pause_timestamp;
		c->pause_timestamp = 0;
	}
	if (!c->play) {
		c->play = true;
		obs_source_signal(c->source, "media_play");
	}
	if (c->end || c->video_frame_position == 0) {
		c->end = false;
		if (c->current_replay.video_frame_count)
			c->video_frame_position = c->current_replay.video_frame_count - 1;
		c->start_timestamp = obs_get_video_frame_time();
		c->backward = true;
	} else if (!c->backward) {
		c->backward = true;
		if (c->blend_worker)
			replay_blend_worker_flush(c->blend_worker);

		const int64_t duration = (int64_t)(((int64_t)c->current_replay.last_frame_timestamp -
						    (int64_t)c->current_replay.first_frame_timestamp) *
						   100.0 / c->speed_percent);
		int64_t play_duration = time - c->start_timestamp;
		if (play_duration > duration) {
			play_duration = duration;
		}
		c->start_timestamp = time - duration + play_duration;
	}
}

uint64_t find_closest_frame(void *data, uint64_t ts, bool le);

static void replay_update_position(struct replay_source *context, bool lock)
{
	if (lock)
		pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);
	if (context->blend_worker)
		replay_blend_worker_flush(context->blend_worker);
	context->previous_frame_timestamp = 0;
	const int replay_count = (int)(context->replays.size / sizeof context->current_replay);
	if (replay_count == 0) {
		context->current_replay.video_frame_count = 0;
		context->current_replay.video_frames = NULL;
		context->current_replay.audio_frame_count = 0;
		context->current_replay.audio_frames = NULL;
		context->replay_position = 0;
		obs_source_output_video(context->source, NULL);
		pthread_mutex_unlock(&context->audio_mutex);
		if (lock)
			pthread_mutex_unlock(&context->video_mutex);
		blog(LOG_INFO, "[replay_source: '%s'] No replay active", obs_source_get_name(context->source));
		return;
	}
	if (context->replay_position >= replay_count) {
		context->replay_position = replay_count - 1;
	} else if (context->replay_position < 0) {
		context->replay_position = 0;
	}
	memcpy(&context->current_replay,
	       circlebuf_data(&context->replays, context->replay_position * sizeof context->current_replay),
	       sizeof context->current_replay);
	context->video_frame_position = 0;
	context->audio_frame_position = 0;
	context->start_timestamp = obs_get_video_frame_time();
	context->backward = context->backward_start;
	if (!context->backward && context->current_replay.trim_front != 0) {
		context->start_timestamp -= (uint64_t)(context->current_replay.trim_front * 100.0 / context->speed_percent);
	} else if (context->backward && context->current_replay.trim_end != 0) {
		context->start_timestamp -= (uint64_t)(context->current_replay.trim_end * 100.0 / context->speed_percent);
	}
	context->pause_timestamp = 0;
	if (context->backward && context->current_replay.video_frame_count) {
		context->video_frame_position = context->current_replay.video_frame_count - 1;
		if (context->current_replay.trim_end > 0 &&
		    (uint64_t)context->current_replay.trim_end < context->current_replay.duration) {
			uint64_t desired_ts =
				context->current_replay.last_frame_timestamp - (uint64_t)context->current_replay.trim_end;
			context->video_frame_position = find_closest_frame(context, desired_ts, true);
		}
	} else if (!context->backward && context->current_replay.video_frame_count &&
		   context->current_replay.trim_front > 0 &&
		   (uint64_t)context->current_replay.trim_front < context->current_replay.duration) {
		uint64_t desired_ts =
			context->current_replay.first_frame_timestamp + (uint64_t)context->current_replay.trim_front;
		context->video_frame_position = find_closest_frame(context, desired_ts, false);
	}
	if (context->active || context->visibility_action == VISIBILITY_ACTION_CONTINUE) {
		if (!context->play && !context->stepped) {
			context->play = true;
			obs_source_signal(context->source, "media_play");
		}
	} else {
		if (context->play) {
			context->play = false;
			obs_source_signal(context->source, "media_pause");
		}
		context->pause_timestamp = obs_get_video_frame_time();
	}

	pthread_mutex_unlock(&context->audio_mutex);
	if (lock)
		pthread_mutex_unlock(&context->video_mutex);

	replay_update_text(context);
}
static bool replay_save_replay(struct replay_source *context, struct replay *r, const char *trigger);

static void replay_free_replay(struct replay *replay, struct replay_source *context)
{
	if (context && !context->destroying && context->auto_save == AUTO_SAVE_ON_RELEASE && !replay->auto_saved) {
		replay->auto_saved = true;
		replay_save_replay(context, replay, "auto-release");
	}
	if (replay == &context->saving_replay) {
		context->free_after_save = false;
	} else if (replay->video_frames == context->saving_replay.video_frames && context->saving_status != SAVING_STATUS_NONE) {
		context->free_after_save = true;
		return;
	}
	for (uint64_t i = 0; i < replay->video_frame_count; i++) {
		struct obs_source_frame *frame = replay->video_frames[i];
		if (frame) {
			replay_frame_release(frame);
			replay->video_frames[i] = NULL;
		}
	}
	replay->video_frame_count = 0;
	if (replay->video_frames) {
		bfree(replay->video_frames);
		replay->video_frames = NULL;
	}

	for (uint64_t i = 0; i < replay->audio_frame_count; i++) {
		free_audio_packet(&replay->audio_frames[i]);
	}
	replay->audio_frame_count = 0;
	if (replay->audio_frames) {
		bfree(replay->audio_frames);
		replay->audio_frames = NULL;
	}
}
static void replay_purge_replays(struct replay_source *context)
{
	if ((int)(context->replays.size / sizeof context->current_replay) > context->replay_max) {
		/* Lock order video_mutex -> replay_mutex, same as the tick (auto release) and the mark setters. */
		pthread_mutex_lock(&context->video_mutex);
		pthread_mutex_lock(&context->replay_mutex);
		const int replays_to_delete = (int)(context->replays.size / sizeof context->current_replay - context->replay_max);
		if (replays_to_delete > context->replay_position) {
			context->replay_position = replays_to_delete;
			replay_update_position(context, false);
		}
		while ((int)(context->replays.size / sizeof context->current_replay) > context->replay_max) {
			struct replay old_replay;
			circlebuf_pop_front(&context->replays, &old_replay, sizeof context->current_replay);
			replay_free_replay(&old_replay, context);
			context->replay_position--;
		}
		if (context->replay_max > 1)
			blog(LOG_INFO, "[replay_source: '%s'] switched to replay %i/%i", obs_source_get_name(context->source),
			     context->replay_position + 1, (int)(context->replays.size / sizeof context->current_replay));
		pthread_mutex_unlock(&context->replay_mutex);
		pthread_mutex_unlock(&context->video_mutex);
	}
}
static void replay_auto_release(struct replay_source *context, bool lock)
{
	if (context->auto_release == AUTO_RELEASE_NEVER || context->replays.size == 0)
		return;

	if (lock)
		pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);
	context->current_replay.video_frame_count = 0;
	context->current_replay.video_frames = NULL;
	context->current_replay.audio_frame_count = 0;
	context->current_replay.audio_frames = NULL;
	context->replay_position = 0;
	context->end = true;
	context->play = false;
	pthread_mutex_unlock(&context->audio_mutex);
	if (lock)
		pthread_mutex_unlock(&context->video_mutex);

	obs_source_output_video(context->source, NULL);

	pthread_mutex_lock(&context->replay_mutex);
	while (context->replays.size) {
		struct replay replay;
		circlebuf_pop_front(&context->replays, &replay, sizeof replay);
		replay_free_replay(&replay, context);
	}
	pthread_mutex_unlock(&context->replay_mutex);

	replay_update_text(context);
	replay_update_progress_crop(context, 0);
	blog(LOG_INFO, "[replay_source: '%s'] auto-released loaded replay", obs_source_get_name(context->source));
}
static void replay_retrieve(struct replay_source *context);

void replay_trigger_threshold(void *data)
{
	struct replay_source *context = data;
	const uint64_t os_time = obs_get_video_frame_time();
	uint64_t duration = context->current_replay.duration;
	if (context->speed_percent < 100.f)
		duration = (uint64_t)(duration * 100.0 / context->speed_percent);
	if (context->threshold_timestamp && context->threshold_timestamp + context->retrieve_delay + duration > os_time)
		return;

	context->threshold_timestamp = os_time;
	blog(LOG_INFO, "[replay_source: '%s'] audio triggered", obs_source_get_name(context->source));
	if (context->retrieve_delay > 0) {
		context->retrieve_timestamp = obs_get_video_frame_time() + context->retrieve_delay;
	} else {
		replay_retrieve(context);
	}
}

static void replay_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_DURATION, 5000);
	obs_data_set_default_int(settings, SETTING_REPLAYS, 1);
	obs_data_set_default_int(settings, SETTING_SPEED, 100);
	obs_data_set_default_int(settings, SETTING_VISIBILITY_ACTION, VISIBILITY_ACTION_CONTINUE);
	obs_data_set_default_int(settings, SETTING_START_DELAY, 0);
	obs_data_set_default_int(settings, SETTING_FRAME_STEP_COUNT, 5);
	obs_data_set_default_int(settings, SETTING_END_ACTION, END_ACTION_LOOP);
	obs_data_set_default_bool(settings, SETTING_BACKWARD, false);
	obs_data_set_default_int(settings, SETTING_SLOWMO_BLEND, SLOWMO_BLEND_OFF);
	obs_data_set_default_string(settings, SETTING_FILE_FORMAT, "%CCYY-%MM-%DD %hh.%mm.%ss");
	obs_data_set_default_bool(settings, SETTING_LOSSLESS, false);
	obs_data_set_default_int(settings, SETTING_STORAGE_FORMAT, STORAGE_FORMAT_NV12);
	obs_data_set_default_int(settings, SETTING_DOWNSCALE, DOWNSCALE_NONE);
	obs_data_set_default_bool(settings, SETTING_KEEP_BUFFER, true);
	obs_data_set_default_int(settings, SETTING_AUTO_RELEASE, AUTO_RELEASE_NEVER);
	obs_data_set_default_int(settings, SETTING_RESERVE_MODE, RESERVE_BUFFER_PLUS_ONE);
	obs_data_set_default_string(settings, SETTING_SAVE_ENCODER, SAVE_ENCODER_AUTO);
	obs_data_set_default_string(settings, SETTING_SAVE_CONTAINER, SAVE_CONTAINER_MP4);
	obs_data_set_default_int(settings, SETTING_SAVE_QUALITY, SETTING_SAVE_QUALITY_DEFAULT);
	obs_data_set_default_int(settings, SETTING_AUTO_SAVE, AUTO_SAVE_OFF);
}

static void replay_source_show(void *data)
{
	struct replay_source *context = data;
	UNUSED_PARAMETER(context);
}

static void replay_source_hide(void *data)
{
	struct replay_source *context = data;
	if (context->auto_release != AUTO_RELEASE_NEVER)
		replay_auto_release(context, true);
}

static void replay_source_active(void *data)
{
	struct replay_source *context = data;
	if (context->visibility_action == VISIBILITY_ACTION_PAUSE || context->visibility_action == VISIBILITY_ACTION_CONTINUE) {
		if (!context->play && !context->end) {
			context->play = true;
			if (context->pause_timestamp) {
				context->start_timestamp += obs_get_video_frame_time() - context->pause_timestamp;
				context->pause_timestamp = 0;
			}
			obs_source_signal(context->source, "media_play");
		}
	} else if (context->visibility_action == VISIBILITY_ACTION_RESTART) {
		context->play = true;
		context->restart = true;
		obs_source_signal(context->source, "media_restart");
	}
	context->active = true;
}

static void replay_source_deactive(void *data)
{
	struct replay_source *context = data;
	if (context->visibility_action == VISIBILITY_ACTION_PAUSE) {
		if (context->play) {
			context->play = false;
			context->pause_timestamp = obs_get_video_frame_time();
			obs_source_signal(context->source, "media_pause");
		}
	} else if (context->visibility_action == VISIBILITY_ACTION_RESTART) {
		if (context->play) {
			context->play = false;
			obs_source_signal(context->source, "media_pause");
		}
		context->restart = true;
	}
	context->active = false;
	if (context->auto_release != AUTO_RELEASE_NEVER)
		replay_auto_release(context, true);
}

void replay_restart(void *data)
{
	struct replay_source *c = data;
	c->restart = true;
	c->play = true;
	if (c->blend_worker)
		replay_blend_worker_flush(c->blend_worker);
	obs_source_signal(c->source, "media_restart");
}

void replay_stop(void *data)
{
	struct replay_source *c = data;
	c->play = false;
	c->restart = true;
	if (c->blend_worker)
		replay_blend_worker_flush(c->blend_worker);
	obs_source_media_ended(c->source);
}

void replay_next(void *data)
{
	struct replay_source *context = data;
	const int replay_count = (int)(context->replays.size / sizeof context->current_replay);
	if (replay_count == 0)
		return;

	if (context->replay_position + 1 >= replay_count) {
		context->replay_position = replay_count - 1;
	} else {
		context->replay_position++;
	}
	replay_update_position(context, true);
	blog(LOG_INFO, "[replay_source: '%s'] next switched to replay %i/%i", obs_source_get_name(context->source),
	     context->replay_position + 1, replay_count);
}

void replay_previous(void *data)
{
	struct replay_source *context = data;
	if (context->replay_position <= 0) {
		context->replay_position = 0;
	} else {
		context->replay_position--;
	}
	replay_update_position(context, true);
	blog(LOG_INFO, "[replay_source: '%s'] previous hotkey switched to replay %i/%i", obs_source_get_name(context->source),
	     context->replay_position + 1, (int)(context->replays.size / sizeof context->current_replay));
}

static void replay_play_pause(void *data, bool pause)
{
	struct replay_source *c = data;
	if (pause) {
		if (c->play) {
			c->play = false;
			if (c->blend_worker)
				replay_blend_worker_flush(c->blend_worker);
			c->pause_timestamp = obs_get_video_frame_time();
			obs_source_signal(c->source, "media_pause");
		} else {
			c->play = true;
			if (c->pause_timestamp) {
				c->start_timestamp += obs_get_video_frame_time() - c->pause_timestamp;
				c->pause_timestamp = 0;
			}
			obs_source_signal(c->source, "media_play");
		}
	} else {
		const int64_t time = obs_get_video_frame_time();
		if (c->pause_timestamp) {
			c->start_timestamp += time - c->pause_timestamp;
			c->pause_timestamp = 0;
		}
		c->play = true;
		if (c->end || (c->video_frame_position == 0 && c->backward)) {
			c->end = false;
			if (c->backward) {
				if (c->current_replay.video_frame_count)
					c->video_frame_position = c->current_replay.video_frame_count - 1;
			} else {
				c->video_frame_position = 0;
			}
			c->start_timestamp = obs_get_video_frame_time();
		}
		obs_source_signal(c->source, "media_play");
	}
}

static void replay_restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_restart(data);
}

static void replay_pause_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_play_pause(data, true);
}

static bool replay_save_replay(struct replay_source *context, struct replay *r, const char *trigger)
{
	if (!context || !r)
		return false;

	if (r->video_frame_count == 0 || !r->video_frames)
		return false;

	const char *src_name = context->source ? obs_source_get_name(context->source) : NULL;
	if (!src_name || !*src_name)
		src_name = context->source_name;

	struct replay_save_job_params params = {
		.source_name = src_name,
		.directory = context->directory,
		.file_format = context->file_format,
		.save_encoder = context->save_encoder,
		.save_container = context->save_container,
		.save_quality = context->save_quality,
		.lossless = context->lossless,
		.trigger = trigger ? trigger : "manual",
		.video_frames = r->video_frames,
		.video_frame_count = r->video_frame_count,
		.audio_frames = r->audio_frames,
		.audio_frame_count = r->audio_frame_count,
		.oai = r->oai,
		.first_frame_timestamp = r->first_frame_timestamp,
		.last_frame_timestamp = r->last_frame_timestamp,
		.duration = r->duration,
		.trim_front = r->trim_front,
		.trim_end = r->trim_end,
	};

	return replay_save_queue_enqueue(&params);
}

void replay_save_with_trigger(struct replay_source *context, const char *trigger)
{
	if (!context)
		return;

	pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);

	context->saving_status = SAVING_STATUS_NONE;
	replay_save_replay(context, &context->current_replay, trigger ? trigger : "manual");

	pthread_mutex_unlock(&context->audio_mutex);
	pthread_mutex_unlock(&context->video_mutex);
}

void replay_save(struct replay_source *context)
{
	replay_save_with_trigger(context, "manual");
}

static void *update_scene_thread(void *data)
{
	obs_source_t *scene = data;
	obs_frontend_set_current_scene(scene);
	return NULL;
}

static bool replay_retrieve_internal(struct replay_source *context, bool emit_event)
{
	if (!context || context->disabled)
		return false;
	obs_source_t *s = (context->source_name && *context->source_name) ? obs_get_source_by_name(context->source_name)
									  : NULL;
	context->source_filter = NULL;
	if (s)
		obs_source_enum_filters(s, EnumFilter, context);

	obs_source_t *as = (context->source_audio_name && *context->source_audio_name)
				   ? obs_get_source_by_name(context->source_audio_name)
				   : NULL;
	context->source_audio_filter = NULL;
	if (as)
		obs_source_enum_filters(as, EnumAudioVideoFilter, context);

	struct replay_filter *vf = context->source_filter ? obs_obj_get_data(context->source_filter) : NULL;
	struct replay_filter *af = context->source_audio_filter ? obs_obj_get_data(context->source_audio_filter) : vf;
	if (vf && vf->video_frames.size == 0)
		vf = NULL;
	if (af && af->audio_frames.size == 0)
		af = NULL;

	if (!vf && !af) {
		if (s)
			obs_source_release(s);
		if (as)
			obs_source_release(as);
		return false;
	}

	struct replay new_replay;
	new_replay.last_frame_timestamp = 0;
	new_replay.first_frame_timestamp = 0;
	new_replay.trim_end = 0;
	new_replay.trim_front = 0;
	new_replay.align_front = 0;
	new_replay.align_end = 0;
	new_replay.has_align = false;
	new_replay.auto_saved = false;
	new_replay.last_arrival_ns = 0;
	if (vf) {
		struct obs_source_frame *frame;
		pthread_mutex_lock(&vf->mutex);
		if (vf->video_frames.size) {
			circlebuf_peek_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame *));
			new_replay.first_frame_timestamp = frame->timestamp;
			new_replay.last_frame_timestamp = frame->timestamp;
		}
		new_replay.video_frame_count = vf->video_frames.size / sizeof(struct obs_source_frame *);
		new_replay.video_frames = bzalloc((size_t)new_replay.video_frame_count * sizeof(struct obs_source_frame *));
		if (context->keep_buffer) {
			for (uint64_t i = 0; i < new_replay.video_frame_count; i++) {
				memcpy(&frame,
				       circlebuf_data(&vf->video_frames,
						      (size_t)i * sizeof(struct obs_source_frame *)),
				       sizeof(struct obs_source_frame *));
				replay_frame_addref(frame);
				new_replay.last_frame_timestamp = frame->timestamp;
				*(new_replay.video_frames + i) = frame;
			}
		} else {
			for (uint64_t i = 0; i < new_replay.video_frame_count; i++) {
				circlebuf_pop_front(&vf->video_frames, &frame, sizeof(struct obs_source_frame *));
				new_replay.last_frame_timestamp = frame->timestamp;
				*(new_replay.video_frames + i) = frame;
			}
		}
		new_replay.last_arrival_ns = vf->last_arrival_ns;
		pthread_mutex_unlock(&vf->mutex);
	} else {
		new_replay.video_frames = NULL;
		new_replay.video_frame_count = 0;
	}
	if (af) {
		pthread_mutex_lock(&af->mutex);
		struct obs_audio_data audio;
		if (!vf && af->audio_frames.size) {
			circlebuf_peek_front(&af->audio_frames, &audio, sizeof(struct obs_audio_data));
			new_replay.first_frame_timestamp = audio.timestamp;
			new_replay.last_frame_timestamp = audio.timestamp;
		}
		new_replay.oai = af->oai;
		if (!vf)
			new_replay.last_arrival_ns = af->last_arrival_ns;
		new_replay.audio_frame_count = af->audio_frames.size / sizeof(struct obs_audio_data);
		new_replay.audio_frames = bzalloc((size_t)new_replay.audio_frame_count * sizeof(struct obs_audio_data));
		if (context->keep_buffer) {
			for (uint64_t i = 0; i < new_replay.audio_frame_count; i++) {
				memcpy(&audio,
				       circlebuf_data(&af->audio_frames, (size_t)i * sizeof(struct obs_audio_data)),
				       sizeof(struct obs_audio_data));
				if (!vf) {
					new_replay.last_frame_timestamp = audio.timestamp;
				}
				memcpy(&new_replay.audio_frames[i], &audio, sizeof(struct obs_audio_data));
				replay_audio_packet_addref(&new_replay.audio_frames[i]);
			}
		} else {
			for (uint64_t i = 0; i < new_replay.audio_frame_count; i++) {
				circlebuf_pop_front(&af->audio_frames, &audio, sizeof(struct obs_audio_data));
				if (!vf) {
					new_replay.last_frame_timestamp = audio.timestamp;
				}
				memcpy(&new_replay.audio_frames[i], &audio, sizeof(struct obs_audio_data));
				for (size_t j = 0; j < MAX_AV_PLANES; j++) {
					if (!audio.data[j])
						break;

					new_replay.audio_frames[i].data[j] = audio.data[j];
				}
			}
		}
		pthread_mutex_unlock(&af->mutex);
	} else {
		new_replay.audio_frames = NULL;
		new_replay.audio_frame_count = 0;
		new_replay.oai.speakers = SPEAKERS_STEREO;
		new_replay.oai.samples_per_sec = 48000;
		new_replay.oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
	}
	if (s)
		obs_source_release(s);
	if (as)
		obs_source_release(as);
	new_replay.duration = new_replay.last_frame_timestamp - new_replay.first_frame_timestamp;

	if (context->start_delay > 0) {
		if (context->backward_start) {
			if (context->speed_percent == 100.0f) {
				new_replay.trim_end = context->start_delay * -1;
			} else {
				new_replay.trim_end = (int64_t)(context->start_delay * context->speed_percent / -100.0);
			}
			new_replay.trim_front = 0;
		} else {
			if (context->speed_percent == 100.0f) {
				new_replay.trim_front = context->start_delay * -1;
			} else {
				new_replay.trim_front = (int64_t)(context->start_delay * context->speed_percent / -100.0);
			}
			new_replay.trim_end = 0;
		}
	} else if (context->start_delay < 0 && context->start_delay * -1 < (int64_t)new_replay.duration) {
		new_replay.trim_front = context->start_delay * -1;
	}

	pthread_mutex_lock(&context->replay_mutex);
	circlebuf_push_back(&context->replays, &new_replay, sizeof new_replay);
	pthread_mutex_unlock(&context->replay_mutex);

	blog(LOG_INFO, "[replay_source: '%s'] replay added of %.2f seconds", obs_source_get_name(context->source),
	     (double)new_replay.duration / (double)1000000000.0);

	if (context->replays.size == sizeof new_replay) {
		replay_update_position(context, true);
	}
	replay_purge_replays(context);
	if (context->load_switch_scene_name) {
		s = obs_get_source_by_name(context->load_switch_scene_name);
		if (s) {
			pthread_t thread;
			pthread_create(&thread, NULL, update_scene_thread, s);
			obs_source_release(s);
		}
	}
	replay_update_text(context);
	if (emit_event && context->auto_save == AUTO_SAVE_ON_LOAD) {
		replay_source_auto_save_loaded(context, "auto-load");
	}
	if (emit_event) {
		uint64_t eff_start = new_replay.first_frame_timestamp;
		if (new_replay.trim_front > 0 && (uint64_t)new_replay.trim_front < new_replay.duration)
			eff_start += (uint64_t)new_replay.trim_front;
		uint64_t eff_end = new_replay.last_frame_timestamp;
		if (new_replay.trim_end > 0 && (uint64_t)new_replay.trim_end < new_replay.duration &&
		    eff_end - (uint64_t)new_replay.trim_end > eff_start)
			eff_end -= (uint64_t)new_replay.trim_end;
		const uint64_t eff_dur = (eff_end > eff_start) ? (eff_end - eff_start) : 0;
		replay_control_emit_replay_loaded(obs_source_get_name(context->source), (int64_t)(eff_dur / 1000000ULL));
	}
	return true;
}

static void replay_retrieve(struct replay_source *context)
{
	replay_retrieve_internal(context, true);
}

// Finds closest frame in the current replay given a desired timestamp.
uint64_t find_closest_frame(void *data, uint64_t ts, bool le)
{
	struct replay_source *c = data;
	int64_t count = c->current_replay.video_frame_count;
	struct obs_source_frame **frames = c->current_replay.video_frames;
	if (ts < c->current_replay.first_frame_timestamp) {
		return 0;
	} else if (ts > c->current_replay.last_frame_timestamp) {
		return count - 1;
	}

	int64_t i = 0, mid = 0, j = count;
	while (i < j) {
		mid = (i + j) / 2;
		if (frames[mid]->timestamp == ts) {
			return mid;
		} else if (frames[mid]->timestamp > ts) {
			if (mid > 0 && frames[mid - 1]->timestamp < ts) {
				return le ? mid - 1 : mid;
			}
			j = mid;
		} else if (frames[mid]->timestamp < ts) {
			if (mid < count - 1 && frames[mid + 1]->timestamp > ts) {
				return le ? mid : mid + 1;
			}
			i = mid + 1;
		}
	}
	if (i == mid + 1) {
		return le ? mid : mid + 1;
	}
	return le ? mid - 1 : mid;
}

static void replay_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed || !context->source_name)
		return;
	blog(LOG_INFO, "[replay_source: '%s'] Load replay pressed", obs_source_get_name(context->source));
	if (context->retrieve_delay > 0) {
		context->retrieve_timestamp = obs_get_video_frame_time() + context->retrieve_delay;
	} else {
		replay_retrieve(context);
	}
}

static void replay_save_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed)
		return;
	blog(LOG_INFO, "[replay_source: '%s'] Save replay pressed", obs_source_get_name(context->source));
	replay_save(context);
}

static void replay_disable_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || c->disabled)
		return;

	c->disabled = true;
	obs_source_update(c->source, NULL);
}

static void replay_enable_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || !c->disabled)
		return;

	c->disabled = false;
	obs_source_update(c->source, NULL);
}

static void replay_disable_next_scene_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || c->next_scene_disabled)
		return;

	c->next_scene_disabled = true;
}

static void replay_enable_next_scene_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || !c->next_scene_disabled)
		return;

	c->next_scene_disabled = false;
}

static void replay_next_scene_current_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed)
		return;

	obs_source_t *current_scene = obs_frontend_get_current_scene();
	if (current_scene == NULL)
		return;

	const char *next_scene_name = obs_source_get_name(current_scene);
	obs_source_release(current_scene);
	if (c->next_scene_name) {
		if (strcmp(c->next_scene_name, next_scene_name) != 0) {
			bfree(c->next_scene_name);
			c->next_scene_name = bstrdup(next_scene_name);
		}
	} else {
		c->next_scene_name = bstrdup(next_scene_name);
	}
}

static void replay_next_scene_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;
	if (!pressed || !c->next_scene_name)
		return;

	obs_source_t *current = obs_frontend_get_current_scene();
	if (current) {
		const char *current_name = obs_source_get_name(current);
		if (strcmp(current_name, c->next_scene_name) != 0) {
			obs_source_t *s = obs_get_source_by_name(c->next_scene_name);
			if (s) {
				pthread_t thread;
				pthread_create(&thread, NULL, update_scene_thread, s);
				obs_source_release(s);
			}
		}
		obs_source_release(current);
	} else {
		obs_source_t *s = obs_get_source_by_name(c->next_scene_name);
		if (s) {
			pthread_t thread;
			pthread_create(&thread, NULL, update_scene_thread, s);
			obs_source_release(s);
		}
	}
}

static struct obs_source_frame *replay_restart_at_begin(struct replay_source *c, const uint64_t os_timestamp)
{
	c->video_frame_position = 0;
	c->restart = false;
	c->start_timestamp = os_timestamp;
	c->pause_timestamp = c->play ? 0 : os_timestamp;
	c->audio_frame_position = 0;
	const int64_t duration =
		(int64_t)(((int64_t)c->current_replay.last_frame_timestamp - (int64_t)c->current_replay.first_frame_timestamp) *
			  100.0 / c->speed_percent);
	if (c->backward) {
		c->start_timestamp -= duration;
	}
	struct obs_source_frame *frame = c->current_replay.video_frames[c->video_frame_position];
	if (c->current_replay.trim_front != 0) {
		c->start_timestamp -= (uint64_t)(c->current_replay.trim_front * 100.0 / c->speed_percent);
		if (c->current_replay.trim_front < 0) {
			struct obs_source_frame out = *frame;
			out.timestamp = os_timestamp;
			c->previous_frame_timestamp = out.timestamp;
			obs_source_output_video(c->source, &out);
			return NULL;
		}
		uint64_t desired_ts = c->current_replay.first_frame_timestamp + c->current_replay.trim_front;
		c->video_frame_position = find_closest_frame(c, desired_ts, false);
		frame = c->current_replay.video_frames[c->video_frame_position];
	}
	return frame;
}

static struct obs_source_frame *replay_restart_at_end(struct replay_source *c, const uint64_t os_timestamp)
{
	c->video_frame_position = c->current_replay.video_frame_count - 1;
	struct obs_source_frame *frame = c->current_replay.video_frames[c->video_frame_position];
	const int64_t duration =
		(int64_t)(((int64_t)c->current_replay.last_frame_timestamp - (int64_t)c->current_replay.first_frame_timestamp) *
			  100.0 / c->speed_percent);
	c->start_timestamp = os_timestamp;
	if (!c->backward) {
		c->start_timestamp -= duration;
	}
	c->pause_timestamp = c->play ? 0 : os_timestamp;
	c->restart = false;
	if (c->current_replay.trim_end != 0) {
		c->start_timestamp -= (uint64_t)(c->current_replay.trim_end * 100.0 / c->speed_percent);

		if (c->current_replay.trim_end < 0) {
			struct obs_source_frame out = *frame;
			out.timestamp = os_timestamp;
			c->previous_frame_timestamp = out.timestamp;
			obs_source_output_video(c->source, &out);
			return NULL;
		}
		uint64_t desired_ts = c->current_replay.last_frame_timestamp - c->current_replay.trim_end;
		c->video_frame_position = find_closest_frame(c, desired_ts, true);
		frame = c->current_replay.video_frames[c->video_frame_position];
	}

	return frame;
}

void replay_source_end_action(struct replay_source *context);

void replay_step_frames(void *data, bool pressed, bool forward, uint64_t num_frames)
{
	struct replay_source *c = data;
	if (!c->current_replay.video_frame_count || !pressed)
		return;
	uint64_t os_timestamp = obs_get_video_frame_time();
	uint64_t next_pos = c->video_frame_position;
	c->stepped = true;
	if (c->play) {
		c->play = false;
		c->pause_timestamp = os_timestamp;
		obs_source_signal(c->source, "media_pause");
	}
	if (forward) {
		if (next_pos + num_frames >= c->current_replay.video_frame_count ||
		    c->current_replay.video_frames[next_pos + num_frames]->timestamp >
			    c->current_replay.last_frame_timestamp - c->current_replay.trim_end) {
			bool bs = c->backward_start;
			bool b = c->backward;
			c->backward_start = false;
			replay_source_end_action(c);
			c->backward_start = bs;
			if (b != c->backward) {
				replay_reverse(c, os_timestamp);
			}
			if (c->restart)
				replay_restart_at_begin(c, os_timestamp);
			return;
		} else {
			next_pos += num_frames;
		}

	} else {
		if (c->video_frame_position < num_frames ||
		    c->current_replay.video_frames[c->video_frame_position - num_frames]->timestamp <
			    c->current_replay.first_frame_timestamp + c->current_replay.trim_front) {
			bool bs = c->backward_start;
			bool b = c->backward;
			c->backward_start = true;
			replay_source_end_action(c);
			c->backward_start = bs;
			if (b != c->backward) {
				replay_reverse(c, os_timestamp);
			}
			if (c->restart)
				replay_restart_at_end(c, os_timestamp);
			return;
		} else {
			next_pos -= num_frames;
		}
	}
	int64_t next_time = c->current_replay.video_frames[next_pos]->timestamp;
	int64_t prev_time = c->current_replay.video_frames[c->video_frame_position]->timestamp;
	int64_t time_diff = (int64_t)((next_time - prev_time) * 100 / c->speed_percent);
	if (c->backward) {
		time_diff *= -1;
	}
	c->start_timestamp -= time_diff;
	c->video_frame_position = next_pos;
}

static void replay_next_n_frames_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct replay_source *c = data;
	replay_step_frames(data, pressed, true, c->frame_step_count);
}

static void replay_next_frame_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	replay_step_frames(data, pressed, true, 1);
}

static void replay_prev_n_frames_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct replay_source *c = data;
	replay_step_frames(data, pressed, false, c->frame_step_count);
}

static void replay_prev_frame_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	replay_step_frames(data, pressed, false, 1);
}

static void replay_next_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_next(data);
}

static void replay_previous_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		replay_previous(data);
}

static void replay_first_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed)
		return;

	context->replay_position = 0;
	replay_update_position(context, true);
	blog(LOG_INFO, "[replay_source: '%s'] first hotkey switched to replay %i/%i", obs_source_get_name(context->source),
	     context->replay_position + 1, (int)(context->replays.size / sizeof context->current_replay));
}

static void replay_last_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	const int replay_count = (int)(context->replays.size / sizeof context->current_replay);
	if (!pressed || replay_count == 0)
		return;

	context->replay_position = replay_count - 1;
	replay_update_position(context, true);
	blog(LOG_INFO, "[replay_source: '%s'] last hotkey switched to replay %i/%i", obs_source_get_name(context->source),
	     context->replay_position + 1, replay_count);
}

static void replay_remove_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed)
		return;
	const int replay_count = (int)(context->replays.size / sizeof context->current_replay);

	if (context->replay_position >= replay_count)
		return;
	if (context->blend_worker)
		replay_blend_worker_flush(context->blend_worker);

	pthread_mutex_lock(&context->replay_mutex);
	struct replay removed_replay;
	for (int i = 0; i < replay_count; i++) {
		if (i == context->replay_position) {
			circlebuf_pop_front(&context->replays, &removed_replay, sizeof removed_replay);
		} else {
			struct replay replay;
			circlebuf_pop_front(&context->replays, &replay, sizeof replay);
			circlebuf_push_back(&context->replays, &replay, sizeof replay);
		}
	}
	blog(LOG_INFO, "[replay_source: '%s'] remove hotkey removed %i/%i", obs_source_get_name(context->source),
	     context->replay_position + 1, replay_count);
	pthread_mutex_unlock(&context->replay_mutex);
	replay_update_position(context, true);
	replay_free_replay(&removed_replay, context);
}

static void replay_clear_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *context = data;
	if (!pressed || context->replays.size == 0)
		return;

	pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);
	if (context->blend_worker)
		replay_blend_worker_flush(context->blend_worker);
	context->current_replay.video_frame_count = 0;
	context->current_replay.audio_frame_count = 0;
	context->replay_position = 0;
	context->end = true;
	context->play = false;
	obs_source_media_ended(context->source);
	pthread_mutex_unlock(&context->audio_mutex);
	pthread_mutex_unlock(&context->video_mutex);
	obs_source_output_video(context->source, NULL);
	pthread_mutex_lock(&context->replay_mutex);
	while (context->replays.size) {
		struct replay replay;
		circlebuf_pop_front(&context->replays, &replay, sizeof replay);
		replay_free_replay(&replay, context);
	}
	pthread_mutex_unlock(&context->replay_mutex);
	replay_update_text(context);
	replay_update_progress_crop(context, 0);
	blog(LOG_INFO, "[replay_source: '%s'] clear hotkey", obs_source_get_name(context->source));

	obs_source_media_ended(context->source);
}
void update_speed(struct replay_source *context, float new_speed)
{
	if (new_speed < SETTING_SPEED_MIN)
		new_speed = SETTING_SPEED_MIN;
	if (new_speed > SETTING_SPEED_MAX)
		new_speed = SETTING_SPEED_MAX;

	if (new_speed == context->speed_percent)
		return;
	//info("update speed from %.2f to %.2f", context->speed_percent, new_speed);
	if (context->current_replay.video_frame_count) {
		struct obs_source_frame *peek_frame = context->current_replay.video_frames[context->video_frame_position];
		uint64_t duration = peek_frame->timestamp - context->current_replay.first_frame_timestamp;
		if (context->backward) {
			duration = context->current_replay.last_frame_timestamp - peek_frame->timestamp;
		}
		const uint64_t old_duration = (uint64_t)(duration * 100.0 / context->speed_percent);
		const uint64_t new_duration = (uint64_t)(duration * 100.0 / new_speed);
		context->start_timestamp += old_duration - new_duration;
	}
	if (context->blend_worker)
		replay_blend_worker_flush(context->blend_worker);
	context->speed_percent = new_speed;
	replay_update_text(context);
}

static void replay_faster_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, c->speed_percent * 3.0f / 2.0f);
}

static void replay_slower_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, c->speed_percent * 2.0f / 3.0f);
}

static void replay_faster_by_5_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, c->speed_percent + 5.0f);
}

static void replay_slower_by_5_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	if (c->speed_percent > 5.0f)
		update_speed(c, c->speed_percent - 5.0f);
}

static void replay_normal_or_faster_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	if (c->speed_percent < 100.0f) {
		update_speed(c, 100.0f);
	} else {
		update_speed(c, c->speed_percent * 3.0f / 2.0f);
	}
}

static void replay_normal_or_slower_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	if (c->speed_percent > 100.0f) {
		update_speed(c, 100.0f);
	} else {
		update_speed(c, c->speed_percent * 2.0f / 3.0f);
	}
}

static void replay_normal_speed_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, 100.0f);
}

static void replay_half_speed_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, 50.0f);
}

static void replay_double_speed_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	update_speed(c, 200.0f);
}

static void replay_forward_or_faster_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	if (c->backward) {
		replay_forward_hotkey(data, id, hotkey, pressed);
		return;
	}
	replay_normal_or_faster_hotkey(data, id, hotkey, pressed);
}

static void replay_backward_or_faster_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;
	if (!c->backward) {
		replay_backward_hotkey(data, id, hotkey, pressed);
		return;
	}
	replay_normal_or_faster_hotkey(data, id, hotkey, pressed);
}

static void replay_trim_front_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	const uint64_t timestamp = c->pause_timestamp == 0 ? obs_get_video_frame_time() : c->pause_timestamp;
	int64_t duration = timestamp - c->start_timestamp;
	if (c->speed_percent != 100.0f) {
		duration = (int64_t)(duration * c->speed_percent / 100.0);
	}
	if (c->backward) {
		duration = (c->current_replay.last_frame_timestamp - c->current_replay.first_frame_timestamp) - duration;
	}
	const int64_t min_front = c->current_replay.has_align ? c->current_replay.align_front : 0;
	if (duration < min_front)
		duration = min_front;

	const int64_t align_end = c->current_replay.has_align ? c->current_replay.align_end : 0;
	const int64_t max_front = (int64_t)(c->current_replay.duration - align_end);
	if (duration > max_front)
		duration = max_front;

	if (duration + c->current_replay.first_frame_timestamp <
	    c->current_replay.last_frame_timestamp - c->current_replay.trim_end) {
		c->current_replay.trim_front = duration;
		struct replay *r = circlebuf_data(&c->replays, c->replay_position * sizeof c->current_replay);
		if (r)
			r->trim_front = duration;
	}
}

static void replay_trim_end_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	const uint64_t timestamp = c->pause_timestamp == 0 ? obs_get_video_frame_time() : c->pause_timestamp;
	if (timestamp > c->start_timestamp) {
		int64_t duration = timestamp - c->start_timestamp;
		if (c->speed_percent != 100.0f) {
			duration = (int64_t)(duration * c->speed_percent / 100.0);
		}
		if (!c->backward) {
			duration = (c->current_replay.last_frame_timestamp - c->current_replay.first_frame_timestamp) - duration;
		}
		const int64_t min_end = c->current_replay.has_align ? c->current_replay.align_end : 0;
		if (duration < min_end)
			duration = min_end;

		const int64_t align_front = c->current_replay.has_align ? c->current_replay.align_front : 0;
		const int64_t max_end = (int64_t)(c->current_replay.duration - align_front);
		if (duration > max_end)
			duration = max_end;

		if (c->current_replay.first_frame_timestamp + c->current_replay.trim_front <
		    c->current_replay.last_frame_timestamp - duration) {
			c->current_replay.trim_end = duration;
			struct replay *r = circlebuf_data(&c->replays, c->replay_position * sizeof c->current_replay);
			if (r)
				r->trim_end = duration;
		}
	}
}

static void replay_trim_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	struct replay_source *c = data;

	if (!pressed)
		return;

	replay_source_mark_reset(c);
}

void update_filter_settings(obs_source_t *filter, obs_data_t *settings)
{
	obs_data_t *filter_settings = obs_source_get_settings(filter);
	if (!filter_settings)
		return;
	bool changed = false;
	if (obs_data_get_int(filter_settings, SETTING_DURATION) != obs_data_get_int(settings, SETTING_DURATION)) {
		obs_data_set_int(filter_settings, SETTING_DURATION, obs_data_get_int(settings, SETTING_DURATION));
		changed = true;
	}
	if (obs_data_get_bool(filter_settings, SETTING_SOUND_TRIGGER) != obs_data_get_bool(settings, SETTING_SOUND_TRIGGER)) {
		obs_data_set_bool(filter_settings, SETTING_SOUND_TRIGGER, obs_data_get_bool(settings, SETTING_SOUND_TRIGGER));
		changed = true;
	}
	if (obs_data_get_double(filter_settings, SETTING_AUDIO_THRESHOLD) !=
	    obs_data_get_double(settings, SETTING_AUDIO_THRESHOLD)) {
		obs_data_set_double(filter_settings, SETTING_AUDIO_THRESHOLD,
				    obs_data_get_double(settings, SETTING_AUDIO_THRESHOLD));
		changed = true;
	}
	if (obs_data_get_int(filter_settings, SETTING_STORAGE_FORMAT) !=
	    obs_data_get_int(settings, SETTING_STORAGE_FORMAT)) {
		obs_data_set_int(filter_settings, SETTING_STORAGE_FORMAT,
				 obs_data_get_int(settings, SETTING_STORAGE_FORMAT));
		changed = true;
	}
	if (obs_data_get_int(filter_settings, SETTING_DOWNSCALE) != obs_data_get_int(settings, SETTING_DOWNSCALE)) {
		obs_data_set_int(filter_settings, SETTING_DOWNSCALE, obs_data_get_int(settings, SETTING_DOWNSCALE));
		changed = true;
	}
	if (obs_data_get_int(filter_settings, SETTING_RESERVE_MODE) !=
	    obs_data_get_int(settings, SETTING_RESERVE_MODE)) {
		obs_data_set_int(filter_settings, SETTING_RESERVE_MODE,
				 obs_data_get_int(settings, SETTING_RESERVE_MODE));
		changed = true;
	}
	if (obs_data_get_int(filter_settings, SETTING_REPLAYS) != obs_data_get_int(settings, SETTING_REPLAYS)) {
		obs_data_set_int(filter_settings, SETTING_REPLAYS, obs_data_get_int(settings, SETTING_REPLAYS));
		changed = true;
	}
	obs_data_release(filter_settings);
	if (changed)
		obs_source_update(filter, NULL);
}

bool replay_source_execute_action(struct replay_source *context, const char *action, bool from_update)
{
	if (!action || !*action)
		return false;

	if (strcmp(action, "Load") == 0) {
		if (context)
			replay_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Next") == 0) {
		if (context)
			replay_next_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Previous") == 0) {
		if (context)
			replay_previous_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "First") == 0) {
		if (context)
			replay_first_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Last") == 0) {
		if (context)
			replay_last_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Remove") == 0) {
		if (context)
			replay_remove_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Clear") == 0) {
		if (context)
			replay_clear_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Save") == 0) {
		if (context)
			replay_save_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Restart") == 0) {
		if (context)
			replay_restart_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Pause") == 0) {
		if (context)
			replay_pause_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Faster") == 0) {
		if (context)
			replay_faster_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Slower") == 0) {
		if (context)
			replay_slower_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Faster5Percent") == 0) {
		if (context)
			replay_faster_by_5_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Slower5Percent") == 0) {
		if (context)
			replay_slower_by_5_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "NormalOrFaster") == 0) {
		if (context)
			replay_normal_or_faster_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "NormalOrSlower") == 0) {
		if (context)
			replay_normal_or_slower_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "NormalSpeed") == 0) {
		if (context)
			replay_normal_speed_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "HalfSpeed") == 0) {
		if (context)
			replay_half_speed_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "DoubleSpeed") == 0) {
		if (context)
			replay_double_speed_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "TrimFront") == 0) {
		if (context)
			replay_trim_front_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "TrimEnd") == 0) {
		if (context)
			replay_trim_end_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "TrimReset") == 0) {
		if (context)
			replay_trim_reset_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "MarkIn") == 0) {
		if (context) {
			uint64_t abs_ts = 0;
			if (replay_source_get_current_playback_time(context, &abs_ts))
				replay_source_set_mark_in(context, abs_ts);
		}
	} else if (strcmp(action, "MarkOut") == 0) {
		if (context) {
			uint64_t abs_ts = 0;
			if (replay_source_get_current_playback_time(context, &abs_ts))
				replay_source_set_mark_out(context, abs_ts);
		}
	} else if (strcmp(action, "MarkReset") == 0) {
		if (context)
			replay_source_mark_reset(context);
	} else if (strcmp(action, "Reverse") == 0) {
		if (context)
			replay_reverse_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Backward") == 0) {
		if (context)
			replay_backward_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "Forward") == 0) {
		if (context)
			replay_forward_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "ForwardOrFaster") == 0) {
		if (context)
			replay_forward_or_faster_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "BackwardOrFaster") == 0) {
		if (context)
			replay_backward_or_faster_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "DisableNextScene") == 0) {
		if (context)
			context->next_scene_disabled = true;
	} else if (strcmp(action, "EnableNextScene") == 0) {
		if (context)
			context->next_scene_disabled = false;
	} else if (strcmp(action, "Disable") == 0) {
		if (context) {
			if (from_update)
				context->disabled = true;
			else
				replay_disable_hotkey(context, 0, NULL, true);
		}
	} else if (strcmp(action, "Enable") == 0) {
		if (context) {
			if (from_update)
				context->disabled = false;
			else
				replay_enable_hotkey(context, 0, NULL, true);
		}
	} else if (strcmp(action, "SetNextSceneToCurrent") == 0) {
		if (context)
			replay_next_scene_current_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "SwitchToNextScene") == 0) {
		if (context)
			replay_next_scene_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "NextFrame") == 0) {
		if (context)
			replay_next_frame_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "PrevFrame") == 0) {
		if (context)
			replay_prev_frame_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "NextNFrames") == 0) {
		if (context)
			replay_next_n_frames_hotkey(context, 0, NULL, true);
	} else if (strcmp(action, "PrevNFrames") == 0) {
		if (context)
			replay_prev_n_frames_hotkey(context, 0, NULL, true);
	} else {
		return false;
	}

	return true;
}

static void replay_source_update(void *data, obs_data_t *settings)
{
	struct replay_source *context = data;
	const char *execute_action = obs_data_get_string(settings, SETTING_EXECUTE_ACTION);
	if (execute_action && strlen(execute_action)) {
		replay_source_execute_action(context, execute_action, true);
		obs_data_erase(settings, SETTING_EXECUTE_ACTION);
	}
	const char *source_name = obs_data_get_string(settings, SETTING_SOURCE);
	if (context->source_name) {
		if (strcmp(context->source_name, source_name) != 0 || context->disabled) {
			obs_source_t *s = obs_get_source_by_name(context->source_name);
			if (s) {
				do {
					context->source_filter = NULL;
					obs_source_enum_filters(s, EnumFilter, data);
					if (context->source_filter) {
						obs_source_filter_remove(s, context->source_filter);
					}
				} while (context->source_filter);
				obs_source_release(s);
			}
			if (strcmp(context->source_name, source_name) != 0) {
				bfree(context->source_name);
				context->source_name = bstrdup(source_name);
			}
		}
	} else {
		context->source_name = bstrdup(source_name);
	}
	const char *source_audio_name = obs_data_get_string(settings, SETTING_SOURCE_AUDIO);
	if (context->source_audio_name) {
		if (strcmp(context->source_audio_name, source_audio_name) != 0 || context->disabled) {
			obs_source_t *s = obs_get_source_by_name(context->source_audio_name);
			if (s) {
				do {
					context->source_audio_filter = NULL;
					obs_source_enum_filters(s, EnumAudioFilter, data);
					if (context->source_audio_filter) {
						obs_source_filter_remove(s, context->source_audio_filter);
					}
				} while (context->source_audio_filter);
				obs_source_release(s);
			}
			if (strcmp(context->source_audio_name, source_audio_name) != 0) {
				bfree(context->source_audio_name);
				context->source_audio_name = bstrdup(source_audio_name);
			}
		}
	} else {
		context->source_audio_name = bstrdup(source_audio_name);
	}
	const char *next_scene_name = obs_data_get_string(settings, SETTING_NEXT_SCENE);
	if (context->next_scene_name) {
		if (strcmp(context->next_scene_name, next_scene_name) != 0) {
			bfree(context->next_scene_name);
			context->next_scene_name = bstrdup(next_scene_name);
		}
	} else {
		context->next_scene_name = bstrdup(next_scene_name);
	}

	const char *load_switch_scene_name = obs_data_get_string(settings, SETTING_LOAD_SWITCH_SCENE);
	if (context->load_switch_scene_name) {
		if (strcmp(context->load_switch_scene_name, load_switch_scene_name) != 0) {
			bfree(context->load_switch_scene_name);
			context->load_switch_scene_name = bstrdup(load_switch_scene_name);
		}
	} else {
		context->load_switch_scene_name = bstrdup(load_switch_scene_name);
	}

	context->visibility_action = (int)obs_data_get_int(settings, SETTING_VISIBILITY_ACTION);
	context->end_action = (int)obs_data_get_int(settings, SETTING_END_ACTION);
	context->start_delay = obs_data_get_int(settings, SETTING_START_DELAY) * 1000000;
	context->retrieve_delay = obs_data_get_int(settings, SETTING_RETRIEVE_DELAY) * 1000000;
	context->frame_step_count = obs_data_get_int(settings, SETTING_FRAME_STEP_COUNT);

	context->replay_max = (int)obs_data_get_int(settings, SETTING_REPLAYS);
	replay_purge_replays(context);
	context->keep_buffer = obs_data_get_bool(settings, SETTING_KEEP_BUFFER);
	context->auto_release = (int)obs_data_get_int(settings, SETTING_AUTO_RELEASE);

	context->speed_percent = (float)obs_data_get_double(settings, SETTING_SPEED);
	if (context->speed_percent < SETTING_SPEED_MIN || context->speed_percent > SETTING_SPEED_MAX)
		context->speed_percent = 100.0f;

	context->backward_start = obs_data_get_bool(settings, SETTING_BACKWARD);
	if (context->backward != context->backward_start) {
		replay_reverse_hotkey(context, 0, NULL, true);
	}
	int old_blend = context->slowmo_blend;
	context->slowmo_blend = (int)obs_data_get_int(settings, SETTING_SLOWMO_BLEND);
	/* The worker lives until source destroy: other threads (hotkeys, vendor requests) may flush it unlocked.
	 * Switching off only drops pending blended frames so the normal output path continues in order. */
	if (old_blend != context->slowmo_blend && context->slowmo_blend == SLOWMO_BLEND_OFF)
		replay_blend_worker_flush(context->blend_worker);
	context->sound_trigger = obs_data_get_bool(settings, SETTING_SOUND_TRIGGER);
	if (!context->disabled) {

		obs_source_t *s = NULL;
		if (strcmp(context->source_name, obs_source_get_name(context->source)) != 0) {
			s = obs_get_source_by_name(context->source_name);
		}
		if (s) {
			context->source_filter = NULL;
			obs_source_enum_filters(s, EnumFilter, data);
			if (!context->source_filter) {
				if ((obs_source_get_output_flags(s) & OBS_SOURCE_ASYNC) == OBS_SOURCE_ASYNC) {
					context->source_filter = obs_source_create_private(
						REPLAY_FILTER_ASYNC_ID, obs_source_get_name(context->source), settings);
					blog(LOG_INFO, "[replay_source: '%s'] created async filter for '%s'",
					     obs_source_get_name(context->source), context->source_name);
				} else {
					context->source_filter = obs_source_create_private(
						REPLAY_FILTER_ID, obs_source_get_name(context->source), settings);
					blog(LOG_INFO, "[replay_source: '%s'] created filter for '%s'",
					     obs_source_get_name(context->source), context->source_name);
				}
				if (context->source_filter) {
					obs_source_filter_add(s, context->source_filter);
				}
			} else if (obs_obj_get_data(context->source_filter)) {
				update_filter_settings(context->source_filter, settings);
				blog(LOG_INFO, "[replay_source: '%s'] updated filter for '%s'",
				     obs_source_get_name(context->source), context->source_name);
			}
			if (obs_obj_get_data(context->source_filter)) {
				((struct replay_filter *)obs_obj_get_data(context->source_filter))->threshold_data = data;
				((struct replay_filter *)obs_obj_get_data(context->source_filter))->trigger_threshold =
					context->sound_trigger ? replay_trigger_threshold : NULL;
				context->filter_loaded = true;
				blog(LOG_INFO, "[replay_source: '%s'] connected to '%s'", obs_source_get_name(context->source),
				     context->source_name);
			}

			obs_source_release(s);
		}
		s = NULL;
		if (strcmp(context->source_audio_name, obs_source_get_name(context->source)) != 0) {
			s = obs_get_source_by_name(context->source_audio_name);
		}
		if (s) {
			context->source_audio_filter = NULL;
			obs_source_enum_filters(s, EnumAudioVideoFilter, data);
			if (!context->source_audio_filter) {
				if ((obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) != 0) {
					context->source_audio_filter = obs_source_create_private(
						REPLAY_FILTER_AUDIO_ID, obs_source_get_name(context->source), settings);
					blog(LOG_INFO, "[replay_source: '%s'] created audio filter for '%s'",
					     obs_source_get_name(context->source), context->source_audio_name);
				}
				if (context->source_audio_filter) {
					obs_source_filter_add(s, context->source_audio_filter);
				}
			} else if (obs_obj_get_data(context->source_audio_filter)) {
				update_filter_settings(context->source_audio_filter, settings);
				blog(LOG_INFO, "[replay_source: '%s'] updated audio filter for '%s'",
				     obs_source_get_name(context->source), context->source_audio_name);
			}
			if (obs_obj_get_data(context->source_audio_filter)) {
				((struct replay_filter *)obs_obj_get_data(context->source_audio_filter))->threshold_data = data;
				((struct replay_filter *)obs_obj_get_data(context->source_audio_filter))->trigger_threshold =
					context->sound_trigger ? replay_trigger_threshold : NULL;
				context->filter_loaded = true;
				blog(LOG_INFO, "[replay_source: '%s'] connected to '%s'", obs_source_get_name(context->source),
				     context->source_audio_name);
			}

			obs_source_release(s);
		}
	}
	const char *file_format = obs_data_get_string(settings, SETTING_FILE_FORMAT);
	if (context->file_format) {
		if (strcmp(context->file_format, file_format) != 0) {
			bfree(context->file_format);
			context->file_format = bstrdup(file_format);
		}
	} else {
		context->file_format = bstrdup(file_format);
	}
	const char *progress_source = obs_data_get_string(settings, SETTING_PROGRESS_SOURCE);
	if (context->progress_source_name) {
		if (strcmp(context->progress_source_name, progress_source) != 0) {
			bfree(context->progress_source_name);
			context->progress_source_name = bstrdup(progress_source);
		}
	} else {
		context->progress_source_name = bstrdup(progress_source);
	}

	const char *text_source = obs_data_get_string(settings, SETTING_TEXT_SOURCE);
	if (context->text_source_name) {
		if (strcmp(context->text_source_name, text_source) != 0) {
			bfree(context->text_source_name);
			context->text_source_name = bstrdup(text_source);
		}
	} else {
		context->text_source_name = bstrdup(text_source);
	}

	const char *text = obs_data_get_string(settings, SETTING_TEXT);
	if (context->text_format) {
		if (strcmp(context->text_format, text) != 0) {
			bfree(context->text_format);
			context->text_format = bstrdup(text);
		}
	} else {
		context->text_format = bstrdup(text);
	}

	context->lossless = obs_data_get_bool(settings, SETTING_LOSSLESS);
	const char *directory = obs_data_get_string(settings, SETTING_DIRECTORY);
	if (context->directory) {
		if (strcmp(context->directory, directory) != 0) {
			bfree(context->directory);
			context->directory = bstrdup(directory);
		}
	} else {
		context->directory = bstrdup(directory);
	}
	const char *save_encoder = obs_data_get_string(settings, SETTING_SAVE_ENCODER);
	if (!save_encoder || !*save_encoder)
		save_encoder = SAVE_ENCODER_AUTO;
	if (context->save_encoder) {
		if (strcmp(context->save_encoder, save_encoder) != 0) {
			bfree(context->save_encoder);
			context->save_encoder = bstrdup(save_encoder);
		}
	} else {
		context->save_encoder = bstrdup(save_encoder);
	}
	const char *save_container = obs_data_get_string(settings, SETTING_SAVE_CONTAINER);
	if (!save_container || !*save_container)
		save_container = SAVE_CONTAINER_MP4;
	if (context->save_container) {
		if (strcmp(context->save_container, save_container) != 0) {
			bfree(context->save_container);
			context->save_container = bstrdup(save_container);
		}
	} else {
		context->save_container = bstrdup(save_container);
	}
	int save_quality = (int)obs_data_get_int(settings, SETTING_SAVE_QUALITY);
	if (save_quality < SETTING_SAVE_QUALITY_MIN || save_quality > SETTING_SAVE_QUALITY_MAX)
		save_quality = SETTING_SAVE_QUALITY_DEFAULT;
	context->save_quality = save_quality;
	context->auto_save = (int)obs_data_get_int(settings, SETTING_AUTO_SAVE);
	replay_update_text(context);
}

struct replay_source_hotkey_def {
	const char *legacy_name;
	const char *locale_key;
	obs_hotkey_func func;
	size_t field_offset;
};

static const struct replay_source_hotkey_def g_source_hotkey_defs[] = {
	{"ReplaySource.Replay", "LoadReplay", replay_hotkey, offsetof(struct replay_source, replay_hotkey)},
	{"ReplaySource.Next", "Next", replay_next_hotkey, offsetof(struct replay_source, next_hotkey)},
	{"ReplaySource.Previous", "Previous", replay_previous_hotkey, offsetof(struct replay_source, previous_hotkey)},
	{"ReplaySource.First", "First", replay_first_hotkey, offsetof(struct replay_source, first_hotkey)},
	{"ReplaySource.Last", "Last", replay_last_hotkey, offsetof(struct replay_source, last_hotkey)},
	{"ReplaySource.Remove", "Remove", replay_remove_hotkey, offsetof(struct replay_source, remove_hotkey)},
	{"ReplaySource.Clear", "Clear", replay_clear_hotkey, offsetof(struct replay_source, clear_hotkey)},
	{"ReplaySource.Save", "SaveReplay", replay_save_hotkey, offsetof(struct replay_source, save_hotkey)},
	{"ReplaySource.Restart", "Restart", replay_restart_hotkey, offsetof(struct replay_source, restart_hotkey)},
	{"ReplaySource.Pause", "Pause", replay_pause_hotkey, offsetof(struct replay_source, pause_hotkey)},
	{"ReplaySource.Faster", "Faster", replay_faster_hotkey, offsetof(struct replay_source, faster_hotkey)},
	{"ReplaySource.Slower", "Slower", replay_slower_hotkey, offsetof(struct replay_source, slower_hotkey)},
	{"ReplaySource.FasterBy5", "Faster5Percent", replay_faster_by_5_hotkey,
	 offsetof(struct replay_source, faster_by_5_hotkey)},
	{"ReplaySource.SlowerBy5", "Slower5Percent", replay_slower_by_5_hotkey,
	 offsetof(struct replay_source, slower_by_5_hotkey)},
	{"ReplaySource.NormalOrFaster", "NormalOrFaster", replay_normal_or_faster_hotkey,
	 offsetof(struct replay_source, normal_or_faster_hotkey)},
	{"ReplaySource.NormalOrSlower", "NormalOrSlower", replay_normal_or_slower_hotkey,
	 offsetof(struct replay_source, normal_or_slower_hotkey)},
	{"ReplaySource.NormalSpeed", "NormalSpeed", replay_normal_speed_hotkey,
	 offsetof(struct replay_source, normal_speed_hotkey)},
	{"ReplaySource.HalfSpeed", "HalfSpeed", replay_half_speed_hotkey,
	 offsetof(struct replay_source, half_speed_hotkey)},
	{"ReplaySource.DoubleSpeed", "DoubleSpeed", replay_double_speed_hotkey,
	 offsetof(struct replay_source, double_speed_hotkey)},
	{"ReplaySource.TrimFront", "TrimFront", replay_trim_front_hotkey,
	 offsetof(struct replay_source, trim_front_hotkey)},
	{"ReplaySource.TrimEnd", "TrimEnd", replay_trim_end_hotkey, offsetof(struct replay_source, trim_end_hotkey)},
	{"ReplaySource.TrimReset", "TrimReset", replay_trim_reset_hotkey,
	 offsetof(struct replay_source, trim_reset_hotkey)},
	{"ReplaySource.Reverse", "Reverse", replay_reverse_hotkey, offsetof(struct replay_source, reverse_hotkey)},
	{"ReplaySource.Forward", "Forward", replay_forward_hotkey, offsetof(struct replay_source, forward_hotkey)},
	{"ReplaySource.Backward", "Backward", replay_backward_hotkey, offsetof(struct replay_source, backward_hotkey)},
	{"ReplaySource.ForwardOrFaster", "ForwardOrFaster", replay_forward_or_faster_hotkey,
	 offsetof(struct replay_source, forward_or_faster_hotkey)},
	{"ReplaySource.BackwardOrFaster", "BackwardOrFaster", replay_backward_or_faster_hotkey,
	 offsetof(struct replay_source, backward_or_faster_hotkey)},
	{"ReplaySource.Disable", "Disable", replay_disable_hotkey, offsetof(struct replay_source, disable_hotkey)},
	{"ReplaySource.Enable", "Enable", replay_enable_hotkey, offsetof(struct replay_source, enable_hotkey)},
	{"ReplaySource.DisableNextScene", "DisableNextScene", replay_disable_next_scene_hotkey,
	 offsetof(struct replay_source, disable_next_scene_hotkey)},
	{"ReplaySource.EnableNextScene", "EnableNextScene", replay_enable_next_scene_hotkey,
	 offsetof(struct replay_source, enable_next_scene_hotkey)},
	{"ReplaySource.NextSceneCurrent", "SetNextSceneToCurrent", replay_next_scene_current_hotkey,
	 offsetof(struct replay_source, next_scene_current_hotkey)},
	{"ReplaySource.NextScene", "SwitchToNextScene", replay_next_scene_hotkey,
	 offsetof(struct replay_source, next_scene_hotkey)},
	{"ReplaySource.PrevFrame", "PrevFrame", replay_prev_frame_hotkey,
	 offsetof(struct replay_source, prev_frame_hotkey)},
	{"ReplaySource.NextFrame", "NextFrame", replay_next_frame_hotkey,
	 offsetof(struct replay_source, next_frame_hotkey)},
	{"ReplaySource.PrevNFrames", "PrevNFrames", replay_prev_n_frames_hotkey,
	 offsetof(struct replay_source, prev_n_frames_hotkey)},
	{"ReplaySource.NextNFrames", "NextNFrames", replay_next_n_frames_hotkey,
	 offsetof(struct replay_source, next_n_frames_hotkey)},
};

#define REPLAY_SOURCE_HOTKEY_COUNT (sizeof(g_source_hotkey_defs) / sizeof(g_source_hotkey_defs[0]))

static inline obs_hotkey_id *replay_source_hotkey_slot(struct replay_source *context, size_t offset)
{
	return (obs_hotkey_id *)((char *)context + offset);
}

static void format_source_hotkey_name(struct dstr *out, const char *legacy_name, const char *source_name)
{
	if (source_name && *source_name)
		dstr_printf(out, "%s.%s", legacy_name, source_name);
	else
		dstr_copy(out, legacy_name);
}

struct hotkey_binding_check {
	obs_hotkey_id id;
	bool found;
};

static bool check_hotkey_binding_cb(void *data, size_t idx, obs_hotkey_binding_t *binding)
{
	UNUSED_PARAMETER(idx);
	struct hotkey_binding_check *check = data;
	if (obs_hotkey_binding_get_hotkey_id(binding) == check->id) {
		check->found = true;
		return false;
	}
	return true;
}

static bool replay_hotkey_has_bindings(obs_hotkey_id id)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return false;
	struct hotkey_binding_check check = {id, false};
	obs_enum_hotkey_bindings(check_hotkey_binding_cb, &check);
	return check.found;
}

static void replay_source_rename_cb(void *data, calldata_t *cd)
{
	struct replay_source *context = data;
	if (!context || !context->source)
		return;
	const char *new_name = calldata_string(cd, "new_name");
	if (!new_name)
		new_name = obs_source_get_name(context->source);

	struct dstr hotkey_name;
	dstr_init(&hotkey_name);
	for (size_t i = 0; i < REPLAY_SOURCE_HOTKEY_COUNT; i++) {
		const struct replay_source_hotkey_def *def = &g_source_hotkey_defs[i];
		obs_hotkey_id id = *replay_source_hotkey_slot(context, def->field_offset);
		if (id == OBS_INVALID_HOTKEY_ID)
			continue;
		format_source_hotkey_name(&hotkey_name, def->legacy_name, new_name);
		obs_hotkey_set_name(id, hotkey_name.array);
	}
	dstr_free(&hotkey_name);
}

static void *replay_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct replay_source *context = bzalloc(sizeof(struct replay_source));
	context->source = source;
	context->keep_buffer = true;
	context->auto_release = AUTO_RELEASE_NEVER;
	context->auto_save = AUTO_SAVE_OFF;
	context->destroying = false;
	context->save_quality = SETTING_SAVE_QUALITY_DEFAULT;
	obs_source_set_async_unbuffered(source, true);
	pthread_mutex_init(&context->video_mutex, NULL);
	pthread_mutex_init(&context->audio_mutex, NULL);
	pthread_mutex_init(&context->replay_mutex, NULL);

	circlebuf_init(&context->replays);
	context->speed_percent = 100.0f;
	context->replay_max = 1;

	const char *src_name = obs_source_get_name(source);
	struct dstr hotkey_name;
	dstr_init(&hotkey_name);
	for (size_t i = 0; i < REPLAY_SOURCE_HOTKEY_COUNT; i++) {
		const struct replay_source_hotkey_def *def = &g_source_hotkey_defs[i];
		obs_hotkey_id *slot = replay_source_hotkey_slot(context, def->field_offset);
		const char *desc = obs_module_text(def->locale_key);
		format_source_hotkey_name(&hotkey_name, def->legacy_name, src_name);

		obs_hotkey_id new_id = obs_hotkey_register_source(source, hotkey_name.array, desc, def->func, context);
		*slot = new_id;

		if (new_id != OBS_INVALID_HOTKEY_ID && src_name && *src_name && !replay_hotkey_has_bindings(new_id)) {
			obs_hotkey_id tmp_id =
				obs_hotkey_register_source(source, def->legacy_name, desc, def->func, context);
			if (tmp_id != OBS_INVALID_HOTKEY_ID) {
				if (replay_hotkey_has_bindings(tmp_id)) {
					obs_data_array_t *saved = obs_hotkey_save(tmp_id);
					if (saved) {
						obs_hotkey_load(new_id, saved);
						obs_data_array_release(saved);
					}
				}
				obs_hotkey_unregister(tmp_id);
			}
		}
	}
	dstr_free(&hotkey_name);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	if (sh)
		signal_handler_connect(sh, "rename", replay_source_rename_cb, context);
	return context;
}

static void replay_source_destroy(void *data)
{
	struct replay_source *context = data;
	context->destroying = true;

	if (context->source) {
		signal_handler_t *sh = obs_source_get_signal_handler(context->source);
		if (sh)
			signal_handler_disconnect(sh, "rename", replay_source_rename_cb, context);
	}
	pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->audio_mutex);
	context->current_replay.video_frame_count = 0;
	context->current_replay.video_frames = NULL;
	context->current_replay.audio_frame_count = 0;
	context->current_replay.audio_frames = NULL;
	pthread_mutex_unlock(&context->video_mutex);
	pthread_mutex_unlock(&context->audio_mutex);

	if (context->source_name)
		bfree(context->source_name);

	if (context->source_audio_name)
		bfree(context->source_audio_name);

	if (context->next_scene_name)
		bfree(context->next_scene_name);

	if (context->load_switch_scene_name)
		bfree(context->load_switch_scene_name);

	if (context->directory)
		bfree(context->directory);

	if (context->file_format)
		bfree(context->file_format);

	if (context->save_encoder)
		bfree(context->save_encoder);

	if (context->save_container)
		bfree(context->save_container);

	if (context->progress_source_name)
		bfree(context->progress_source_name);

	if (context->text_source_name)
		bfree(context->text_source_name);

	if (context->text_format)
		bfree(context->text_format);

	pthread_mutex_lock(&context->replay_mutex);
	while (context->replays.size) {
		circlebuf_pop_front(&context->replays, &context->current_replay, sizeof context->current_replay);
		replay_free_replay(&context->current_replay, context);
	}
	circlebuf_free(&context->replays);
	pthread_mutex_unlock(&context->replay_mutex);
	if (context->blend_worker) {
		replay_blend_worker_destroy(context->blend_worker);
		context->blend_worker = NULL;
	}

	pthread_mutex_destroy(&context->video_mutex);
	pthread_mutex_destroy(&context->audio_mutex);
	pthread_mutex_destroy(&context->replay_mutex);
	bfree(context);
}

static void replay_output_frame(struct replay_source *context, struct obs_source_frame *frame)
{
	uint64_t t = frame->timestamp;
	if (t < context->current_replay.first_frame_timestamp || t > context->current_replay.last_frame_timestamp)
		return;
	struct obs_source_frame out = *frame;
	if (context->backward) {
		out.timestamp = context->current_replay.last_frame_timestamp - t;
	} else {
		out.timestamp = t - context->current_replay.first_frame_timestamp;
	}
	if (context->speed_percent != 100.0f) {
		out.timestamp = (uint64_t)(out.timestamp * 100.0 / context->speed_percent);
	}
	out.timestamp += context->start_timestamp;
	if (context->previous_frame_timestamp <= out.timestamp) {
		context->previous_frame_timestamp = out.timestamp;
		obs_source_output_video(context->source, &out);
	}
	replay_update_progress_crop(context, t);
}

void replay_source_end_action(struct replay_source *context)
{
	const int replay_count = (int)(context->replays.size / sizeof context->current_replay);
	if (replay_count == 0) {
		context->play = false;
		context->end = true;
		obs_source_media_ended(context->source);
		return;
	}
	bool finish = false;
	if (context->end_action == END_ACTION_HIDE || context->end_action == END_ACTION_PAUSE ||
	    (context->end_action == END_ACTION_HIDE_ALL && replay_count == 1) ||
	    (context->end_action == END_ACTION_PAUSE_ALL && replay_count == 1)) {
		context->play = false;
		context->end = true;
		finish = true;
		obs_source_media_ended(context->source);
	} else if (context->end_action == END_ACTION_REVERSE ||
		   (context->end_action == END_ACTION_REVERSE_ALL && replay_count == 1)) {
		replay_reverse_hotkey(context, 0, NULL, true);
	} else if (context->end_action == END_ACTION_LOOP || (context->end_action == END_ACTION_LOOP_ALL && replay_count == 1)) {
		context->restart = true;
	} else {
		if (context->backward) {
			if (context->replay_position <= 0) {
				context->replay_position = replay_count - 1;
				finish = true;
			} else {
				context->replay_position--;
				replay_update_position(context, false);
			}
		} else {
			if (context->replay_position + 1 >= replay_count) {
				context->replay_position = 0;
				finish = true;
			} else {
				context->replay_position++;
				replay_update_position(context, false);
			}
		}
		if (finish) {
			if (context->end_action == END_ACTION_REVERSE_ALL) {
				replay_reverse_hotkey(context, 0, NULL, true);
				finish = false;
			} else if (context->end_action == END_ACTION_LOOP_ALL) {
				replay_update_position(context, false);
				finish = false;
			} else {
				context->play = false;
				context->end = true;
				obs_source_media_ended(context->source);
			}
		}
	}
	if (finish && context->next_scene_name && context->active && !context->next_scene_disabled) {
		obs_source_t *current = obs_frontend_get_current_scene();
		if (current) {
			const char *current_name = obs_source_get_name(current);
			if (strcmp(current_name, context->next_scene_name) != 0) {
				obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
				if (s) {
					obs_source_release(s);
					pthread_t thread;
					pthread_create(&thread, NULL, update_scene_thread, s);
				}
			}
			obs_source_release(current);
		} else {
			obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
			if (s) {
				obs_source_release(s);
				pthread_t thread;
				pthread_create(&thread, NULL, update_scene_thread, s);
			}
		}
	}
}

static void replay_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct replay_source *context = data;

	const uint64_t os_timestamp = obs_get_video_frame_time();

	if (context->retrieve_timestamp && context->retrieve_timestamp < os_timestamp) {
		context->retrieve_timestamp = 0;
		replay_retrieve(context);
	}
	if (!context->filter_loaded) {
		if (context->source_name) {
			obs_source_t *s = obs_get_source_by_name(context->source_name);
			if (s) {
				obs_data_t *settings = obs_source_get_settings(context->source);
				replay_source_update(data, settings);
				obs_data_release(settings);
				obs_source_release(s);
				return;
			}
		}
		if (context->source_audio_name) {
			obs_source_t *s = obs_get_source_by_name(context->source_audio_name);
			if (s) {
				obs_data_t *settings = obs_source_get_settings(context->source);
				replay_source_update(data, settings);
				obs_data_release(settings);
				obs_source_release(s);
				return;
			}
		}
		return;
	}

	if (context->saving_status == SAVING_STATUS_STARTING) {
		replay_save(context);
	}

	pthread_mutex_lock(&context->video_mutex);
	if (!context->current_replay.video_frame_count && !context->current_replay.audio_frame_count) {
		if (context->play) {
			context->play = false;
			obs_source_signal(context->source, "media_pause");
		}
	} else if (context->disabled) {
		context->play = false;
		context->end = true;
		obs_source_media_ended(context->source);
	}
	if (!context->play) {
		if (context->end && (context->end_action == END_ACTION_HIDE || context->end_action == END_ACTION_HIDE_ALL)) {
			obs_source_output_video(context->source, NULL);
			if (context->auto_release != AUTO_RELEASE_NEVER && context->replays.size) {
				replay_auto_release(context, false);
			}
		}
		if (context->stepped && context->current_replay.video_frame_count) {
			context->stepped = false;
			struct obs_source_frame *frame = context->current_replay.video_frames[context->video_frame_position];
			struct obs_source_frame out = *frame;
			out.timestamp = os_timestamp;
			obs_source_output_video(context->source, &out);
			replay_update_text(context);
		}
		pthread_mutex_unlock(&context->video_mutex);
		return;
	}
	if (context->end) {
		context->end = false;
		obs_source_media_started(context->source);
	}
	replay_update_text(context);

	if (context->current_replay.video_frame_count) {
		if (context->video_frame_position >= context->current_replay.video_frame_count) {
			context->video_frame_position = context->current_replay.video_frame_count - 1;
		}
		struct obs_source_frame *frame = context->current_replay.video_frames[context->video_frame_position];
		if (context->backward) {
			if (context->restart) {
				frame = replay_restart_at_end(context, os_timestamp);
				if (!frame) {
					pthread_mutex_unlock(&context->video_mutex);
					return;
				}
			}

			bool do_blend = (context->slowmo_blend == SLOWMO_BLEND_CROSSFADE) &&
					(context->speed_percent < 100.0f);
			if (do_blend) {
				const uint64_t first_ts = context->current_replay.first_frame_timestamp;
				const uint64_t last_ts = context->current_replay.last_frame_timestamp;
				const int64_t trim_front = context->current_replay.trim_front;

				while (context->play && context->video_frame_position > 0) {
					struct obs_source_frame *next_f =
						context->current_replay.video_frames[context->video_frame_position - 1];
					uint64_t next_t_out = context->start_timestamp +
						(uint64_t)((last_ts - next_f->timestamp) * 100.0 / context->speed_percent);
					if (os_timestamp < next_t_out)
						break;

					struct obs_source_frame *cur_f =
						context->current_replay.video_frames[context->video_frame_position];
					if (cur_f->timestamp <= first_ts + trim_front) {
						replay_source_end_action(context);
						break;
					}
					context->video_frame_position--;
				}

				struct obs_source_frame *frame_a =
					context->current_replay.video_frames[context->video_frame_position];
				struct obs_source_frame *frame_b = frame_a;
				uint32_t w8 = 0;

				uint64_t t_a = context->start_timestamp +
					(uint64_t)((last_ts - frame_a->timestamp) * 100.0 / context->speed_percent);

				if (context->video_frame_position > 0) {
					frame_b = context->current_replay.video_frames[context->video_frame_position - 1];
					uint64_t t_b = context->start_timestamp +
						(uint64_t)((last_ts - frame_b->timestamp) * 100.0 / context->speed_percent);
					if (t_b > t_a && os_timestamp >= t_a) {
						double w = (double)(os_timestamp - t_a) / (double)(t_b - t_a);
						if (w < 0.0)
							w = 0.0;
						if (w > 1.0)
							w = 1.0;
						w8 = (uint32_t)llround(w * 256.0);
						if (w8 > 256)
							w8 = 256;
					}
				}

				if (os_timestamp >= context->previous_frame_timestamp) {
					context->previous_frame_timestamp = os_timestamp;
					if (!context->blend_worker)
						context->blend_worker = replay_blend_worker_create(context->source);
					replay_blend_worker_push(context->blend_worker, frame_a, frame_b, w8, os_timestamp);
					replay_update_progress_crop(context, frame_a->timestamp);
				}

				if (context->video_frame_position == 0 ||
				    frame_a->timestamp <= first_ts + trim_front) {
					replay_source_end_action(context);
				}
			} else {
				const int64_t video_duration = (int64_t)os_timestamp - (int64_t)context->start_timestamp;
				//TODO audio backwards
				int64_t source_duration = (int64_t)((context->current_replay.last_frame_timestamp - frame->timestamp) *
								    100.0 / context->speed_percent);

				struct obs_source_frame *output_frame = frame;
				while (context->play && video_duration >= source_duration) {
					output_frame = frame;
					if (frame->timestamp <=
					    context->current_replay.first_frame_timestamp + context->current_replay.trim_front) {
						replay_source_end_action(context);
						break;
					}
					if (context->video_frame_position == 0) {
						replay_source_end_action(context);
						break;
					}
					context->video_frame_position--;

					frame = context->current_replay.video_frames[context->video_frame_position];

					source_duration = (int64_t)((context->current_replay.last_frame_timestamp - frame->timestamp) *
								    100.0 / context->speed_percent);
				}
				replay_output_frame(context, output_frame);
				if (context->video_frame_position == 0) {
					replay_source_end_action(context);
				}
			}
		} else {
			if (context->restart) {
				frame = replay_restart_at_begin(context, os_timestamp);
				if (!frame) {
					pthread_mutex_unlock(&context->video_mutex);
					return;
				}
			}
			if (context->start_timestamp > os_timestamp) {
				pthread_mutex_unlock(&context->video_mutex);
				return;
			}
			const int64_t video_duration = (int64_t)os_timestamp - (int64_t)context->start_timestamp;

			if (context->current_replay.audio_frame_count > 1) {
				pthread_mutex_lock(&context->audio_mutex);
				struct obs_audio_data peek_audio =
					context->current_replay.audio_frames[context->audio_frame_position];
				const int64_t frame_duration = (context->current_replay.last_frame_timestamp -
								context->current_replay.first_frame_timestamp) /
							       context->current_replay.video_frame_count;
				//const uint64_t duration = audio_frames_to_ns(info.samples_per_sec, peek_audio.frames);
				int64_t audio_duration = (int64_t)(((int64_t)peek_audio.timestamp -
								    (int64_t)context->current_replay.first_frame_timestamp) *
								   100.0 / context->speed_percent);
				while (context->play && video_duration + frame_duration > audio_duration) {
					if (peek_audio.timestamp > context->current_replay.first_frame_timestamp - frame_duration &&
					    peek_audio.timestamp < context->current_replay.last_frame_timestamp + frame_duration) {
						context->audio.frames = peek_audio.frames;

						if (context->speed_percent != 100.0f) {
							context->audio.timestamp =
								(uint64_t)(context->start_timestamp +
									   (((int64_t)peek_audio.timestamp -
									     (int64_t)context->current_replay.first_frame_timestamp) *
									    100.0 / context->speed_percent));
							context->audio.samples_per_sec =
								(uint32_t)(context->current_replay.oai.samples_per_sec *
									   context->speed_percent / 100.0);
						} else {
							context->audio.timestamp = peek_audio.timestamp + context->start_timestamp -
										   context->current_replay.first_frame_timestamp;
							context->audio.samples_per_sec =
								context->current_replay.oai.samples_per_sec;
						}
						for (size_t i = 0; i < MAX_AV_PLANES; i++) {
							context->audio.data[i] = peek_audio.data[i];
						}

						context->audio.speakers = context->current_replay.oai.speakers;
						context->audio.format = context->current_replay.oai.format;

						obs_source_output_audio(context->source, &context->audio);
					}
					context->audio_frame_position++;
					if (context->audio_frame_position >= context->current_replay.audio_frame_count) {
						context->audio_frame_position = 0;
						break;
					}
					peek_audio = context->current_replay.audio_frames[context->audio_frame_position];
					audio_duration = (int64_t)(((int64_t)peek_audio.timestamp -
								    (int64_t)context->current_replay.first_frame_timestamp) *
								   100.0 / context->speed_percent);
				}
				pthread_mutex_unlock(&context->audio_mutex);
			}
			bool do_blend = (context->slowmo_blend == SLOWMO_BLEND_CROSSFADE) &&
					(context->speed_percent < 100.0f);
			if (do_blend) {
				const size_t count = (size_t)context->current_replay.video_frame_count;
				const uint64_t first_ts = context->current_replay.first_frame_timestamp;
				const uint64_t last_ts = context->current_replay.last_frame_timestamp;
				const int64_t trim_end = context->current_replay.trim_end;

				while (context->play && context->video_frame_position + 1 < count) {
					struct obs_source_frame *next_f =
						context->current_replay.video_frames[context->video_frame_position + 1];
					uint64_t next_t_out = context->start_timestamp +
						(uint64_t)((next_f->timestamp - first_ts) * 100.0 / context->speed_percent);
					if (os_timestamp < next_t_out)
						break;

					struct obs_source_frame *cur_f =
						context->current_replay.video_frames[context->video_frame_position];
					if (cur_f->timestamp >= last_ts - trim_end) {
						replay_source_end_action(context);
						break;
					}
					context->video_frame_position++;
				}

				struct obs_source_frame *frame_a =
					context->current_replay.video_frames[context->video_frame_position];
				struct obs_source_frame *frame_b = frame_a;
				uint32_t w8 = 0;

				uint64_t t_a = context->start_timestamp +
					(uint64_t)((frame_a->timestamp - first_ts) * 100.0 / context->speed_percent);

				if (context->video_frame_position + 1 < count) {
					frame_b = context->current_replay.video_frames[context->video_frame_position + 1];
					uint64_t t_b = context->start_timestamp +
						(uint64_t)((frame_b->timestamp - first_ts) * 100.0 / context->speed_percent);
					if (t_b > t_a && os_timestamp >= t_a) {
						double w = (double)(os_timestamp - t_a) / (double)(t_b - t_a);
						if (w < 0.0)
							w = 0.0;
						if (w > 1.0)
							w = 1.0;
						w8 = (uint32_t)llround(w * 256.0);
						if (w8 > 256)
							w8 = 256;
					}
				}

				if (os_timestamp >= context->previous_frame_timestamp) {
					context->previous_frame_timestamp = os_timestamp;
					if (!context->blend_worker)
						context->blend_worker = replay_blend_worker_create(context->source);
					replay_blend_worker_push(context->blend_worker, frame_a, frame_b, w8, os_timestamp);
					replay_update_progress_crop(context, frame_a->timestamp);
				}

				if (context->video_frame_position >= count - 1 ||
				    frame_a->timestamp >= last_ts - trim_end) {
					replay_source_end_action(context);
				}
			} else {
				int64_t source_duration = (int64_t)((frame->timestamp - context->current_replay.first_frame_timestamp) *
								    100.0 / context->speed_percent);
				struct obs_source_frame *output_frame = frame;
				while (context->play && video_duration >= source_duration) {
					output_frame = frame;
					if (frame->timestamp >=
					    context->current_replay.last_frame_timestamp - context->current_replay.trim_end) {
						replay_source_end_action(context);
						break;
					}
					context->video_frame_position++;
					if (context->video_frame_position >= context->current_replay.video_frame_count) {
						context->video_frame_position = context->current_replay.video_frame_count - 1;
						replay_source_end_action(context);
						break;
					}
					frame = context->current_replay.video_frames[context->video_frame_position];
					source_duration = (int64_t)((frame->timestamp - context->current_replay.first_frame_timestamp) *
								    100.0 / context->speed_percent);
				}
				replay_output_frame(context, output_frame);
				if (context->video_frame_position >= context->current_replay.video_frame_count - 1) {
					context->video_frame_position = context->current_replay.video_frame_count - 1;
					replay_source_end_action(context);
				}
			}
		}
	} else if (context->current_replay.audio_frame_count) {
		//no video, only audio
		struct obs_audio_data peek_audio = context->current_replay.audio_frames[context->audio_frame_position];

		if (context->current_replay.first_frame_timestamp == peek_audio.timestamp) {
			context->start_timestamp = os_timestamp;
			context->pause_timestamp = 0;
			context->restart = false;
		} else if (context->restart) {
			context->audio_frame_position = 0;
			peek_audio = context->current_replay.audio_frames[context->audio_frame_position];
			context->restart = false;
			context->start_timestamp = os_timestamp;
			context->pause_timestamp = 0;
		}
		if (context->start_timestamp == os_timestamp && context->current_replay.trim_front != 0) {
			if (context->speed_percent == 100.0f) {
				context->start_timestamp -= context->current_replay.trim_front;
			} else {
				context->start_timestamp -=
					(uint64_t)(context->current_replay.trim_front * 100.0 / context->speed_percent);
			}
			if (context->current_replay.trim_front < 0) {
				pthread_mutex_unlock(&context->video_mutex);
				return;
			}
			while (peek_audio.timestamp <
			       context->current_replay.first_frame_timestamp + context->current_replay.trim_front) {
				context->audio_frame_position++;
				if (context->audio_frame_position >= context->current_replay.audio_frame_count) {
					context->audio_frame_position = 0;
					break;
				}
				peek_audio = context->current_replay.audio_frames[context->audio_frame_position];
			}
		}
		if (context->start_timestamp > os_timestamp) {
			pthread_mutex_unlock(&context->video_mutex);
			return;
		}

		const int64_t video_duration = os_timestamp - context->start_timestamp;

		int64_t audio_duration =
			(int64_t)(((int64_t)peek_audio.timestamp - (int64_t)context->current_replay.first_frame_timestamp) * 100.0 /
				  context->speed_percent);

		while (context->play && context->current_replay.audio_frame_count > 1 && video_duration >= audio_duration) {
			if (peek_audio.timestamp >=
			    context->current_replay.last_frame_timestamp - context->current_replay.trim_end) {
				if (context->end_action != END_ACTION_LOOP) {
					context->play = false;
					context->end = true;
					obs_source_media_ended(context->source);
				} else if (context->current_replay.trim_end != 0) {
					context->restart = true;
				}
				if (context->next_scene_name && context->active) {
					obs_source_t *s = obs_get_source_by_name(context->next_scene_name);
					if (s) {
						obs_frontend_set_current_scene(s);
						obs_source_release(s);
					}
				}
			}

			context->audio.frames = peek_audio.frames;

			if (context->speed_percent != 100.0f) {
				context->audio.timestamp =
					(uint64_t)(context->start_timestamp +
						   (peek_audio.timestamp - context->current_replay.first_frame_timestamp) * 100.0 /
							   context->speed_percent);
				context->audio.samples_per_sec =
					(uint32_t)(context->current_replay.oai.samples_per_sec * context->speed_percent / 100.0);
			} else {
				context->audio.timestamp = peek_audio.timestamp + context->start_timestamp -
							   context->current_replay.first_frame_timestamp;
				context->audio.samples_per_sec = context->current_replay.oai.samples_per_sec;
			}
			for (size_t i = 0; i < MAX_AV_PLANES; i++) {
				context->audio.data[i] = peek_audio.data[i];
			}
			context->audio.speakers = context->current_replay.oai.speakers;
			context->audio.format = context->current_replay.oai.format;

			obs_source_output_audio(context->source, &context->audio);
			context->audio_frame_position++;
			if (context->audio_frame_position >= context->current_replay.audio_frame_count) {
				context->audio_frame_position = 0;
				break;
			}
			peek_audio = context->current_replay.audio_frames[context->audio_frame_position];
			audio_duration =
				(int64_t)(((int64_t)peek_audio.timestamp - (int64_t)context->current_replay.first_frame_timestamp) *
					  100.0 / context->speed_percent);
		}
	}
	pthread_mutex_unlock(&context->video_mutex);
}
static bool EnumVideoSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0)
		obs_property_list_add_string(prop, obs_source_get_name(source), obs_source_get_name(source));
	return true;
}
static bool EnumAudioSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0)
		obs_property_list_add_string(prop, obs_source_get_name(source), obs_source_get_name(source));
	return true;
}
static bool EnumScenes(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE)
		obs_property_list_add_string(prop, obs_source_get_name(source), obs_source_get_name(source));
	return true;
}
static bool replay_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct replay_source *s = data;
	replay_retrieve(s);
	return false; // no properties changed
}

static bool EnumTextSources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	const char *source_id = obs_source_get_unversioned_id(source);
	if (strcmp(source_id, "text_gdiplus") == 0 || strcmp(source_id, "text_ft2_source") == 0)
		obs_property_list_add_string(prop, obs_source_get_name(source), obs_source_get_name(source));
	return true;
}

static bool replay_video_source_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *data)
{
	UNUSED_PARAMETER(property);
	const char *source_name = obs_data_get_string(data, SETTING_SOURCE);
	bool async_source = false;
	if (source_name) {
		obs_source_t *s = obs_get_source_by_name(source_name);
		if (s && (obs_source_get_output_flags(s) & OBS_SOURCE_ASYNC) == OBS_SOURCE_ASYNC) {
			async_source = true;
		}
		obs_source_release(s);
	}
	obs_property_t *prop = obs_properties_get(props, SETTING_INTERNAL_FRAMES);
	obs_property_set_visible(prop, async_source);
	return true;
}

static bool replay_text_source_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *data)
{
	UNUSED_PARAMETER(property);
	const char *source_name = obs_data_get_string(data, SETTING_TEXT_SOURCE);
	bool text_source = false;
	if (source_name) {
		obs_source_t *s = obs_get_source_by_name(source_name);
		if (s) {
			text_source = true;
		}
		obs_source_release(s);
	}
	obs_property_t *prop = obs_properties_get(props, SETTING_TEXT);
	obs_property_set_visible(prop, text_source);
	return true;
}

static bool replay_sound_trigger_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *data)
{
	UNUSED_PARAMETER(property);
	const bool sound_trigger = obs_data_get_bool(data, SETTING_SOUND_TRIGGER);
	obs_property_t *prop = obs_properties_get(props, SETTING_AUDIO_THRESHOLD);
	obs_property_set_visible(prop, sound_trigger);
	return true;
}

static obs_properties_t *replay_source_properties(void *data)
{
	struct replay_source *s = data;
	UNUSED_PARAMETER(s);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_list(props, SETTING_SOURCE, obs_module_text("VideoSource"),
						       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumVideoSources, prop);
	obs_enum_scenes(EnumVideoSources, prop);
	obs_property_set_modified_callback(prop, replay_video_source_modified);
	obs_properties_add_bool(props, SETTING_INTERNAL_FRAMES, obs_module_text("CaptureInternalFrames"));

	prop = obs_properties_add_list(props, SETTING_SOURCE_AUDIO, obs_module_text("AudioSource"), OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumAudioSources, prop);

	prop = obs_properties_add_int(props, SETTING_DURATION, obs_module_text("Duration"), SETTING_DURATION_MIN,
				      SETTING_DURATION_MAX, 1000);
	obs_property_int_set_suffix(prop, "ms");
	prop = obs_properties_add_int(props, SETTING_RETRIEVE_DELAY, obs_module_text("LoadDelay"), 0, 100000, 1000);
	obs_property_int_set_suffix(prop, "ms");
	obs_properties_add_int(props, SETTING_REPLAYS, obs_module_text("MaxReplays"), 1, 10, 1);

	prop = obs_properties_add_list(props, SETTING_VISIBILITY_ACTION, obs_module_text("VisibilityAction"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Restart"), VISIBILITY_ACTION_RESTART);
	obs_property_list_add_int(prop, obs_module_text("Pause"), VISIBILITY_ACTION_PAUSE);
	obs_property_list_add_int(prop, obs_module_text("Continue"), VISIBILITY_ACTION_CONTINUE);
	obs_property_list_add_int(prop, obs_module_text("None"), VISIBILITY_ACTION_NONE);

	prop = obs_properties_add_int(props, SETTING_START_DELAY, obs_module_text("StartDelay"), -100000, 100000, 1000);
	obs_property_int_set_suffix(prop, "ms");
	prop = obs_properties_add_int(props, SETTING_FRAME_STEP_COUNT, obs_module_text("FrameStepCount"), 1, 1000, 1);

	prop = obs_properties_add_list(props, SETTING_END_ACTION, obs_module_text("EndAction"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("HideAfterSingle"), END_ACTION_HIDE);
	obs_property_list_add_int(prop, obs_module_text("HideAfterAll"), END_ACTION_HIDE_ALL);
	obs_property_list_add_int(prop, obs_module_text("PauseAfterSingle"), END_ACTION_PAUSE);
	obs_property_list_add_int(prop, obs_module_text("PauseAfterAll"), END_ACTION_PAUSE_ALL);
	obs_property_list_add_int(prop, obs_module_text("LoopSingle"), END_ACTION_LOOP);
	obs_property_list_add_int(prop, obs_module_text("LoopAll"), END_ACTION_LOOP_ALL);
	obs_property_list_add_int(prop, obs_module_text("ReverseAfterSingle"), END_ACTION_REVERSE);
	obs_property_list_add_int(prop, obs_module_text("ReverseAfterAll"), END_ACTION_REVERSE_ALL);

	prop = obs_properties_add_list(props, SETTING_NEXT_SCENE, obs_module_text("NextScene"), OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_scenes(EnumScenes, prop);

	obs_properties_add_float_slider(props, SETTING_SPEED, obs_module_text("SpeedPercentage"), SETTING_SPEED_MIN,
					SETTING_SPEED_MAX, 1.0);
	obs_properties_add_bool(props, SETTING_BACKWARD, obs_module_text("Backwards"));
	obs_property_t *p_blend = obs_properties_add_list(props, SETTING_SLOWMO_BLEND,
							  obs_module_text("SlowmoBlend"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_blend, obs_module_text("SlowmoBlend.Off"), SLOWMO_BLEND_OFF);
	obs_property_list_add_int(p_blend, obs_module_text("SlowmoBlend.Crossfade"), SLOWMO_BLEND_CROSSFADE);

	obs_properties_add_path(props, SETTING_DIRECTORY, obs_module_text("Directory"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(props, SETTING_FILE_FORMAT, obs_module_text("FilenameFormatting"), OBS_TEXT_DEFAULT);
	prop = obs_properties_add_list(props, SETTING_SAVE_ENCODER, obs_module_text("SaveEncoder"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("SaveEncoderAuto"), SAVE_ENCODER_AUTO);
	obs_property_list_add_string(prop, obs_module_text("SaveEncoderHevcNvenc"), SAVE_ENCODER_HEVC_NVENC);
	obs_property_list_add_string(prop, obs_module_text("SaveEncoderH264Nvenc"), SAVE_ENCODER_H264_NVENC);
	obs_property_list_add_string(prop, obs_module_text("SaveEncoderX264"), SAVE_ENCODER_X264);

	prop = obs_properties_add_list(props, SETTING_SAVE_CONTAINER, obs_module_text("SaveContainer"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, obs_module_text("SaveContainerMp4"), SAVE_CONTAINER_MP4);
	obs_property_list_add_string(prop, obs_module_text("SaveContainerMkv"), SAVE_CONTAINER_MKV);

	obs_properties_add_int(props, SETTING_SAVE_QUALITY, obs_module_text("SaveQuality"), SETTING_SAVE_QUALITY_MIN,
			       SETTING_SAVE_QUALITY_MAX, 1);
	prop = obs_properties_add_list(props, SETTING_AUTO_SAVE, obs_module_text("AutoSave"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("AutoSave.Off"), AUTO_SAVE_OFF);
	obs_property_list_add_int(prop, obs_module_text("AutoSave.OnLoad"), AUTO_SAVE_ON_LOAD);
	obs_property_list_add_int(prop, obs_module_text("AutoSave.OnRelease"), AUTO_SAVE_ON_RELEASE);

	obs_properties_add_bool(props, SETTING_LOSSLESS, obs_module_text("Lossless"));
	prop = obs_properties_add_list(props, SETTING_PROGRESS_SOURCE, obs_module_text("ProgressCropSource"),
				       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumVideoSources, prop);

	prop = obs_properties_add_list(props, SETTING_TEXT_SOURCE, obs_module_text("TextSource"), OBS_COMBO_TYPE_EDITABLE,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_sources(EnumTextSources, prop);
	obs_property_set_modified_callback(prop, replay_text_source_modified);

	obs_properties_add_text(props, SETTING_TEXT, obs_module_text("TextFormat"), OBS_TEXT_MULTILINE);

	prop = obs_properties_add_bool(props, SETTING_SOUND_TRIGGER, obs_module_text("SoundTriggerLoadReplay"));
	obs_property_set_modified_callback(prop, replay_sound_trigger_modified);

	obs_properties_add_float_slider(props, SETTING_AUDIO_THRESHOLD, obs_module_text("ThresholdDb"), SETTING_AUDIO_THRESHOLD_MIN,
					SETTING_AUDIO_THRESHOLD_MAX, 0.1);

	prop = obs_properties_add_list(props, SETTING_LOAD_SWITCH_SCENE, obs_module_text("LoadReplaySwitchScene"),
				       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, "", "");
	obs_enum_scenes(EnumScenes, prop);

	obs_properties_t *multicam_group = obs_properties_create();
	prop = obs_properties_add_list(multicam_group, SETTING_STORAGE_FORMAT, obs_module_text("StorageFormat"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("StorageFormat.NV12"), STORAGE_FORMAT_NV12);
	obs_property_list_add_int(prop, obs_module_text("StorageFormat.UYVY"), STORAGE_FORMAT_UYVY);
	obs_property_list_add_int(prop, obs_module_text("StorageFormat.Original"), STORAGE_FORMAT_ORIGINAL);

	prop = obs_properties_add_list(multicam_group, SETTING_DOWNSCALE, obs_module_text("ReplayDownscale"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("ReplayDownscale.Off"), DOWNSCALE_NONE);
	obs_property_list_add_int(prop, obs_module_text("ReplayDownscale.Max1080p"), DOWNSCALE_MAX_1080P);

	obs_properties_add_bool(multicam_group, SETTING_KEEP_BUFFER, obs_module_text("KeepBufferOnLoad"));

	prop = obs_properties_add_list(multicam_group, SETTING_AUTO_RELEASE, obs_module_text("AutoReleaseReplay"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("AutoReleaseReplay.Never"), AUTO_RELEASE_NEVER);
	obs_property_list_add_int(prop, obs_module_text("AutoReleaseReplay.OnDeactivateOrHide"),
				  AUTO_RELEASE_DEACTIVATE_HIDE);

	prop = obs_properties_add_list(multicam_group, SETTING_RESERVE_MODE, obs_module_text("ReserveMemory"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("ReserveMemory.Buffer"), RESERVE_BUFFER);
	obs_property_list_add_int(prop, obs_module_text("ReserveMemory.BufferPlusOne"), RESERVE_BUFFER_PLUS_ONE);
	obs_property_list_add_int(prop, obs_module_text("ReserveMemory.BufferPlusMax"), RESERVE_BUFFER_PLUS_MAX);

	obs_properties_add_group(props, "multicam_buffer_group", obs_module_text("BufferAndMemoryGroup"),
				 OBS_GROUP_NORMAL, multicam_group);

	obs_properties_add_button(props, "replay_button", obs_module_text("LoadReplay"), replay_button);

	return props;
}

int64_t replay_get_duration(void *data)
{
	struct replay_source *c = data;
	return (int64_t)((c->current_replay.duration / c->speed_percent * 100.0) / 1000000UL);
}

int64_t replay_get_time(void *data)
{
	struct replay_source *c = data;
	if (c->replays.size && c->start_timestamp) {
		uint64_t time = 0;
		if (c->pause_timestamp > c->start_timestamp) {
			time = c->pause_timestamp - c->start_timestamp;
		} else {
			time = obs_get_video_frame_time() - c->start_timestamp;
		}
		return time / 1000000UL;
	}
	return 0;
}

void replay_set_time(void *data, int64_t seconds)
{
	struct replay_source *c = data;

	if (c->pause_timestamp > c->start_timestamp) {
		c->start_timestamp = c->pause_timestamp - seconds * 1000000UL;
	} else {
		c->start_timestamp = obs_get_video_frame_time() - seconds * 1000000UL;
	}
}

enum obs_media_state replay_get_state(void *data)
{
	struct replay_source *c = data;
	if (c->play)
		return OBS_MEDIA_STATE_PLAYING;
	if (c->end)
		return OBS_MEDIA_STATE_ENDED;
	return OBS_MEDIA_STATE_PAUSED;
}

struct obs_source_info replay_source_info = {
	.id = REPLAY_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = replay_source_get_name,
	.create = replay_source_create,
	.destroy = replay_source_destroy,
	.load = replay_source_update,
	.update = replay_source_update,
	.get_defaults = replay_source_defaults,
	.show = replay_source_show,
	.hide = replay_source_hide,
	.activate = replay_source_active,
	.deactivate = replay_source_deactive,
	.video_tick = replay_source_tick,
	.get_properties = replay_source_properties,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_get_duration = replay_get_duration,
	.media_get_state = replay_get_state,
	.media_get_time = replay_get_time,
	.media_next = replay_next,
	.media_play_pause = replay_play_pause,
	.media_previous = replay_previous,
	.media_restart = replay_restart,
	.media_stop = replay_stop,
	.media_set_time = replay_set_time,
};

void replay_source_set_speed(struct replay_source *context, float speed_percent)
{
	if (!context)
		return;
	pthread_mutex_lock(&context->video_mutex);
	update_speed(context, speed_percent);
	pthread_mutex_unlock(&context->video_mutex);
}

void replay_source_restart_playback(struct replay_source *context)
{
	if (!context)
		return;
	pthread_mutex_lock(&context->video_mutex);
	replay_restart(context);
	pthread_mutex_unlock(&context->video_mutex);
}

void replay_source_toggle_pause(struct replay_source *context)
{
	if (!context)
		return;
	pthread_mutex_lock(&context->video_mutex);
	replay_play_pause(context, true);
	pthread_mutex_unlock(&context->video_mutex);
}

struct filter_lookup_ctx {
	const char *owner_name;
	bool allow_audio_id;
	obs_source_t *found;
};

static void lookup_filter_cb(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	struct filter_lookup_ctx *lctx = param;
	const char *filter_name = obs_source_get_name(filter);
	const char *id = obs_source_get_unversioned_id(filter);
	if (!filter_name || !lctx->owner_name || !id || strcmp(filter_name, lctx->owner_name) != 0)
		return;
	if (strcmp(id, REPLAY_FILTER_ASYNC_ID) == 0 || strcmp(id, REPLAY_FILTER_ID) == 0 ||
	    (lctx->allow_audio_id && strcmp(id, REPLAY_FILTER_AUDIO_ID) == 0)) {
		lctx->found = filter;
	}
}

static uint64_t inspect_filter_buffered_ns(const char *target_source_name, const char *owner_name, bool audio_Fallback)
{
	if (!target_source_name || !*target_source_name || !owner_name || !*owner_name)
		return 0;
	obs_source_t *s = obs_get_source_by_name(target_source_name);
	if (!s)
		return 0;
	struct filter_lookup_ctx lctx = {
		.owner_name = owner_name,
		.allow_audio_id = audio_Fallback,
		.found = NULL,
	};
	obs_source_enum_filters(s, lookup_filter_cb, &lctx);
	uint64_t buffered_ns = 0;
	struct replay_filter *f = lctx.found ? obs_obj_get_data(lctx.found) : NULL;
	if (f) {
		pthread_mutex_lock(&f->mutex);
		if (f->video_frames.size >= sizeof(struct obs_source_frame *)) {
			struct obs_source_frame *first = NULL;
			struct obs_source_frame *last = NULL;
			circlebuf_peek_front(&f->video_frames, &first, sizeof(first));
			circlebuf_peek_back(&f->video_frames, &last, sizeof(last));
			if (first && last && last->timestamp >= first->timestamp)
				buffered_ns = last->timestamp - first->timestamp;
		} else if (audio_Fallback && f->audio_frames.size >= sizeof(struct obs_audio_data)) {
			struct obs_audio_data first = {0};
			struct obs_audio_data last = {0};
			circlebuf_peek_front(&f->audio_frames, &first, sizeof(first));
			circlebuf_peek_back(&f->audio_frames, &last, sizeof(last));
			if (last.timestamp >= first.timestamp)
				buffered_ns = last.timestamp - first.timestamp;
		}
		pthread_mutex_unlock(&f->mutex);
	}
	obs_source_release(s);
	return buffered_ns;
}

static void compute_replay_window_ns(const struct replay *r, uint64_t *start_out, uint64_t *end_out,
				     uint64_t *duration_out)
{
	if (!r || (r->video_frame_count == 0 && r->audio_frame_count == 0)) {
		if (start_out)
			*start_out = 0;
		if (end_out)
			*end_out = 0;
		if (duration_out)
			*duration_out = 0;
		return;
	}
	uint64_t start_ns = r->first_frame_timestamp;
	if (r->trim_front > 0 && (uint64_t)r->trim_front < r->duration)
		start_ns += (uint64_t)r->trim_front;
	uint64_t end_ns = r->last_frame_timestamp;
	if (r->trim_end > 0 && (uint64_t)r->trim_end < r->duration && end_ns - (uint64_t)r->trim_end > start_ns)
		end_ns -= (uint64_t)r->trim_end;
	if (start_out)
		*start_out = start_ns;
	if (end_out)
		*end_out = end_ns;
	if (duration_out)
		*duration_out = (end_ns > start_ns) ? (end_ns - start_ns) : 0;
}

bool replay_source_get_status(struct replay_source *context, struct replay_source_status *out)
{
	if (!context || !out)
		return false;
	memset(out, 0, sizeof(*out));

	pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->replay_mutex);

	const char *owner_name = context->source ? obs_source_get_name(context->source) : "";
	out->name = owner_name ? owner_name : "";
	out->video_source = context->source_name ? context->source_name : "";
	out->loaded_replays = (int)(context->replays.size / sizeof(struct replay));
	out->active = context->active;
	out->playing = context->play;
	out->speed_percent = context->speed_percent;
	out->backward = context->backward;

	if (out->loaded_replays > 0) {
		compute_replay_window_ns(&context->current_replay, &out->effective_start_ns, &out->effective_end_ns,
					 &out->effective_duration_ns);
		out->duration_ms = (int64_t)(out->effective_duration_ns / 1000000ULL);
		const int64_t base_front = context->current_replay.has_align ? context->current_replay.align_front : 0;
		const int64_t in_ns = context->current_replay.trim_front - base_front;
		const int64_t out_ns =
			(int64_t)context->current_replay.duration - context->current_replay.trim_end - base_front;
		out->mark_in_ms = (int64_t)(in_ns / 1000000LL);
		out->mark_out_ms = (int64_t)(out_ns / 1000000LL);
	}

	char *v_src = context->source_name ? bstrdup(context->source_name) : NULL;
	char *a_src = context->source_audio_name ? bstrdup(context->source_audio_name) : NULL;
	char *owner_copy = owner_name ? bstrdup(owner_name) : NULL;

	pthread_mutex_unlock(&context->replay_mutex);
	pthread_mutex_unlock(&context->video_mutex);

	uint64_t buffered_ns = inspect_filter_buffered_ns(v_src, owner_copy, false);
	if (buffered_ns == 0 && a_src && *a_src)
		buffered_ns = inspect_filter_buffered_ns(a_src, owner_copy, true);
	out->buffered_ms = (int64_t)(buffered_ns / 1000000ULL);

	bfree(v_src);
	bfree(a_src);
	bfree(owner_copy);
	return true;
}

bool replay_source_load_unaligned(struct replay_source *context, uint64_t *first_ts_out, uint64_t *last_ts_out,
				  uint64_t *last_arrival_ns_out)
{
	if (!context)
		return false;
	if (!replay_retrieve_internal(context, false))
		return false;

	bool ok = false;
	pthread_mutex_lock(&context->replay_mutex);
	const size_t count = context->replays.size / sizeof(struct replay);
	if (count > 0) {
		struct replay *r = circlebuf_data(&context->replays, (count - 1) * sizeof(struct replay));
		if (r && (r->video_frame_count > 0 || r->audio_frame_count > 0)) {
			if (first_ts_out)
				*first_ts_out = r->first_frame_timestamp;
			if (last_ts_out)
				*last_ts_out = r->last_frame_timestamp;
			if (last_arrival_ns_out)
				*last_arrival_ns_out = r->last_arrival_ns;
			ok = true;
		}
	}
	pthread_mutex_unlock(&context->replay_mutex);
	return ok;
}

void replay_source_unload_newest(struct replay_source *context)
{
	if (!context)
		return;
	const int count = (int)(context->replays.size / sizeof(struct replay));
	if (count <= 0)
		return;
	context->replay_position = count - 1;
	replay_remove_hotkey(context, 0, NULL, true);
}

void replay_source_align_loaded_replay(struct replay_source *context, uint64_t common_start_ns, uint64_t common_end_ns,
				       uint64_t *start_ns_out, uint64_t *duration_ns_out, int64_t *trim_front_ns_out,
				       int64_t *trim_end_ns_out)
{
	if (!context)
		return;

	uint64_t eff_start = 0;
	uint64_t eff_dur = 0;
	int64_t tf = 0;
	int64_t te = 0;
	bool updated = false;

	pthread_mutex_lock(&context->replay_mutex);
	const int count = (int)(context->replays.size / sizeof(struct replay));
	if (count > 0) {
		struct replay *r = circlebuf_data(&context->replays, (size_t)(count - 1) * sizeof(struct replay));
		if (r) {
			if (common_end_ns > common_start_ns && common_start_ns >= r->first_frame_timestamp &&
			    common_end_ns <= r->last_frame_timestamp) {
				r->trim_front = (int64_t)(common_start_ns - r->first_frame_timestamp);
				r->trim_end = (int64_t)(r->last_frame_timestamp - common_end_ns);
				r->align_front = r->trim_front;
				r->align_end = r->trim_end;
				r->has_align = true;
			}
			tf = r->trim_front;
			te = r->trim_end;
			compute_replay_window_ns(r, &eff_start, NULL, &eff_dur);
			context->replay_position = count - 1;
			updated = true;
		}
	}
	pthread_mutex_unlock(&context->replay_mutex);

	if (updated)
		replay_update_position(context, true);

	if (start_ns_out)
		*start_ns_out = eff_start;
	if (duration_ns_out)
		*duration_ns_out = eff_dur;
	if (trim_front_ns_out)
		*trim_front_ns_out = tf;
	if (trim_end_ns_out)
		*trim_end_ns_out = te;
}

bool replay_source_get_active_window_frames(struct replay_source *context, uint64_t *first_frame_ts_out,
					    uint64_t *last_frame_ts_out)
{
	if (!context)
		return false;
	pthread_mutex_lock(&context->video_mutex);
	if (context->current_replay.video_frame_count == 0 || !context->current_replay.video_frames) {
		pthread_mutex_unlock(&context->video_mutex);
		return false;
	}
	uint64_t eff_start = 0;
	uint64_t eff_end = 0;
	compute_replay_window_ns(&context->current_replay, &eff_start, &eff_end, NULL);
	const uint64_t first_idx = find_closest_frame(context, eff_start, false);
	const uint64_t last_idx = find_closest_frame(context, eff_end, true);
	const uint64_t first_ts = context->current_replay.video_frames[first_idx]->timestamp;
	const uint64_t last_ts = context->current_replay.video_frames[last_idx]->timestamp;
	pthread_mutex_unlock(&context->video_mutex);

	if (first_frame_ts_out)
		*first_frame_ts_out = first_ts;
	if (last_frame_ts_out)
		*last_frame_ts_out = last_ts;
	return true;
}

bool replay_source_get_current_playback_time(struct replay_source *c, uint64_t *abs_ts_out)
{
	if (!c)
		return false;

	pthread_mutex_lock(&c->video_mutex);
	if (c->current_replay.video_frame_count == 0 && c->current_replay.audio_frame_count == 0) {
		pthread_mutex_unlock(&c->video_mutex);
		return false;
	}

	const uint64_t timestamp = c->pause_timestamp == 0 ? obs_get_video_frame_time() : c->pause_timestamp;
	int64_t duration = (int64_t)(timestamp - c->start_timestamp);
	if (c->speed_percent != 100.0f) {
		duration = (int64_t)(duration * c->speed_percent / 100.0);
	}
	if (c->backward) {
		duration = (int64_t)(c->current_replay.last_frame_timestamp - c->current_replay.first_frame_timestamp) -
			   duration;
	}

	uint64_t abs_ts = c->current_replay.first_frame_timestamp;
	if (duration > 0) {
		if ((uint64_t)duration < c->current_replay.duration)
			abs_ts += (uint64_t)duration;
		else
			abs_ts = c->current_replay.last_frame_timestamp;
	}

	pthread_mutex_unlock(&c->video_mutex);

	if (abs_ts_out)
		*abs_ts_out = abs_ts;
	return true;
}

bool replay_source_set_mark_in(struct replay_source *c, uint64_t abs_ts)
{
	if (!c)
		return false;

	pthread_mutex_lock(&c->video_mutex);
	pthread_mutex_lock(&c->replay_mutex);

	if (c->current_replay.video_frame_count == 0 && c->current_replay.audio_frame_count == 0) {
		pthread_mutex_unlock(&c->replay_mutex);
		pthread_mutex_unlock(&c->video_mutex);
		return false;
	}

	int64_t new_trim = 0;
	if (abs_ts > c->current_replay.first_frame_timestamp)
		new_trim = (int64_t)(abs_ts - c->current_replay.first_frame_timestamp);

	/* Clamping to aligned base window */
	const int64_t min_front = c->current_replay.has_align ? c->current_replay.align_front : 0;
	if (new_trim < min_front)
		new_trim = min_front;

	const int64_t align_end = c->current_replay.has_align ? c->current_replay.align_end : 0;
	const int64_t max_front = (int64_t)(c->current_replay.duration - align_end);
	if (new_trim > max_front)
		new_trim = max_front;

	if (new_trim + c->current_replay.trim_end < (int64_t)c->current_replay.duration) {
		c->current_replay.trim_front = new_trim;
		struct replay *r = circlebuf_data(&c->replays, c->replay_position * sizeof(c->current_replay));
		if (r)
			r->trim_front = new_trim;
	}

	pthread_mutex_unlock(&c->replay_mutex);
	pthread_mutex_unlock(&c->video_mutex);
	return true;
}

bool replay_source_set_mark_out(struct replay_source *c, uint64_t abs_ts)
{
	if (!c)
		return false;

	pthread_mutex_lock(&c->video_mutex);
	pthread_mutex_lock(&c->replay_mutex);

	if (c->current_replay.video_frame_count == 0 && c->current_replay.audio_frame_count == 0) {
		pthread_mutex_unlock(&c->replay_mutex);
		pthread_mutex_unlock(&c->video_mutex);
		return false;
	}

	int64_t new_trim = 0;
	if (abs_ts < c->current_replay.last_frame_timestamp)
		new_trim = (int64_t)(c->current_replay.last_frame_timestamp - abs_ts);

	/* Clamping to aligned base window */
	const int64_t min_end = c->current_replay.has_align ? c->current_replay.align_end : 0;
	if (new_trim < min_end)
		new_trim = min_end;

	const int64_t align_front = c->current_replay.has_align ? c->current_replay.align_front : 0;
	const int64_t max_end = (int64_t)(c->current_replay.duration - align_front);
	if (new_trim > max_end)
		new_trim = max_end;

	if (c->current_replay.trim_front + new_trim < (int64_t)c->current_replay.duration) {
		c->current_replay.trim_end = new_trim;
		struct replay *r = circlebuf_data(&c->replays, c->replay_position * sizeof(c->current_replay));
		if (r)
			r->trim_end = new_trim;
	}

	pthread_mutex_unlock(&c->replay_mutex);
	pthread_mutex_unlock(&c->video_mutex);
	return true;
}

void replay_source_mark_reset(struct replay_source *c)
{
	if (!c)
		return;

	pthread_mutex_lock(&c->video_mutex);
	pthread_mutex_lock(&c->replay_mutex);

	if (c->replays.size == 0) {
		pthread_mutex_unlock(&c->replay_mutex);
		pthread_mutex_unlock(&c->video_mutex);
		return;
	}

	if (c->current_replay.has_align) {
		c->current_replay.trim_front = c->current_replay.align_front;
		c->current_replay.trim_end = c->current_replay.align_end;
	} else {
		c->current_replay.trim_end = 0;
		if (c->start_delay > 0) {
			if (c->speed_percent == 100.0f) {
				c->current_replay.trim_front = c->start_delay * -1;
			} else {
				c->current_replay.trim_front = (int64_t)(c->start_delay * c->speed_percent / -100.0);
			}
		} else {
			c->current_replay.trim_front = 0;
		}
	}

	struct replay *r = circlebuf_data(&c->replays, c->replay_position * sizeof(c->current_replay));
	if (r) {
		r->trim_front = c->current_replay.trim_front;
		r->trim_end = c->current_replay.trim_end;
	}

	pthread_mutex_unlock(&c->replay_mutex);
	pthread_mutex_unlock(&c->video_mutex);
}

void replay_source_get_marks_ms(struct replay_source *context, int64_t *in_ms_out, int64_t *out_ms_out,
				int64_t *dur_ms_out)
{
	if (!context) {
		if (in_ms_out)
			*in_ms_out = 0;
		if (out_ms_out)
			*out_ms_out = 0;
		if (dur_ms_out)
			*dur_ms_out = 0;
		return;
	}

	pthread_mutex_lock(&context->video_mutex);
	pthread_mutex_lock(&context->replay_mutex);

	const int64_t base_front = context->current_replay.has_align ? context->current_replay.align_front : 0;
	const int64_t in_ns = context->current_replay.trim_front - base_front;
	const int64_t out_ns =
		(int64_t)context->current_replay.duration - context->current_replay.trim_end - base_front;
	const int64_t dur_ns = (out_ns > in_ns) ? (out_ns - in_ns) : 0;

	pthread_mutex_unlock(&context->replay_mutex);
	pthread_mutex_unlock(&context->video_mutex);

	if (in_ms_out)
		*in_ms_out = (int64_t)(in_ns / 1000000LL);
	if (out_ms_out)
		*out_ms_out = (int64_t)(out_ns / 1000000LL);
	if (dur_ms_out)
		*dur_ms_out = (int64_t)(dur_ns / 1000000LL);
}

void replay_source_auto_save_loaded(struct replay_source *context, const char *trigger)
{
	if (!context || context->auto_save != AUTO_SAVE_ON_LOAD)
		return;

	pthread_mutex_lock(&context->replay_mutex);
	const size_t count = context->replays.size / sizeof(struct replay);
	if (count > 0) {
		struct replay *r = circlebuf_data(&context->replays, (count - 1) * sizeof(struct replay));
		if (r && !r->auto_saved) {
			r->auto_saved = true;
			context->current_replay.auto_saved = true;
			replay_save_replay(context, r, trigger ? trigger : "auto-load");
		}
	}
	pthread_mutex_unlock(&context->replay_mutex);
}

void replay_source_test_set_playback_pos(struct replay_source *context, uint64_t pause_ts, uint64_t start_ts)
{
	if (!context)
		return;

	pthread_mutex_lock(&context->video_mutex);
	context->pause_timestamp = pause_ts;
	context->start_timestamp = start_ts;
	pthread_mutex_unlock(&context->video_mutex);
}

void replay_source_set_blend_output_hook(obs_source_t *source, replay_blend_output_cb cb, void *param)
{
	if (!source)
		return;
	struct replay_source *c = obs_obj_get_data(source);
	if (!c)
		return;
	pthread_mutex_lock(&c->video_mutex);
	if (!c->blend_worker)
		c->blend_worker = replay_blend_worker_create(c->source);
	replay_blend_worker_set_output_hook(c->blend_worker, cb, param);
	pthread_mutex_unlock(&c->video_mutex);
}
