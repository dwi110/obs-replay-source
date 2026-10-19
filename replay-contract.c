	#include "replay-contract.h"
	
	#include "obs-websocket-api.h"

#include "replay-audio-capture.h"
#include "replay-control.h"
#include "replay-decode.h"
#include "replay-library.h"
#include "replay-save-queue.h"
#include "version.h"

#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <inttypes.h>
#include <string.h>

#define CONTRACT_VENDOR "MulticamReplay"
#define ANGLE_PREFIX "Replay "
#define STALE_NS 2000000000ULL
#define READY_WINDOW_NS 12000000000ULL
#define LOOKBACK_DEFAULT_MS 12000
#define LOOKBACK_MIN_MS 1000
#define LOOKBACK_MAX_MS 30000
#define SPEED_MIN 5.0f
#define SPEED_MAX 400.0f
#define AUDIO_OFFSET_MAX_MS 5000
#define MAX_MEMBERS 32
#define MAX_JOBS 32
#define AUDIO_HISTORY_NS 30000000000ULL

struct member {
	obs_source_t *source;
	char angle[64];
};

struct job {
	char job_id[32];
	char type[8];
	char library_id[40];
	char angle[64];
	char tags[256];
	char state[16];
	char error[256];
	char path[1024];
	double progress;
};

struct contract {
	pthread_mutex_t mutex;
	bool initialized;
	bool shutting_down;
	obs_websocket_vendor vendor;

	uint64_t clip_seq;
	uint64_t job_seq;
	bool loaded;
	bool loading;
	char clip_id[32];
	char label[128];
	char clip_library_id[40];
	char origin[16];
	char clip_audio_mode[8];
	uint64_t base_ts;
	uint64_t duration_ns;
	bool loop;
	float speed;
	char error[256];

	char audio_mode[8];
	char audio_angle[64];
	char audio_source[256];
	int64_t audio_offset_ms;

	char save_angle[64];
	bool save_angle_manual;
	char program_angle[64];

	struct member members[MAX_MEMBERS];
	size_t member_count;

	struct job jobs[MAX_JOBS];
	size_t job_count;

	char playlist_state[16];
	int playlist_index;
	char playlist_library_id[40];
	char **playlist_ids;
	size_t playlist_count;
	bool playlist_advance_guard;

	/* Each load gets the generation at start; Release, Trigger, playlist stop, a newer load and shutdown bump it.
	 * A finished load publishes only if its generation is still current. */
	volatile long load_gen;
	volatile long load_threads;

	obs_hotkey_id hotkeys[26];
	size_t hotkey_count;
};

static struct contract g;

/* --------------------------------- helpers --------------------------------- */

static void respond_ok(obs_data_t *response)
{
	if (response)
		obs_data_set_bool(response, "ok", true);
}

static void respond_error(obs_data_t *response, const char *code, const char *message)
{
	if (!response)
		return;
	obs_data_set_bool(response, "ok", false);
	obs_data_set_string(response, "code", code);
	obs_data_set_string(response, "message", message ? message : "");
}

static void emit_event(const char *name, obs_data_t *data)
{
	if (g.vendor)
		obs_websocket_vendor_emit_event(g.vendor, name, data);
}

static void emit_simple(const char *name, const char *k1, const char *v1, const char *k2, const char *v2,
			const char *k3, const char *v3, const char *k4, const char *v4, const char *k5, const char *v5)
{
	obs_data_t *data = obs_data_create();
	if (k1)
		obs_data_set_string(data, k1, v1 ? v1 : "");
	if (k2)
		obs_data_set_string(data, k2, v2 ? v2 : "");
	if (k3)
		obs_data_set_string(data, k3, v3 ? v3 : "");
	if (k4)
		obs_data_set_string(data, k4, v4 ? v4 : "");
	if (k5)
		obs_data_set_string(data, k5, v5 ? v5 : "");
	emit_event(name, data);
	obs_data_release(data);
}

static uint64_t frame_tolerance_ns(void)
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0)
		return 2ULL * 1000000000ULL * ovi.fps_den / ovi.fps_num;
	return 2ULL * 1000000000ULL / 60ULL;
}

static uint64_t clock_frame_ns(void)
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0)
		return 1000000000ULL * ovi.fps_den / ovi.fps_num;
	return 1000000000ULL / 60ULL;
}

static void release_members_locked(void)
{
	for (size_t i = 0; i < g.member_count; i++) {
		replay_source_set_channel(obs_obj_get_data(g.members[i].source), false, false);
		replay_source_clear_replays(obs_obj_get_data(g.members[i].source));
		obs_source_release(g.members[i].source);
	}
	g.member_count = 0;
}

/* Invalidate any load in flight: its decode aborts and its result is dropped, the current clip stays. */
static void cancel_load_locked(void)
{
	os_atomic_inc_long(&g.load_gen);
	g.loading = false;
}

static void stop_playlist_locked(void)
{
	for (size_t i = 0; i < g.playlist_count; i++)
		bfree(g.playlist_ids[i]);
	bfree(g.playlist_ids);
	g.playlist_ids = NULL;
	g.playlist_count = 0;
	g.playlist_index = -1;
	g.playlist_library_id[0] = '\0';
	strcpy(g.playlist_state, "idle");
}

static struct job *add_job_locked(const char *type, const char *library_id, const char *angle, const char *tags)
{
	if (g.job_count == MAX_JOBS) {
		memmove(&g.jobs[0], &g.jobs[1], (MAX_JOBS - 1) * sizeof(struct job));
		g.job_count = MAX_JOBS - 1;
	}
	struct job *job = &g.jobs[g.job_count++];
	memset(job, 0, sizeof *job);
	snprintf(job->job_id, sizeof job->job_id, "j%" PRIu64, ++g.job_seq);
	strncpy(job->type, type, sizeof job->type - 1);
	strncpy(job->library_id, library_id ? library_id : "", sizeof job->library_id - 1);
	strncpy(job->angle, angle ? angle : "", sizeof job->angle - 1);
	strncpy(job->tags, tags ? tags : "", sizeof job->tags - 1);
	strncpy(job->state, "queued", sizeof job->state - 1);
	return job;
}

static void copy_abs_path(char *dst, size_t dst_size, const char *path)
{
	char abs[1024];
	if (dst_size == 0)
		return;
	dst[0] = '\0';
	if (path && os_get_abs_path(path, abs, sizeof abs) && abs[0])
		strncpy(dst, abs, dst_size - 1);
	else if (path)
		strncpy(dst, path, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static struct job *find_job_by_path_locked(const char *path)
{
	char abs[1024];
	const char *want = path;
	if (!path || !*path)
		return NULL;
	if (os_get_abs_path(path, abs, sizeof abs) && abs[0])
		want = abs;
	for (size_t i = g.job_count; i > 0; i--)
		if (strcmp(g.jobs[i - 1].path, path) == 0 || strcmp(g.jobs[i - 1].path, want) == 0)
			return &g.jobs[i - 1];
	return NULL;
}

static struct job *find_job_locked(const char *job_id)
{
	if (!job_id || !*job_id)
		return NULL;
	for (size_t i = 0; i < g.job_count; i++)
		if (strcmp(g.jobs[i].job_id, job_id) == 0)
			return &g.jobs[i];
	return NULL;
}

/* --------------------------------- angles --------------------------------- */

struct discovered {
	obs_source_t *source;
	char angle[64];
	uint64_t first_ts;
	uint64_t last_ts;
	uint64_t arrival_ns;
	uint64_t late_ns;
	bool has_buffer;
};

struct discover_ctx {
	struct discovered *list;
	size_t count;
	size_t max;
};

static bool discover_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	obs_source_t **found = param;
	if (*found)
		return true;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *id = src ? obs_source_get_unversioned_id(src) : NULL;
	if (id && strcmp(id, REPLAY_SOURCE_ID) == 0)
		*found = src;
	else if (obs_sceneitem_is_group(item))
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), discover_item, param);
	return true;
}

static bool discover_scene(void *param, obs_source_t *scene_src)
{
	struct discover_ctx *ctx = param;
	const char *name = obs_source_get_name(scene_src);
	if (!name || strncmp(name, ANGLE_PREFIX, strlen(ANGLE_PREFIX)) != 0)
		return true;
	const char *angle = name + strlen(ANGLE_PREFIX);
	if (!*angle || strchr(angle, ',') || strlen(angle) >= 64)
		return true;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene)
		return true;
	obs_source_t *found = NULL;
	obs_scene_enum_items(scene, discover_item, &found);
	if (!found || ctx->count == ctx->max)
		return true;
	obs_source_t *ref = obs_source_get_ref(found);
	if (!ref)
		return true;
	struct discovered *d = &ctx->list[ctx->count++];
	memset(d, 0, sizeof *d);
	d->source = ref;
	strncpy(d->angle, angle, sizeof d->angle - 1);
	d->has_buffer = replay_source_peek_capture(obs_obj_get_data(ref), &d->first_ts, &d->last_ts, &d->arrival_ns,
						   &d->late_ns);
	return true;
}

static size_t discover_angles(struct discovered *list, size_t max)
{
	struct discover_ctx ctx = {list, 0, max};
	obs_enum_scenes(discover_scene, &ctx);
	return ctx.count;
}

static void release_discovered(struct discovered *list, size_t count)
{
	for (size_t i = 0; i < count; i++)
		obs_source_release(list[i].source);
}

static bool is_member_locked(obs_source_t *source)
{
	for (size_t i = 0; i < g.member_count; i++)
		if (g.members[i].source == source)
			return true;
	return false;
}

static void refresh_program_angle(void)
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return;
	const char *name = obs_source_get_name(scene);
	if (name && strncmp(name, ANGLE_PREFIX, strlen(ANGLE_PREFIX)) == 0 && strlen(name) > strlen(ANGLE_PREFIX)) {
		pthread_mutex_lock(&g.mutex);
		strncpy(g.program_angle, name + strlen(ANGLE_PREFIX), sizeof g.program_angle - 1);
		pthread_mutex_unlock(&g.mutex);
	}
	obs_source_release(scene);
}

static void frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED)
		refresh_program_angle();
}

/* --------------------------------- status --------------------------------- */

static void fill_status(obs_data_t *response)
{
	struct discovered angles[MAX_MEMBERS];
	const size_t angle_count = discover_angles(angles, MAX_MEMBERS);
	const uint64_t now = os_gettime_ns();
	const uint64_t tolerance = frame_tolerance_ns();

	pthread_mutex_lock(&g.mutex);

	bool all_fresh = angle_count > 0;
	uint64_t window_start = 0;
	uint64_t window_end = UINT64_MAX;
	char stalled[80] = "";
	for (size_t i = 0; i < angle_count; i++) {
		const bool fresh = angles[i].has_buffer && angles[i].arrival_ns > 0 && now - angles[i].arrival_ns <= STALE_NS;
		if (!fresh) {
			all_fresh = false;
			if (!stalled[0])
				snprintf(stalled, sizeof stalled, "stalled:%s", angles[i].angle);
		} else {
			if (angles[i].first_ts > window_start)
				window_start = angles[i].first_ts;
			if (angles[i].last_ts < window_end)
				window_end = angles[i].last_ts;
		}
	}
	const bool window_ok = all_fresh && window_end > window_start && window_end - window_start + tolerance >= READY_WINDOW_NS;

	bool audio_ok = true;
	if (strcmp(g.audio_mode, "source") == 0) {
		uint64_t oldest = 0, newest = 0;
		bool gap = false;
		audio_ok = replay_audio_capture_coverage(&oldest, &newest, &gap) && !gap && window_ok && oldest <= window_start &&
			   newest >= window_end;
	}
	const bool ready = all_fresh && window_ok && audio_ok;

	respond_ok(response);
	obs_data_set_int(response, "contract", 1);
	obs_data_set_string(response, "implementation", "raw-nv12");
	obs_data_set_string(response, "implementationVersion", PROJECT_VERSION);
	obs_data_set_bool(response, "ready", ready);
	const char *reason = "";
	if (!ready)
		reason = stalled[0] ? stalled : (!window_ok ? "history" : "audio");
	obs_data_set_string(response, "notReadyReason", reason);
	obs_data_set_string(response, "error", g.error);

	const char *state = "idle";
	int64_t playhead_ms = 0;
	int64_t mark_in_ms = 0;
	int64_t mark_out_ms = 0;
	bool backward = false;
	if (g.loading)
		state = "loading";
	else if (g.loaded && g.member_count > 0) {
		bool playing = false, ended = false;
		uint64_t abs_ts = 0;
		struct replay_source *ref = obs_obj_get_data(g.members[0].source);
		replay_source_channel_status(ref, &playing, &ended, &backward, &abs_ts, NULL, NULL, NULL);
		state = playing ? "playing" : "paused";
		if (abs_ts > g.base_ts)
			playhead_ms = (int64_t)((abs_ts - g.base_ts) / 1000000ULL);
		replay_source_get_marks_ms(ref, &mark_in_ms, &mark_out_ms, NULL);
	}

	obs_data_set_string(response, "state", state);
	if (g.loaded) {
		obs_data_t *clip = obs_data_create();
		obs_data_set_string(clip, "clipId", g.clip_id);
		obs_data_set_string(clip, "label", g.label);
		obs_data_set_int(clip, "durationMs", (long long)(g.duration_ns / 1000000ULL));
		obs_data_set_int(clip, "playheadMs", (long long)playhead_ms);
		obs_data_set_int(clip, "markInMs", (long long)mark_in_ms);
		obs_data_set_int(clip, "markOutMs", (long long)mark_out_ms);
		obs_data_set_int(clip, "speedPercent", (long long)(g.speed + 0.5f));
		obs_data_set_string(clip, "direction", backward ? "backward" : "forward");
		obs_data_set_bool(clip, "loop", g.loop);
		obs_data_set_string(clip, "origin", g.origin);
		obs_data_set_string(clip, "libraryId", g.clip_library_id);
		obs_data_set_string(clip, "audioMode", g.clip_audio_mode);
		obs_data_set_obj(response, "clip", clip);
		obs_data_release(clip);
	}
	/* No clip: leave the key out. obs_data cannot hold JSON null, and obs_data_set_obj(NULL) reaches
	 * obs-websocket clients as an empty object. */

	obs_data_array_t *angle_array = obs_data_array_create();
	uint64_t late_max = 0;
	for (size_t i = 0; i < angle_count; i++) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "angle", angles[i].angle);
		obs_data_set_bool(entry, "inClip", is_member_locked(angles[i].source));
		const bool fresh = angles[i].has_buffer && angles[i].arrival_ns > 0 && now - angles[i].arrival_ns <= STALE_NS;
		obs_data_set_bool(entry, "receiving", fresh);
		const uint64_t buffered = angles[i].last_ts > angles[i].first_ts ? angles[i].last_ts - angles[i].first_ts : 0;
		obs_data_set_int(entry, "bufferedMs", (long long)(buffered / 1000000ULL));
		obs_data_set_int(entry, "lateMs", (long long)(angles[i].late_ns / 1000000ULL));
		if (angles[i].late_ns > late_max)
			late_max = angles[i].late_ns;
		obs_data_array_push_back(angle_array, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(response, "angles", angle_array);
	obs_data_array_release(angle_array);

	obs_data_set_string(response, "programAngle", g.program_angle);
	const char *save_angle = g.save_angle_manual ? g.save_angle : g.program_angle;
	obs_data_set_string(response, "saveAngle", save_angle);
	obs_data_set_bool(response, "saveAngleManual", g.save_angle_manual);

	obs_data_t *audio = obs_data_create();
	obs_data_set_string(audio, "mode", g.audio_mode);
	obs_data_set_string(audio, "angle", g.audio_angle);
	obs_data_set_string(audio, "sourceName", g.audio_source);
	obs_data_set_int(audio, "offsetMs", (long long)g.audio_offset_ms);
	obs_data_set_obj(response, "audio", audio);
	obs_data_release(audio);

	const struct job *last_save = NULL;
	int pending = 0;
	for (size_t i = 0; i < g.job_count; i++) {
		if (strcmp(g.jobs[i].type, "save") != 0)
			continue;
		last_save = &g.jobs[i];
		if (strcmp(g.jobs[i].state, "queued") == 0 || strcmp(g.jobs[i].state, "running") == 0)
			pending++;
	}
	obs_data_t *save = obs_data_create();
	obs_data_set_string(save, "state", last_save ? last_save->state : "idle");
	obs_data_set_string(save, "jobId", last_save ? last_save->job_id : "");
	obs_data_set_string(save, "libraryId", last_save ? last_save->library_id : "");
	obs_data_set_int(save, "pending", pending);
	obs_data_set_string(save, "error", last_save ? last_save->error : "");
	obs_data_set_obj(response, "save", save);
	obs_data_release(save);

	obs_data_set_string(response, "activeGameId", replay_library_active_game());

	obs_data_t *playlist = obs_data_create();
	obs_data_set_string(playlist, "state", g.playlist_state);
	obs_data_set_int(playlist, "index", g.playlist_index);
	obs_data_set_int(playlist, "count", (long long)g.playlist_count);
	obs_data_set_string(playlist, "libraryId", g.playlist_library_id);
	obs_data_set_obj(response, "playlist", playlist);
	obs_data_release(playlist);

	pthread_mutex_unlock(&g.mutex);
	release_discovered(angles, angle_count);
	UNUSED_PARAMETER(late_max);
}

/* --------------------------------- trigger --------------------------------- */

static void release_clip_locked(bool emit)
{
	char released[32];
	strncpy(released, g.clip_id, sizeof released - 1);
	released[sizeof released - 1] = '\0';
	const bool was_loaded = g.loaded;
	cancel_load_locked();
	release_members_locked();
	g.loaded = false;
	g.loading = false;
	g.clip_id[0] = '\0';
	g.clip_library_id[0] = '\0';
	if (emit && was_loaded)
		emit_simple("ClipReleased", "clipId", released, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

static void apply_clip_audio_locked(const struct obs_audio_data *packets, uint64_t count,
				    const struct audio_convert_info *oai)
{
	for (size_t i = 0; i < g.member_count; i++) {
		struct obs_audio_data *copy = NULL;
		if (packets && count > 0) {
			copy = bzalloc((size_t)count * sizeof(struct obs_audio_data));
			for (uint64_t p = 0; p < count; p++) {
				copy[p] = packets[p];
				replay_audio_packet_addref(&copy[p]);
			}
		}
		replay_source_set_replay_audio(obs_obj_get_data(g.members[i].source), copy, count, oai);
	}
}

static bool load_included_locked(struct discovered *included, size_t count, uint64_t start, uint64_t end, bool loop)
{
	for (size_t i = 0; i < count && g.member_count < MAX_MEMBERS; i++) {
		struct replay_source *ctx = obs_obj_get_data(included[i].source);
		replay_source_set_channel(ctx, true, loop);
		if (!replay_source_load_unaligned(ctx, NULL, NULL, NULL)) {
			replay_source_set_channel(ctx, false, false);
			continue;
		}
		replay_source_align_loaded_replay(ctx, start, end, NULL, NULL, NULL, NULL);
		obs_source_t *ref = obs_source_get_ref(included[i].source);
		if (!ref)
			continue;
		struct member *m = &g.members[g.member_count++];
		m->source = ref;
		strncpy(m->angle, included[i].angle, sizeof m->angle - 1);
	}
	return g.member_count > 0;
}

static void fail_trigger(obs_data_t *response, const char *code, const char *message)
{
	emit_simple("TriggerFailed", "code", code, "message", message, NULL, NULL, NULL, NULL, NULL, NULL);
	respond_error(response, code, message);
}

static void handle_trigger(obs_data_t *request, obs_data_t *response)
{
	long long lookback_ms = LOOKBACK_DEFAULT_MS;
	if (request && obs_data_has_user_value(request, "lookbackMs"))
		lookback_ms = obs_data_get_int(request, "lookbackMs");
	if (lookback_ms < LOOKBACK_MIN_MS || lookback_ms > LOOKBACK_MAX_MS) {
		fail_trigger(response, "InvalidParameter", "lookbackMs must be between 1000 and 30000");
		return;
	}
	const char *label = request ? obs_data_get_string(request, "label") : "";

	struct discovered angles[MAX_MEMBERS];
	const size_t angle_count = discover_angles(angles, MAX_MEMBERS);
	const uint64_t now = os_gettime_ns();
	const uint64_t lookback_ns = (uint64_t)lookback_ms * 1000000ULL;

	pthread_mutex_lock(&g.mutex);
	if (g.shutting_down) {
		pthread_mutex_unlock(&g.mutex);
		release_discovered(angles, angle_count);
		fail_trigger(response, "ShuttingDown", "plugin is shutting down");
		return;
	}

	/* Window end: the candidate end that keeps the most fresh angles fully covered, newest on a tie. Taking the
	 * oldest end of all fresh angles let one angle that stopped delivering 1-2 s ago (not yet stale) pull the
	 * window back past the history of every healthy angle and evict them all. */
	bool fresh[MAX_MEMBERS];
	size_t fresh_count = 0;
	for (size_t i = 0; i < angle_count; i++) {
		fresh[i] = angles[i].has_buffer && angles[i].arrival_ns > 0 && now - angles[i].arrival_ns <= STALE_NS;
		if (fresh[i])
			fresh_count++;
	}
	/* Audio mode "angle": a fresh audio angle must be part of the chosen window. */
	size_t audio_idx = SIZE_MAX;
	if (strcmp(g.audio_mode, "angle") == 0) {
		for (size_t i = 0; i < angle_count; i++)
			if (fresh[i] && strcmp(angles[i].angle, g.audio_angle) == 0)
				audio_idx = i;
	}
	size_t best_count = 0;
	uint64_t best_end = 0;
	for (size_t j = 0; j < angle_count; j++) {
		const uint64_t cand = angles[j].last_ts;
		if (!fresh[j] || cand <= lookback_ns)
			continue;
		if (audio_idx != SIZE_MAX &&
		    (angles[audio_idx].last_ts < cand || angles[audio_idx].first_ts > cand - lookback_ns))
			continue;
		size_t count = 0;
		for (size_t i = 0; i < angle_count; i++) {
			if (fresh[i] && angles[i].last_ts >= cand && angles[i].first_ts <= cand - lookback_ns)
				count++;
		}
		if (count > best_count || (count == best_count && count > 0 && cand > best_end)) {
			best_count = count;
			best_end = cand;
		}
	}
	/* Youngest point all chosen angles have; at least best_end, so coverage still holds. */
	uint64_t end = UINT64_MAX;
	for (size_t i = 0; best_count > 0 && i < angle_count; i++) {
		if (fresh[i] && angles[i].last_ts >= best_end && angles[i].first_ts <= best_end - lookback_ns &&
		    angles[i].last_ts < end)
			end = angles[i].last_ts;
	}
	const uint64_t start = best_count > 0 ? end - lookback_ns : 0;

	struct discovered included[MAX_MEMBERS];
	size_t included_count = 0;
	obs_data_array_t *skipped = obs_data_array_create();
	for (size_t i = 0; i < angle_count; i++) {
		const bool covers = fresh[i] && start > 0 && angles[i].last_ts >= end && angles[i].first_ts <= start;
		if (covers) {
			included[included_count++] = angles[i];
		} else {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "angle", angles[i].angle);
			obs_data_set_string(entry, "reason", fresh[i] ? "history" : "stalled");
			obs_data_array_push_back(skipped, entry);
			obs_data_release(entry);
		}
	}

	if (included_count == 0) {
		pthread_mutex_unlock(&g.mutex);
		emit_simple("TriggerFailed", "code", "NotReady", "message", fresh_count ? "history too short" : "no angle receiving",
			    NULL, NULL, NULL, NULL, NULL, NULL);
		obs_data_array_release(skipped);
		release_discovered(angles, angle_count);
		respond_error(response, "NotReady", fresh_count ? "no angle covers the requested window" : "no angle is receiving");
		return;
	}

	if (strcmp(g.audio_mode, "source") == 0) {
		uint64_t oldest = 0, newest = 0;
		bool gap = false;
		const bool covered = replay_audio_capture_coverage(&oldest, &newest, &gap) && !gap && oldest <= start && newest >= end;
		if (!covered) {
			pthread_mutex_unlock(&g.mutex);
			emit_simple("TriggerFailed", "code", "NotReady", "message", "audio does not cover the window", NULL, NULL,
				    NULL, NULL, NULL, NULL);
			obs_data_array_release(skipped);
			release_discovered(angles, angle_count);
			respond_error(response, "NotReady", "selected audio source does not cover the window");
			return;
		}
	}
	if (strcmp(g.audio_mode, "angle") == 0) {
		bool found = false;
		for (size_t i = 0; i < included_count; i++)
			if (strcmp(included[i].angle, g.audio_angle) == 0)
				found = true;
		if (!found) {
			pthread_mutex_unlock(&g.mutex);
			emit_simple("TriggerFailed", "code", "NotReady", "message", "audio angle not in clip", NULL, NULL, NULL,
				    NULL, NULL, NULL);
			obs_data_array_release(skipped);
			release_discovered(angles, angle_count);
			respond_error(response, "NotReady", "selected audio angle is not in the clip");
			return;
		}
	}

	const bool had_clip = g.loaded;
	char old_id[32];
	strncpy(old_id, g.clip_id, sizeof old_id - 1);
	old_id[sizeof old_id - 1] = '\0';
	stop_playlist_locked();
	release_clip_locked(false);

	if (!load_included_locked(included, included_count, start, end, true)) {
		pthread_mutex_unlock(&g.mutex);
		emit_simple("TriggerFailed", "code", "Internal", "message", "failed to load angles", NULL, NULL, NULL, NULL,
			    NULL, NULL);
		obs_data_array_release(skipped);
		release_discovered(angles, angle_count);
		respond_error(response, "Internal", "failed to load the included angles");
		return;
	}

	struct obs_audio_data *audio_packets = NULL;
	uint64_t audio_count = 0;
	struct audio_convert_info audio_info;
	memset(&audio_info, 0, sizeof audio_info);
	if (strcmp(g.audio_mode, "angle") == 0) {
		for (size_t i = 0; i < g.member_count; i++)
			if (strcmp(g.members[i].angle, g.audio_angle) == 0)
				replay_source_copy_replay_audio(obs_obj_get_data(g.members[i].source), &audio_packets, &audio_count,
								&audio_info);
	} else if (strcmp(g.audio_mode, "source") == 0) {
		replay_audio_capture_window(start, end, &audio_packets, &audio_count, &audio_info);
	}
	if (strcmp(g.audio_mode, "off") == 0)
		apply_clip_audio_locked(NULL, 0, NULL);
	else
		apply_clip_audio_locked(audio_packets, audio_count, &audio_info);
	if (audio_packets) {
		for (uint64_t p = 0; p < audio_count; p++)
			free_audio_packet(&audio_packets[p]);
		bfree(audio_packets);
	}

	snprintf(g.clip_id, sizeof g.clip_id, "c%" PRIu64, ++g.clip_seq);
	strncpy(g.label, (label && *label) ? label : "", sizeof g.label - 1);
	strcpy(g.origin, "trigger");
	g.clip_library_id[0] = '\0';
	strncpy(g.clip_audio_mode, g.audio_mode, sizeof g.clip_audio_mode - 1);
	g.base_ts = start;
	g.duration_ns = lookback_ns;
	g.loop = true;
	g.loaded = true;
	g.loading = false;
	g.error[0] = '\0';
	for (size_t i = 0; i < g.member_count; i++) {
		struct replay_source *ctx = obs_obj_get_data(g.members[i].source);
		replay_source_set_backward(ctx, false);
		replay_source_restart_playback(ctx);
		replay_source_set_playing(ctx, true);
	}

	struct dstr angle_list;
	dstr_init(&angle_list);
	for (size_t i = 0; i < g.member_count; i++) {
		if (i)
			dstr_cat(&angle_list, ",");
		dstr_cat(&angle_list, g.members[i].angle);
	}
	char id_copy[32];
	strncpy(id_copy, g.clip_id, sizeof id_copy - 1);
	id_copy[sizeof id_copy - 1] = '\0';
	char duration_text[32];
	snprintf(duration_text, sizeof duration_text, "%" PRIu64, (uint64_t)(g.duration_ns / 1000000ULL));

	pthread_mutex_unlock(&g.mutex);

	if (had_clip)
		emit_simple("ClipReleased", "clipId", old_id, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	emit_simple("ClipLoaded", "clipId", id_copy, "origin", "trigger", "durationMs", duration_text, "angles",
		    angle_list.array, "libraryId", "");
	dstr_free(&angle_list);

	respond_ok(response);
	obs_data_set_string(response, "clipId", id_copy);
	obs_data_set_array(response, "skipped", skipped);
	obs_data_array_release(skipped);
	release_discovered(angles, angle_count);
}

static void handle_release(obs_data_t *response)
{
	pthread_mutex_lock(&g.mutex);
	stop_playlist_locked();
	release_clip_locked(true);
	emit_simple("PlaylistChanged", "state", "idle", "index", "-1", "count", "0", "libraryId", "", NULL, NULL);
	pthread_mutex_unlock(&g.mutex);
	respond_ok(response);
}

/* --------------------------------- transport --------------------------------- */

static void for_each_member(void (*fn)(struct replay_source *, void *), void *param)
{
	pthread_mutex_lock(&g.mutex);
	obs_source_t *refs[MAX_MEMBERS];
	size_t count = g.member_count;
	for (size_t i = 0; i < count; i++)
		refs[i] = obs_source_get_ref(g.members[i].source);
	pthread_mutex_unlock(&g.mutex);
	for (size_t i = 0; i < count; i++) {
		if (!refs[i])
			continue;
		fn(obs_obj_get_data(refs[i]), param);
		obs_source_release(refs[i]);
	}
}

static void do_set_playing(struct replay_source *ctx, void *param)
{
	replay_source_set_playing(ctx, param != NULL);
}

static void do_restart(struct replay_source *ctx, void *param)
{
	UNUSED_PARAMETER(param);
	replay_source_restart_playback(ctx);
}

static void do_set_speed(struct replay_source *ctx, void *param)
{
	replay_source_set_speed(ctx, *(float *)param);
}

static void do_set_backward(struct replay_source *ctx, void *param)
{
	replay_source_set_backward(ctx, param != NULL);
}

static void do_step(struct replay_source *ctx, void *param)
{
	replay_source_channel_step(ctx, *(int64_t *)param);
}

struct seek_param {
	uint64_t base_ts;
	uint64_t position_ns;
};

static void do_seek(struct replay_source *ctx, void *param)
{
	struct seek_param *seek = param;
	uint64_t first = 0;
	replay_source_channel_status(ctx, NULL, NULL, NULL, NULL, NULL, &first, NULL);
	const uint64_t offset = first < seek->base_ts ? (seek->base_ts - first) + seek->position_ns : seek->position_ns;
	replay_source_seek_ns(ctx, offset);
}

static void do_set_loop(struct replay_source *ctx, void *param)
{
	replay_source_set_channel(ctx, true, param != NULL);
}

static bool require_clip(obs_data_t *response)
{
	pthread_mutex_lock(&g.mutex);
	const bool loaded = g.loaded && g.member_count > 0;
	pthread_mutex_unlock(&g.mutex);
	if (!loaded)
		respond_error(response, "NoClipLoaded", "no clip is loaded");
	return loaded;
}

static void handle_speed(obs_data_t *request, obs_data_t *response)
{
	const double speed = request ? obs_data_get_double(request, "speedPercent") : 0.0;
	if (speed < SPEED_MIN || speed > SPEED_MAX) {
		respond_error(response, "InvalidParameter", "speedPercent must be between 5 and 400");
		return;
	}
	if (!require_clip(response))
		return;
	float value = (float)speed;
	pthread_mutex_lock(&g.mutex);
	g.speed = value;
	pthread_mutex_unlock(&g.mutex);
	for_each_member(do_set_speed, &value);
	respond_ok(response);
}

static void handle_direction(bool backward, obs_data_t *response)
{
	if (!require_clip(response))
		return;
	for_each_member(do_set_backward, backward ? (void *)1 : NULL);
	respond_ok(response);
}

static void handle_toggle_direction(obs_data_t *response)
{
	if (!require_clip(response))
		return;
	pthread_mutex_lock(&g.mutex);
	bool backward = false;
	if (g.member_count > 0)
		replay_source_channel_status(obs_obj_get_data(g.members[0].source), NULL, NULL, &backward, NULL, NULL, NULL, NULL);
	pthread_mutex_unlock(&g.mutex);
	for_each_member(do_set_backward, backward ? NULL : (void *)1);
	respond_ok(response);
}

static void handle_step(obs_data_t *request, obs_data_t *response)
{
	const long long frames = request ? obs_data_get_int(request, "frames") : 0;
	if (frames == 0 || frames < -600 || frames > 600) {
		respond_error(response, "InvalidParameter", "frames must be between -600 and 600, not 0");
		return;
	}
	if (!require_clip(response))
		return;
	int64_t delta = (int64_t)frames * (int64_t)clock_frame_ns();
	for_each_member(do_step, &delta);
	respond_ok(response);
}

static void handle_seek(obs_data_t *request, obs_data_t *response)
{
	if (!require_clip(response))
		return;
	long long position = obs_data_get_int(request, "positionMs");
	pthread_mutex_lock(&g.mutex);
	int64_t mark_in = 0, mark_out = 0;
	if (g.member_count > 0)
		replay_source_get_marks_ms(obs_obj_get_data(g.members[0].source), &mark_in, &mark_out, NULL);
	if (position < mark_in)
		position = mark_in;
	if (position > mark_out)
		position = mark_out;
	struct seek_param seek = {g.base_ts, (uint64_t)position * 1000000ULL};
	pthread_mutex_unlock(&g.mutex);
	for_each_member(do_seek, &seek);
	respond_ok(response);
}

static void handle_loop(obs_data_t *request, obs_data_t *response)
{
	if (!require_clip(response))
		return;
	const bool loop = obs_data_get_bool(request, "loop");
	pthread_mutex_lock(&g.mutex);
	g.loop = loop;
	pthread_mutex_unlock(&g.mutex);
	for_each_member(do_set_loop, loop ? (void *)1 : NULL);
	respond_ok(response);
}

/* --------------------------------- marks --------------------------------- */

static void handle_mark(obs_data_t *request, obs_data_t *response, bool in_mark)
{
	if (!require_clip(response))
		return;
	pthread_mutex_lock(&g.mutex);
	if (g.member_count == 0) {
		pthread_mutex_unlock(&g.mutex);
		respond_error(response, "NoClipLoaded", "no clip is loaded");
		return;
	}
	struct replay_source *ref = obs_obj_get_data(g.members[0].source);
	int64_t mark_in = 0, mark_out = 0;
	replay_source_get_marks_ms(ref, &mark_in, &mark_out, NULL);
	long long position;
	if (request && obs_data_has_user_value(request, "positionMs"))
		position = obs_data_get_int(request, "positionMs");
	else {
		bool playing = false, ended = false, backward = false;
		uint64_t abs_ts = 0;
		replay_source_channel_status(ref, &playing, &ended, &backward, &abs_ts, NULL, NULL, NULL);
		position = abs_ts > g.base_ts ? (long long)((abs_ts - g.base_ts) / 1000000ULL) : 0;
	}
	const long long other = in_mark ? mark_out : mark_in;
	if ((in_mark && position >= other) || (!in_mark && position <= other)) {
		pthread_mutex_unlock(&g.mutex);
		respond_error(response, "InvalidParameter", "mark in must stay below mark out");
		return;
	}
	const uint64_t abs = g.base_ts + (uint64_t)position * 1000000ULL;
	for (size_t i = 0; i < g.member_count; i++) {
		struct replay_source *ctx = obs_obj_get_data(g.members[i].source);
		if (in_mark)
			replay_source_set_mark_in(ctx, abs);
		else
			replay_source_set_mark_out(ctx, abs);
	}
	replay_source_get_marks_ms(ref, &mark_in, &mark_out, NULL);
	pthread_mutex_unlock(&g.mutex);
	respond_ok(response);
	obs_data_set_int(response, "markInMs", (long long)mark_in);
	obs_data_set_int(response, "markOutMs", (long long)mark_out);
}

static void handle_mark_reset(obs_data_t *response)
{
	if (!require_clip(response))
		return;
	pthread_mutex_lock(&g.mutex);
	for (size_t i = 0; i < g.member_count; i++)
		replay_source_mark_reset(obs_obj_get_data(g.members[i].source));
	int64_t mark_in = 0, mark_out = 0;
	if (g.member_count > 0)
		replay_source_get_marks_ms(obs_obj_get_data(g.members[0].source), &mark_in, &mark_out, NULL);
	pthread_mutex_unlock(&g.mutex);
	respond_ok(response);
	obs_data_set_int(response, "markInMs", (long long)mark_in);
	obs_data_set_int(response, "markOutMs", (long long)mark_out);
}
/* MSVC has no strtok_r. Comma-only split with the same save-pointer contract. */
static char *split_comma(char *s, char **save)
{
	if (!s)
		s = *save;
	if (!s)
		return NULL;
	while (*s == ',')
		s++;
	if (!*s)
		return NULL;
	char *end = strchr(s, ',');
	if (end) {
		*end = '\0';
		*save = end + 1;
	} else {
		*save = s + strlen(s);
	}
	return s;
}

static bool valid_tag(const char *tag)
{
	const size_t len = strlen(tag);
	if (len < 1 || len > 32)
		return false;
	for (size_t i = 0; i < len; i++) {
		const char c = tag[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return true;
}

/* Writes normalized comma-separated tags into out. Returns false on an invalid tag. */
static bool normalize_tags(const char *raw, char *out, size_t out_size)
{
	if (!raw || !*raw) {
		strncpy(out, "other", out_size);
		return true;
	}
	char buf[256];
	strncpy(buf, raw, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	out[0] = '\0';
	char *saveptr = NULL;
	for (char *tok = split_comma(buf, &saveptr); tok; tok = split_comma(NULL, &saveptr)) {
		while (*tok == ' ')
			tok++;
		if (!valid_tag(tok))
			return false;
		if (out[0])
			strncat(out, ",", out_size - strlen(out) - 1);
		strncat(out, tok, out_size - strlen(out) - 1);
	}
	if (!out[0])
		strncpy(out, "other", out_size);
	return true;
}

static void union_tags(char *dst, size_t dst_size, const char *add)
{
	char buf[256];
	strncpy(buf, add, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	char *saveptr = NULL;
	for (char *tok = split_comma(buf, &saveptr); tok; tok = split_comma(NULL, &saveptr)) {
		char wrapped[64];
		snprintf(wrapped, sizeof wrapped, ",%s,", tok);
		char haystack[300];
		snprintf(haystack, sizeof haystack, ",%s,", dst);
		if (!strstr(haystack, wrapped) && strlen(dst) + strlen(tok) + 2 < dst_size) {
			if (dst[0])
				strcat(dst, ",");
			strcat(dst, tok);
		}
	}
}

static struct member *member_by_angle_locked(const char *angle)
{
	for (size_t i = 0; i < g.member_count; i++)
		if (strcmp(g.members[i].angle, angle) == 0)
			return &g.members[i];
	return NULL;
}

static void emit_save_event(const char *name, const struct job *job, bool ok)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "jobId", job->job_id);
	obs_data_set_string(data, "libraryId", job->library_id);
	obs_data_set_string(data, "angle", job->angle);
	obs_data_set_string(data, "tags", job->tags);
	if (strcmp(name, "SaveFinished") == 0) {
		obs_data_set_string(data, "status", ok ? "ok" : "failed");
		obs_data_set_string(data, "message", job->error);
	}
	emit_event(name, data);
	obs_data_release(data);
}

static void save_hook(const char *trigger, const char *path, bool started, bool ok)
{
	pthread_mutex_lock(&g.mutex);
	struct job *job = NULL;
	if (started && trigger && strncmp(trigger, "contract:", 9) == 0) {
		const char *job_id = trigger + 9;
		for (size_t i = g.job_count; i > 0; i--)
			if (strcmp(g.jobs[i - 1].job_id, job_id) == 0) {
				job = &g.jobs[i - 1];
				break;
			}
		if (job && path)
			copy_abs_path(job->path, sizeof job->path, path);
	} else if (!started) {
		job = find_job_by_path_locked(path);
	}
	if (!job) {
		pthread_mutex_unlock(&g.mutex);
		return;
	}
	strncpy(job->state, started ? "running" : (ok ? "completed" : "failed"), sizeof job->state - 1);
	job->progress = started ? 0.5 : 1.0;
	if (!ok && !started)
		strncpy(job->error, "save failed", sizeof job->error - 1);
	struct job copy = *job;
	pthread_mutex_unlock(&g.mutex);

	if (!started)
		replay_library_update_entry(copy.library_id, ok ? "ready" : "failed", copy.path, ok ? NULL : copy.error);
	else if (copy.path[0])
		replay_library_update_entry(copy.library_id, NULL, copy.path, NULL);
	emit_save_event(started ? "SaveStarted" : "SaveFinished", &copy, ok);
}

static bool enqueue_save_locked(struct member *member, const char *tags, const char *game_id, struct job **job_out,
				obs_data_t *response)
{
	struct replay_source *ctx = obs_obj_get_data(member->source);
	int64_t mark_in = 0, mark_out = 0;
	replay_source_get_marks_ms(ctx, &mark_in, &mark_out, NULL);
	const int64_t range_start = (int64_t)g.base_ts + mark_in * 1000000LL;
	const int64_t range_end = (int64_t)g.base_ts + mark_out * 1000000LL;

	struct replay_library_entry existing;
	if (replay_library_find_merge(member->angle, game_id, range_start, range_end, &existing)) {
		union_tags(existing.tags, sizeof existing.tags, tags);
		replay_library_set_tags(existing.library_id, existing.tags);
		struct job *job = add_job_locked("save", existing.library_id, member->angle, existing.tags);
		strncpy(job->state, "completed", sizeof job->state - 1);
		job->progress = 1.0;
		*job_out = job;
		return true;
	}

	struct replay_library_entry entry;
	memset(&entry, 0, sizeof entry);
	strncpy(entry.game_id, game_id, sizeof entry.game_id - 1);
	strncpy(entry.angle, member->angle, sizeof entry.angle - 1);
	strncpy(entry.label, g.label, sizeof entry.label - 1);
	strncpy(entry.tags, tags, sizeof entry.tags - 1);
	strcpy(entry.state, "saving");
	entry.created_ms = (int64_t)(os_gettime_ns() / 1000000ULL);
	entry.duration_ms = mark_out - mark_in;
	entry.range_start_ns = range_start;
	entry.range_end_ns = range_end;
	if (!replay_library_add_entry(&entry)) {
		respond_error(response, "LibraryUnavailable", "could not create the library entry");
		return false;
	}
	struct job *job = add_job_locked("save", entry.library_id, member->angle, tags);
	char trigger[64];
	snprintf(trigger, sizeof trigger, "contract:%s", job->job_id);
	replay_save_with_trigger(ctx, trigger);
	*job_out = job;
	return true;
}

static void handle_save(obs_data_t *request, obs_data_t *response, bool all_angles)
{
	char tags[256];
	if (!normalize_tags(request ? obs_data_get_string(request, "tags") : "", tags, sizeof tags)) {
		respond_error(response, "InvalidParameter", "tags must match [a-z0-9_]{1,32}");
		return;
	}
	if (!require_clip(response))
		return;
	const char *requested_game = request ? obs_data_get_string(request, "gameId") : "";
	const char *game_id = (requested_game && *requested_game) ? requested_game : replay_library_active_game();
	if (!replay_library_has_game(game_id)) {
		respond_error(response, "GameNotFound", "game does not exist");
		return;
	}

	pthread_mutex_lock(&g.mutex);
	if (all_angles) {
		obs_data_array_t *jobs = obs_data_array_create();
		bool failed = false;
		for (size_t i = 0; i < g.member_count && !failed; i++) {
			struct job *job = NULL;
			if (!enqueue_save_locked(&g.members[i], tags, game_id, &job, response))
				failed = true;
			else {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "angle", g.members[i].angle);
				obs_data_set_string(entry, "jobId", job->job_id);
				obs_data_set_string(entry, "libraryId", job->library_id);
				obs_data_array_push_back(jobs, entry);
				obs_data_release(entry);
				if (strcmp(job->state, "completed") == 0)
					emit_save_event("SaveFinished", job, true);
			}
		}
		pthread_mutex_unlock(&g.mutex);
		if (!failed) {
			respond_ok(response);
			obs_data_set_array(response, "jobs", jobs);
		}
		obs_data_array_release(jobs);
		return;
	}

	const char *requested_angle = request ? obs_data_get_string(request, "angle") : "";
	const char *angle = (requested_angle && *requested_angle) ? requested_angle
								  : (g.save_angle_manual ? g.save_angle : g.program_angle);
	if (!angle || !*angle) {
		pthread_mutex_unlock(&g.mutex);
		respond_error(response, "NoSaveAngle", "no save angle is selected and no replay scene is on program");
		return;
	}
	struct member *member = member_by_angle_locked(angle);
	if (!member) {
		pthread_mutex_unlock(&g.mutex);
		respond_error(response, "UnknownAngle", "angle is not in the loaded clip");
		return;
	}
	struct job *job = NULL;
	if (!enqueue_save_locked(member, tags, game_id, &job, response)) {
		pthread_mutex_unlock(&g.mutex);
		return;
	}
	const bool merged = strcmp(job->state, "completed") == 0;
	struct job copy = *job;
	pthread_mutex_unlock(&g.mutex);
	if (merged)
		emit_save_event("SaveFinished", &copy, true);
	respond_ok(response);
	obs_data_set_string(response, "jobId", copy.job_id);
	obs_data_set_string(response, "libraryId", copy.library_id);
}

static void handle_save_angle(obs_data_t *request, obs_data_t *response)
{
	const char *angle = request ? obs_data_get_string(request, "angle") : "";
	pthread_mutex_lock(&g.mutex);
	if (angle && *angle) {
		if (g.loaded && !member_by_angle_locked(angle)) {
			pthread_mutex_unlock(&g.mutex);
			respond_error(response, "UnknownAngle", "angle is not in the loaded clip");
			return;
		}
		strncpy(g.save_angle, angle, sizeof g.save_angle - 1);
		g.save_angle_manual = true;
	} else {
		g.save_angle[0] = '\0';
		g.save_angle_manual = false;
	}
	pthread_mutex_unlock(&g.mutex);
	respond_ok(response);
}

/* --------------------------------- audio config --------------------------------- */

static void persist_audio_config(void)
{
	const char *dir = replay_library_dir();
	if (!dir || !*dir)
		return;
	struct dstr path;
	dstr_init_copy(&path, dir);
	dstr_cat(&path, "/audio.json");
	obs_data_t *data = obs_data_create();
	pthread_mutex_lock(&g.mutex);
	obs_data_set_string(data, "mode", g.audio_mode);
	obs_data_set_string(data, "angle", g.audio_angle);
	obs_data_set_string(data, "sourceName", g.audio_source);
	obs_data_set_int(data, "offsetMs", (long long)g.audio_offset_ms);
	pthread_mutex_unlock(&g.mutex);
	obs_data_save_json(data, path.array);
	obs_data_release(data);
	dstr_free(&path);
}

static void load_audio_config(void)
{
	const char *dir = replay_library_dir();
	if (!dir || !*dir)
		return;
	struct dstr path;
	dstr_init_copy(&path, dir);
	dstr_cat(&path, "/audio.json");
	obs_data_t *data = obs_data_create_from_json_file(path.array);
	dstr_free(&path);
	if (!data)
		return;
	const char *mode = obs_data_get_string(data, "mode");
	if (mode && *mode)
		strncpy(g.audio_mode, mode, sizeof g.audio_mode - 1);
	strncpy(g.audio_angle, obs_data_get_string(data, "angle"), sizeof g.audio_angle - 1);
	strncpy(g.audio_source, obs_data_get_string(data, "sourceName"), sizeof g.audio_source - 1);
	g.audio_offset_ms = obs_data_get_int(data, "offsetMs");
	obs_data_release(data);
	if (strcmp(g.audio_mode, "source") == 0 && g.audio_source[0]) {
		char err[128];
		replay_audio_capture_configure(g.audio_source, g.audio_offset_ms * 1000000LL, AUDIO_HISTORY_NS, err, sizeof err);
	}
}

static void handle_replay_audio(obs_data_t *request, obs_data_t *response)
{
	const char *mode = request ? obs_data_get_string(request, "mode") : NULL;
	if (request && obs_data_has_user_value(request, "offsetMs")) {
		const long long offset = obs_data_get_int(request, "offsetMs");
		if (offset < -AUDIO_OFFSET_MAX_MS || offset > AUDIO_OFFSET_MAX_MS) {
			respond_error(response, "InvalidParameter", "offsetMs must be between -5000 and 5000");
			return;
		}
	}
	if (mode && *mode && strcmp(mode, "off") != 0 && strcmp(mode, "angle") != 0 && strcmp(mode, "source") != 0) {
		respond_error(response, "InvalidParameter", "mode must be off, angle or source");
		return;
	}
	if (mode && strcmp(mode, "source") == 0) {
		const char *source_name = obs_data_get_string(request, "sourceName");
		if (!source_name || !*source_name) {
			respond_error(response, "InvalidParameter", "source mode needs sourceName");
			return;
		}
		char err[128];
		const long long offset = request ? obs_data_get_int(request, "offsetMs") : 0;
		if (!replay_audio_capture_configure(source_name, offset * 1000000LL, AUDIO_HISTORY_NS, err, sizeof err)) {
			respond_error(response, "InvalidParameter", err[0] ? err : "audio source not found");
			return;
		}
	}
	pthread_mutex_lock(&g.mutex);
	if (mode && *mode)
		strncpy(g.audio_mode, mode, sizeof g.audio_mode - 1);
	if (request && obs_data_has_user_value(request, "angle"))
		strncpy(g.audio_angle, obs_data_get_string(request, "angle"), sizeof g.audio_angle - 1);
	if (request && obs_data_has_user_value(request, "sourceName"))
		strncpy(g.audio_source, obs_data_get_string(request, "sourceName"), sizeof g.audio_source - 1);
	if (request && obs_data_has_user_value(request, "offsetMs"))
		g.audio_offset_ms = obs_data_get_int(request, "offsetMs");
	pthread_mutex_unlock(&g.mutex);
	persist_audio_config();
	respond_ok(response);
}

/* --------------------------------- library load & playlist --------------------------------- */

struct load_task {
	char library_id[40];
	char file[1024];
	char angle[64];
	char job_id[32];
	bool for_playlist;
	long gen;
};

static void install_decoded_locked(struct replay_decoded_clip *clip, bool loop, struct dstr *angle_list)
{
	struct discovered angles[MAX_MEMBERS];
	const size_t count = discover_angles(angles, MAX_MEMBERS);
	for (size_t i = 0; i < count && g.member_count < MAX_MEMBERS; i++) {
		struct replay_source *ctx = obs_obj_get_data(angles[i].source);
		replay_source_set_channel(ctx, true, loop);
		if (!replay_source_install_decoded(ctx, clip->video_frames, clip->video_frame_count, clip->audio_frames,
						   clip->audio_frame_count, &clip->oai, clip->first_frame_timestamp,
						   clip->last_frame_timestamp, clip->duration)) {
			replay_source_set_channel(ctx, false, false);
			continue;
		}
		obs_source_t *ref = obs_source_get_ref(angles[i].source);
		if (!ref)
			continue;
		struct member *m = &g.members[g.member_count++];
		m->source = ref;
		strncpy(m->angle, angles[i].angle, sizeof m->angle - 1);
		if (angle_list) {
			if (angle_list->len)
				dstr_cat(angle_list, ",");
			dstr_cat(angle_list, angles[i].angle);
		}
	}
	release_discovered(angles, count);
}

static bool load_should_abort(void *param)
{
	const struct load_task *task = param;
	return os_atomic_load_long(&g.load_gen) != task->gen;
}

static void *load_thread(void *param)
{
	struct load_task *task = param;
	struct replay_decoded_clip clip;
	memset(&clip, 0, sizeof clip);
	char err[256] = "";
	const bool ok = replay_decode_file(task->file, os_gettime_ns(), &clip, load_should_abort, task, err, sizeof err);

	pthread_mutex_lock(&g.mutex);
	/* A cancelled or superseded load must not touch the clip, the playlist or g.loading (a newer load owns it). */
	const bool current = os_atomic_load_long(&g.load_gen) == task->gen && !g.shutting_down;
	if (current)
		g.loading = false;
	if (!ok || !current) {
		struct job *job = find_job_locked(task->job_id);
		if (job) {
			strncpy(job->state, "failed", sizeof job->state - 1);
			strncpy(job->error, !current ? "cancelled" : err[0] ? err : "load failed", sizeof job->error - 1);
		}
		if (current && err[0])
			strncpy(g.error, err, sizeof g.error - 1);
		const bool playlist_error = current && task->for_playlist;
		if (playlist_error)
			strcpy(g.playlist_state, "error");
		pthread_mutex_unlock(&g.mutex);
		replay_decoded_clip_free(&clip);
		if (playlist_error)
			emit_simple("PlaylistChanged", "state", "error", "index", "-1", "count", "0", "libraryId", "", NULL,
				    NULL);
		bfree(task);
		os_atomic_dec_long(&g.load_threads);
		return NULL;
	}
	release_members_locked();
	struct dstr angle_list;
	dstr_init(&angle_list);
	install_decoded_locked(&clip, false, &angle_list);
	for (size_t i = 0; i < g.member_count; i++) {
		struct replay_source *ctx = obs_obj_get_data(g.members[i].source);
		replay_source_set_backward(ctx, false);
		replay_source_restart_playback(ctx);
		replay_source_set_playing(ctx, true);
	}
	snprintf(g.clip_id, sizeof g.clip_id, "c%" PRIu64, ++g.clip_seq);
	strncpy(g.clip_library_id, task->library_id, sizeof g.clip_library_id - 1);
	strcpy(g.origin, task->for_playlist ? "playlist" : "library");
	strcpy(g.clip_audio_mode, "angle");
	g.base_ts = clip.first_frame_timestamp;
	g.duration_ns = clip.duration;
	g.loop = false;
	g.loaded = g.member_count > 0;
	struct job *job = find_job_locked(task->job_id);
	if (job) {
		strncpy(job->state, g.loaded ? "completed" : "failed", sizeof job->state - 1);
		job->progress = 1.0;
	}
	char id_copy[32];
	strncpy(id_copy, g.clip_id, sizeof id_copy - 1);
	id_copy[sizeof id_copy - 1] = '\0';
	char duration_text[32];
	snprintf(duration_text, sizeof duration_text, "%" PRIu64, (uint64_t)(g.duration_ns / 1000000ULL));
	const bool playlist = task->for_playlist;
	const bool loaded = g.loaded;
	if (playlist) {
		strcpy(g.playlist_state, loaded ? "playing" : "error");
		strncpy(g.playlist_library_id, task->library_id, sizeof g.playlist_library_id - 1);
	}
	char playlist_index[16];
	char playlist_count[16];
	snprintf(playlist_index, sizeof playlist_index, "%d", g.playlist_index);
	snprintf(playlist_count, sizeof playlist_count, "%zu", g.playlist_count);
	char library_id[40];
	strncpy(library_id, task->library_id, sizeof library_id - 1);
	library_id[sizeof library_id - 1] = '\0';
	pthread_mutex_unlock(&g.mutex);

	emit_simple("ClipLoaded", "clipId", id_copy, "origin", playlist ? "playlist" : "library", "durationMs",
		    duration_text, "angles", angle_list.array ? angle_list.array : "", "libraryId", library_id);
	if (playlist)
		emit_simple("PlaylistChanged", "state", loaded ? "playing" : "error", "index", playlist_index, "count",
			    playlist_count, "libraryId", library_id, NULL, NULL);
	dstr_free(&angle_list);
	replay_decoded_clip_free(&clip);
	bfree(task);
	os_atomic_dec_long(&g.load_threads);
	return NULL;
}

static bool start_load_locked(const struct replay_library_entry *entry, bool for_playlist, const char *job_id,
			      obs_data_t *response)
{
	if (!replay_decode_supported()) {
		respond_error(response, "Internal", "FFmpeg decode is not available");
		return false;
	}
	struct load_task *task = bzalloc(sizeof *task);
	strncpy(task->library_id, entry->library_id, sizeof task->library_id - 1);
	strncpy(task->file, entry->file, sizeof task->file - 1);
	strncpy(task->angle, entry->angle, sizeof task->angle - 1);
	strncpy(task->job_id, job_id ? job_id : "", sizeof task->job_id - 1);
	task->for_playlist = for_playlist;
	struct job *job = find_job_locked(job_id);
	if (job)
		strncpy(job->state, "running", sizeof job->state - 1);
	/* A newer load supersedes one still in flight. */
	task->gen = os_atomic_inc_long(&g.load_gen);
	g.loading = true;
	os_atomic_inc_long(&g.load_threads);
	pthread_t thread;
	if (pthread_create(&thread, NULL, load_thread, task) != 0) {
		os_atomic_dec_long(&g.load_threads);
		g.loading = false;
		bfree(task);
		respond_error(response, "Internal", "could not start the load");
		return false;
	}
	pthread_detach(thread);
	return true;
}

static void handle_load_entry(obs_data_t *request, obs_data_t *response)
{
	const char *library_id = request ? obs_data_get_string(request, "libraryId") : "";
	struct replay_library_entry entry;
	if (!replay_library_get(library_id, &entry)) {
		respond_error(response, "EntryNotFound", "library entry does not exist");
		return;
	}
	if (strcmp(entry.state, "ready") != 0 || !entry.file[0]) {
		respond_error(response, "EntryNotReady", "library entry is not ready to load");
		return;
	}
	pthread_mutex_lock(&g.mutex);
	if (g.loaded)
		release_clip_locked(true);
	struct job *job = add_job_locked("load", entry.library_id, entry.angle, entry.tags);
	const bool started = start_load_locked(&entry, false, job->job_id, response);
	char job_id[32];
	strncpy(job_id, job->job_id, sizeof job_id - 1);
	job_id[sizeof job_id - 1] = '\0';
	pthread_mutex_unlock(&g.mutex);
	if (!started)
		return;
	respond_ok(response);
	obs_data_set_string(response, "jobId", job_id);
}

static void emit_playlist_locked(void)
{
	char index_text[16], count_text[16];
	snprintf(index_text, sizeof index_text, "%d", g.playlist_index);
	snprintf(count_text, sizeof count_text, "%zu", g.playlist_count);
	emit_simple("PlaylistChanged", "state", g.playlist_state, "index", index_text, "count", count_text, "libraryId",
		    g.playlist_library_id, NULL, NULL);
}

static bool load_playlist_index_locked(obs_data_t *response)
{
	if (g.playlist_index < 0 || (size_t)g.playlist_index >= g.playlist_count) {
		strcpy(g.playlist_state, "idle");
		g.playlist_library_id[0] = '\0';
		emit_playlist_locked();
		return false;
	}
	struct replay_library_entry entry;
	if (!replay_library_get(g.playlist_ids[g.playlist_index], &entry) || strcmp(entry.state, "ready") != 0) {
		strcpy(g.playlist_state, "error");
		emit_playlist_locked();
		if (response)
			respond_error(response, "EntryNotReady", "playlist entry is not ready");
		return false;
	}
	strncpy(g.playlist_library_id, entry.library_id, sizeof g.playlist_library_id - 1);
	strcpy(g.playlist_state, "loading");
	emit_playlist_locked();
	struct job *job = add_job_locked("load", entry.library_id, entry.angle, entry.tags);
	return start_load_locked(&entry, true, job->job_id, response);
}

static void handle_play_playlist(obs_data_t *request, obs_data_t *response)
{
	const char *game_id = request ? obs_data_get_string(request, "gameId") : "";
	const char *tags = request ? obs_data_get_string(request, "tags") : "";
	char **ids = NULL;
	const size_t count = replay_library_playlist(game_id, tags, &ids);
	if (count == 0) {
		bfree(ids);
		respond_error(response, "NoMatchingEntries", "no library entries match");
		return;
	}
	pthread_mutex_lock(&g.mutex);
	stop_playlist_locked();
	g.playlist_ids = ids;
	g.playlist_count = count;
	g.playlist_index = 0;
	const bool started = load_playlist_index_locked(response);
	pthread_mutex_unlock(&g.mutex);
	if (started)
		respond_ok(response);
}

static void handle_playlist_control(obs_data_t *request, obs_data_t *response)
{
	const char *action = request ? obs_data_get_string(request, "action") : "";
	pthread_mutex_lock(&g.mutex);
	if (g.playlist_count == 0 || strcmp(g.playlist_state, "idle") == 0) {
		pthread_mutex_unlock(&g.mutex);
		respond_error(response, "NoActivePlaylist", "no playlist is active");
		return;
	}
	if (strcmp(action, "stop") == 0) {
		/* An entry still loading must not start playing after Stop. */
		if (g.loading)
			cancel_load_locked();
		stop_playlist_locked();
		emit_playlist_locked();
	} else if (strcmp(action, "pause") == 0) {
		strcpy(g.playlist_state, "paused");
		emit_playlist_locked();
		pthread_mutex_unlock(&g.mutex);
		for_each_member(do_set_playing, NULL);
		respond_ok(response);
		return;
	} else if (strcmp(action, "play") == 0) {
		strcpy(g.playlist_state, "playing");
		emit_playlist_locked();
		pthread_mutex_unlock(&g.mutex);
		for_each_member(do_set_playing, (void *)1);
		respond_ok(response);
		return;
	} else if (strcmp(action, "next") == 0 || strcmp(action, "previous") == 0) {
		const bool next = strcmp(action, "next") == 0;
		if (next && (size_t)g.playlist_index + 1 >= g.playlist_count) {
			pthread_mutex_unlock(&g.mutex);
			respond_error(response, "PlaylistEnd", "already at the last playlist entry");
			return;
		}
		if (!next && g.playlist_index <= 0) {
			pthread_mutex_unlock(&g.mutex);
			respond_error(response, "PlaylistStart", "already at the first playlist entry");
			return;
		}
		g.playlist_index += next ? 1 : -1;
		load_playlist_index_locked(NULL);
	} else {
		pthread_mutex_unlock(&g.mutex);
		respond_error(response, "InvalidParameter", "action must be play, pause, next, previous or stop");
		return;
	}
	pthread_mutex_unlock(&g.mutex);
	respond_ok(response);
}

static void playlist_tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(seconds);
	pthread_mutex_lock(&g.mutex);
	if (strcmp(g.playlist_state, "playing") != 0 || !g.loaded || g.loading || g.member_count == 0) {
		g.playlist_advance_guard = false;
		pthread_mutex_unlock(&g.mutex);
		return;
	}
	bool ended = false;
	replay_source_channel_status(obs_obj_get_data(g.members[0].source), NULL, &ended, NULL, NULL, NULL, NULL, NULL);
	if (!ended || g.playlist_advance_guard) {
		pthread_mutex_unlock(&g.mutex);
		return;
	}
	g.playlist_advance_guard = true;
	if ((size_t)g.playlist_index + 1 >= g.playlist_count) {
		strcpy(g.playlist_state, "idle");
		emit_playlist_locked();
		pthread_mutex_unlock(&g.mutex);
		return;
	}
	g.playlist_index++;
	load_playlist_index_locked(NULL);
	pthread_mutex_unlock(&g.mutex);
}

/* --------------------------------- library requests --------------------------------- */

static void handle_get_library(obs_data_t *response)
{
	respond_ok(response);
	obs_data_array_t *games = obs_data_array_create();
	replay_library_games(games);
	obs_data_set_array(response, "games", games);
	obs_data_array_release(games);
	obs_data_set_string(response, "activeGameId", replay_library_active_game());
	obs_data_set_string(response, "tags", "highlight,scoring_play,foul,other");
	obs_data_array_t *entries = obs_data_array_create();
	replay_library_entries(entries);
	obs_data_set_array(response, "entries", entries);
	obs_data_array_release(entries);

	pthread_mutex_lock(&g.mutex);
	obs_data_array_t *jobs = obs_data_array_create();
	for (size_t i = 0; i < g.job_count; i++) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "jobId", g.jobs[i].job_id);
		obs_data_set_string(entry, "type", g.jobs[i].type);
		obs_data_set_string(entry, "libraryId", g.jobs[i].library_id);
		obs_data_set_string(entry, "state", g.jobs[i].state);
		obs_data_set_double(entry, "progress", g.jobs[i].progress);
		obs_data_set_string(entry, "error", g.jobs[i].error);
		obs_data_array_push_back(jobs, entry);
		obs_data_release(entry);
	}
	obs_data_t *playlist = obs_data_create();
	obs_data_set_string(playlist, "state", g.playlist_state);
	obs_data_set_int(playlist, "index", g.playlist_index);
	obs_data_set_int(playlist, "count", (long long)g.playlist_count);
	obs_data_set_string(playlist, "libraryId", g.playlist_library_id);
	pthread_mutex_unlock(&g.mutex);
	obs_data_set_array(response, "jobs", jobs);
	obs_data_array_release(jobs);
	obs_data_set_obj(response, "playlist", playlist);
	obs_data_release(playlist);
}

static void handle_create_game(obs_data_t *request, obs_data_t *response)
{
	const char *name = request ? obs_data_get_string(request, "name") : "";
	char id[64];
	if (!replay_library_create_game(name, id, sizeof id)) {
		respond_error(response, "InvalidParameter", "game name must not be empty");
		return;
	}
	respond_ok(response);
	obs_data_set_string(response, "gameId", id);
}

static void handle_set_active_game(obs_data_t *request, obs_data_t *response)
{
	const char *id = request ? obs_data_get_string(request, "gameId") : "";
	if (!replay_library_set_active_game(id)) {
		respond_error(response, "GameNotFound", "game does not exist");
		return;
	}
	respond_ok(response);
}

static void handle_set_tags(obs_data_t *request, obs_data_t *response)
{
	const char *library_id = request ? obs_data_get_string(request, "libraryId") : "";
	char tags[256];
	if (!normalize_tags(request ? obs_data_get_string(request, "tags") : "", tags, sizeof tags)) {
		respond_error(response, "InvalidParameter", "tags must match [a-z0-9_]{1,32}");
		return;
	}
	if (!replay_library_set_tags(library_id, tags)) {
		respond_error(response, "EntryNotFound", "library entry does not exist");
		return;
	}
	respond_ok(response);
}

/* --------------------------------- dispatch --------------------------------- */

void replay_contract_request(const char *type, obs_data_t *request, obs_data_t *response)
{
	if (!type)
		return;
	if (!g.initialized) {
		respond_error(response, "Internal", "contract is not initialized");
		return;
	}
	if (strcmp(type, "GetStatus") == 0)
		fill_status(response);
	else if (strcmp(type, "GetLibrary") == 0)
		handle_get_library(response);
	else if (strcmp(type, "Trigger") == 0)
		handle_trigger(request, response);
	else if (strcmp(type, "Release") == 0)
		handle_release(response);
	else if (strcmp(type, "Play") == 0) {
		if (require_clip(response)) {
			for_each_member(do_set_playing, (void *)1);
			respond_ok(response);
		}
	} else if (strcmp(type, "Pause") == 0) {
		if (require_clip(response)) {
			for_each_member(do_set_playing, NULL);
			respond_ok(response);
		}
	} else if (strcmp(type, "TogglePlay") == 0) {
		if (require_clip(response)) {
			pthread_mutex_lock(&g.mutex);
			bool playing = false;
			if (g.member_count > 0)
				replay_source_channel_status(obs_obj_get_data(g.members[0].source), &playing, NULL, NULL, NULL,
							     NULL, NULL, NULL);
			pthread_mutex_unlock(&g.mutex);
			for_each_member(do_set_playing, playing ? NULL : (void *)1);
			respond_ok(response);
		}
	} else if (strcmp(type, "Restart") == 0) {
		if (require_clip(response)) {
			for_each_member(do_restart, NULL);
			respond_ok(response);
		}
	} else if (strcmp(type, "SetSpeed") == 0)
		handle_speed(request, response);
	else if (strcmp(type, "SetDirection") == 0) {
		const char *direction = request ? obs_data_get_string(request, "direction") : "";
		if (strcmp(direction, "forward") != 0 && strcmp(direction, "backward") != 0)
			respond_error(response, "InvalidParameter", "direction must be forward or backward");
		else
			handle_direction(strcmp(direction, "backward") == 0, response);
	} else if (strcmp(type, "ToggleDirection") == 0)
		handle_toggle_direction(response);
	else if (strcmp(type, "Step") == 0)
		handle_step(request, response);
	else if (strcmp(type, "Seek") == 0)
		handle_seek(request, response);
	else if (strcmp(type, "SetLoop") == 0)
		handle_loop(request, response);
	else if (strcmp(type, "MarkIn") == 0)
		handle_mark(request, response, true);
	else if (strcmp(type, "MarkOut") == 0)
		handle_mark(request, response, false);
	else if (strcmp(type, "MarkReset") == 0)
		handle_mark_reset(response);
	else if (strcmp(type, "SetSaveAngle") == 0)
		handle_save_angle(request, response);
	else if (strcmp(type, "SetReplayAudio") == 0)
		handle_replay_audio(request, response);
	else if (strcmp(type, "Save") == 0)
		handle_save(request, response, false);
	else if (strcmp(type, "SaveAllAngles") == 0)
		handle_save(request, response, true);
	else if (strcmp(type, "CreateGame") == 0)
		handle_create_game(request, response);
	else if (strcmp(type, "SetActiveGame") == 0)
		handle_set_active_game(request, response);
	else if (strcmp(type, "SetTags") == 0)
		handle_set_tags(request, response);
	else if (strcmp(type, "LoadLibraryEntry") == 0)
		handle_load_entry(request, response);
	else if (strcmp(type, "PlayPlaylist") == 0)
		handle_play_playlist(request, response);
	else if (strcmp(type, "PlaylistControl") == 0)
		handle_playlist_control(request, response);
	else
		respond_error(response, "InvalidParameter", "unknown request");
}

static void vendor_request(obs_data_t *request, obs_data_t *response, void *priv)
{
	replay_contract_request((const char *)priv, request, response);
}

/* --------------------------------- hotkeys --------------------------------- */

enum hotkey_action {
	HK_TRIGGER,
	HK_RELEASE,
	HK_TOGGLE_PLAY,
	HK_RESTART,
	HK_SPEED_100,
	HK_SPEED_75,
	HK_SPEED_50,
	HK_SPEED_25,
	HK_TOGGLE_DIRECTION,
	HK_STEP_FORWARD,
	HK_STEP_BACKWARD,
	HK_TOGGLE_LOOP,
	HK_MARK_IN,
	HK_MARK_OUT,
	HK_MARK_RESET,
	HK_SAVE_HIGHLIGHT,
	HK_SAVE_SCORING,
	HK_SAVE_FOUL,
	HK_SAVE_OTHER,
	HK_SAVE_ALL,
	HK_PLAYLIST_ALL,
	HK_PLAYLIST_TOGGLE,
	HK_PLAYLIST_NEXT,
	HK_PLAYLIST_PREVIOUS,
	HK_PLAYLIST_STOP
};

static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	obs_data_t *request = obs_data_create();
	const char *type = NULL;
	switch ((int)(intptr_t)data) {
	case HK_TRIGGER:
		type = "Trigger";
		break;
	case HK_RELEASE:
		type = "Release";
		break;
	case HK_TOGGLE_PLAY:
		type = "TogglePlay";
		break;
	case HK_RESTART:
		type = "Restart";
		break;
	case HK_SPEED_100:
	case HK_SPEED_75:
	case HK_SPEED_50:
	case HK_SPEED_25:
		type = "SetSpeed";
		obs_data_set_double(request, "speedPercent",
				    (int)(intptr_t)data == HK_SPEED_100 ? 100
				    : (int)(intptr_t)data == HK_SPEED_75 ? 75
				    : (int)(intptr_t)data == HK_SPEED_50 ? 50
									: 25);
		break;
	case HK_TOGGLE_DIRECTION:
		type = "ToggleDirection";
		break;
	case HK_STEP_FORWARD:
	case HK_STEP_BACKWARD:
		type = "Step";
		obs_data_set_int(request, "frames", (int)(intptr_t)data == HK_STEP_FORWARD ? 1 : -1);
		break;
	case HK_TOGGLE_LOOP:
		pthread_mutex_lock(&g.mutex);
		obs_data_set_bool(request, "loop", !g.loop);
		pthread_mutex_unlock(&g.mutex);
		type = "SetLoop";
		break;
	case HK_MARK_IN:
		type = "MarkIn";
		break;
	case HK_MARK_OUT:
		type = "MarkOut";
		break;
	case HK_MARK_RESET:
		type = "MarkReset";
		break;
	case HK_SAVE_HIGHLIGHT:
	case HK_SAVE_SCORING:
	case HK_SAVE_FOUL:
	case HK_SAVE_OTHER:
	case HK_SAVE_ALL:
		type = (int)(intptr_t)data == HK_SAVE_ALL ? "SaveAllAngles" : "Save";
		obs_data_set_string(request, "tags",
				    (int)(intptr_t)data == HK_SAVE_HIGHLIGHT ? "highlight"
				    : (int)(intptr_t)data == HK_SAVE_SCORING ? "scoring_play"
				    : (int)(intptr_t)data == HK_SAVE_FOUL    ? "foul"
									    : "other");
		break;
	case HK_PLAYLIST_ALL:
		type = "PlayPlaylist";
		break;
	case HK_PLAYLIST_TOGGLE:
		pthread_mutex_lock(&g.mutex);
		type = strcmp(g.playlist_state, "playing") == 0 ? "PlaylistControl" : "PlaylistControl";
		obs_data_set_string(request, "action", strcmp(g.playlist_state, "playing") == 0 ? "pause" : "play");
		pthread_mutex_unlock(&g.mutex);
		break;
	case HK_PLAYLIST_NEXT:
	case HK_PLAYLIST_PREVIOUS:
	case HK_PLAYLIST_STOP:
		type = "PlaylistControl";
		obs_data_set_string(request, "action", (int)(intptr_t)data == HK_PLAYLIST_NEXT       ? "next"
							: (int)(intptr_t)data == HK_PLAYLIST_PREVIOUS ? "previous"
												      : "stop");
		break;
	}
	if (type)
		replay_contract_request(type, request, NULL);
	obs_data_release(request);
}

static void register_hotkey(int action, const char *name, const char *description)
{
	if (g.hotkey_count >= sizeof g.hotkeys / sizeof g.hotkeys[0])
		return;
	g.hotkeys[g.hotkey_count++] =
		obs_hotkey_register_frontend(name, description, hotkey_cb, (void *)(intptr_t)action);
}

/* --------------------------------- lifecycle --------------------------------- */

void replay_contract_init(void)
{
	if (g.initialized)
		return;
	pthread_mutex_init(&g.mutex, NULL);
	strcpy(g.audio_mode, "off");
	strcpy(g.clip_audio_mode, "off");
	strcpy(g.playlist_state, "idle");
	g.playlist_index = -1;
	g.speed = 100.0f;

	if (obs_current_module()) {
		char *dir = obs_module_config_path("library");
		if (dir) {
			replay_library_init(dir);
			bfree(dir);
		}
	} else {
		replay_library_init("/tmp/replay-contract-library");
	}
	load_audio_config();
	replay_control_set_save_hook(save_hook);
	if (obs_current_module())
		obs_frontend_add_event_callback(frontend_event, NULL);
	obs_add_tick_callback(playlist_tick, NULL);

	register_hotkey(HK_TRIGGER, "MulticamReplay.Trigger", "Multicam Replay: trigger");
	register_hotkey(HK_RELEASE, "MulticamReplay.Release", "Multicam Replay: release");
	register_hotkey(HK_TOGGLE_PLAY, "MulticamReplay.TogglePlay", "Multicam Replay: play/pause");
	register_hotkey(HK_RESTART, "MulticamReplay.Restart", "Multicam Replay: restart");
	register_hotkey(HK_SPEED_100, "MulticamReplay.Speed100", "Multicam Replay: speed 100%");
	register_hotkey(HK_SPEED_75, "MulticamReplay.Speed75", "Multicam Replay: speed 75%");
	register_hotkey(HK_SPEED_50, "MulticamReplay.Speed50", "Multicam Replay: speed 50%");
	register_hotkey(HK_SPEED_25, "MulticamReplay.Speed25", "Multicam Replay: speed 25%");
	register_hotkey(HK_TOGGLE_DIRECTION, "MulticamReplay.ToggleDirection", "Multicam Replay: reverse direction");
	register_hotkey(HK_STEP_FORWARD, "MulticamReplay.StepForward", "Multicam Replay: step forward");
	register_hotkey(HK_STEP_BACKWARD, "MulticamReplay.StepBackward", "Multicam Replay: step backward");
	register_hotkey(HK_TOGGLE_LOOP, "MulticamReplay.ToggleLoop", "Multicam Replay: toggle loop");
	register_hotkey(HK_MARK_IN, "MulticamReplay.MarkIn", "Multicam Replay: mark in");
	register_hotkey(HK_MARK_OUT, "MulticamReplay.MarkOut", "Multicam Replay: mark out");
	register_hotkey(HK_MARK_RESET, "MulticamReplay.MarkReset", "Multicam Replay: reset marks");
	register_hotkey(HK_SAVE_HIGHLIGHT, "MulticamReplay.SaveHighlight", "Multicam Replay: save highlight");
	register_hotkey(HK_SAVE_SCORING, "MulticamReplay.SaveScoringPlay", "Multicam Replay: save scoring play");
	register_hotkey(HK_SAVE_FOUL, "MulticamReplay.SaveFoul", "Multicam Replay: save foul");
	register_hotkey(HK_SAVE_OTHER, "MulticamReplay.SaveOther", "Multicam Replay: save other");
	register_hotkey(HK_SAVE_ALL, "MulticamReplay.SaveAllAngles", "Multicam Replay: save all angles");
	register_hotkey(HK_PLAYLIST_ALL, "MulticamReplay.PlaylistAll", "Multicam Replay: play all saved");
	register_hotkey(HK_PLAYLIST_TOGGLE, "MulticamReplay.PlaylistTogglePlay", "Multicam Replay: playlist play/pause");
	register_hotkey(HK_PLAYLIST_NEXT, "MulticamReplay.PlaylistNext", "Multicam Replay: playlist next");
	register_hotkey(HK_PLAYLIST_PREVIOUS, "MulticamReplay.PlaylistPrevious", "Multicam Replay: playlist previous");
	register_hotkey(HK_PLAYLIST_STOP, "MulticamReplay.PlaylistStop", "Multicam Replay: playlist stop");

	g.initialized = true;
}

void replay_contract_shutdown(void)
{
	if (!g.initialized)
		return;
	pthread_mutex_lock(&g.mutex);
	g.shutting_down = true;
	os_atomic_inc_long(&g.load_gen);
	release_members_locked();
	stop_playlist_locked();
	pthread_mutex_unlock(&g.mutex);
	/* Detached load threads lock g.mutex when they finish; the decode aborts on the generation change. */
	for (int i = 0; i < 2000 && os_atomic_load_long(&g.load_threads) > 0; i++)
		os_sleep_ms(5);
	const bool loads_drained = os_atomic_load_long(&g.load_threads) == 0;
	if (!loads_drained)
		blog(LOG_WARNING, "[MulticamReplay] library load still running at shutdown; keeping its lock");

	obs_remove_tick_callback(playlist_tick, NULL);
	obs_frontend_remove_event_callback(frontend_event, NULL);
	replay_control_set_save_hook(NULL);
	replay_audio_capture_shutdown();
	replay_library_shutdown();
	for (size_t i = 0; i < g.hotkey_count; i++)
		if (g.hotkeys[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(g.hotkeys[i]);
	g.hotkey_count = 0;
	if (loads_drained)
		pthread_mutex_destroy(&g.mutex);
	g.initialized = false;
}

void replay_contract_register_vendor(void)
{
	g.vendor = obs_websocket_register_vendor(CONTRACT_VENDOR);
	if (!g.vendor) {
		blog(LOG_INFO, "[MulticamReplay] obs-websocket not available; vendor API disabled");
		return;
	}
	const char *requests[] = {"GetStatus",     "GetLibrary",      "Trigger",        "Release",
				  "Play",          "Pause",           "TogglePlay",     "Restart",
				  "SetSpeed",      "SetDirection",    "ToggleDirection","Step",
				  "Seek",          "SetLoop",         "MarkIn",         "MarkOut",
				  "MarkReset",     "SetSaveAngle",    "SetReplayAudio", "Save",
				  "SaveAllAngles", "CreateGame",      "SetActiveGame",  "SetTags",
				  "LoadLibraryEntry","PlayPlaylist",  "PlaylistControl"};
	for (size_t i = 0; i < sizeof requests / sizeof requests[0]; i++)
		obs_websocket_vendor_register_request(g.vendor, requests[i], vendor_request, (void *)requests[i]);
	blog(LOG_INFO, "[MulticamReplay] Registered vendor '%s' with %zu requests", CONTRACT_VENDOR,
	     sizeof requests / sizeof requests[0]);
}

void replay_contract_test_set_program_angle(const char *angle)
{
	pthread_mutex_lock(&g.mutex);
	strncpy(g.program_angle, angle ? angle : "", sizeof g.program_angle - 1);
	pthread_mutex_unlock(&g.mutex);
}
