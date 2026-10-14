#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "replay-control.h"
#include "replay-save-queue.h"
#include "obs-websocket-api.h"

struct replay_control_state {
	bool initialized;
	obs_hotkey_id load_all_hotkey;
	obs_hotkey_id all_normal_speed_hotkey;
	obs_hotkey_id all_half_speed_hotkey;
	obs_hotkey_id all_quarter_speed_hotkey;
	obs_hotkey_id all_restart_hotkey;
	obs_hotkey_id all_pause_hotkey;
	obs_hotkey_id all_mark_in_hotkey;
	obs_hotkey_id all_mark_out_hotkey;
	obs_hotkey_id all_mark_reset_hotkey;
	obs_websocket_vendor vendor;
};

static struct replay_control_state g_ctrl = {
	.load_all_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_normal_speed_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_half_speed_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_quarter_speed_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_restart_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_pause_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_mark_in_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_mark_out_hotkey = OBS_INVALID_HOTKEY_ID,
	.all_mark_reset_hotkey = OBS_INVALID_HOTKEY_ID,
	.vendor = NULL,
};

static const char *lookup_hotkey_desc(const char *locale_key, const char *fallback)
{
	const char *desc = obs_module_text(locale_key);
	if (!desc || !*desc || strcmp(desc, locale_key) == 0)
		return fallback;
	return desc;
}

typedef DARRAY(char *) string_list_t;
typedef DARRAY(obs_source_t *) source_list_t;

static bool enum_replay_sources_cb(void *param, obs_source_t *source)
{
	source_list_t *list = param;
	const char *id = obs_source_get_unversioned_id(source);
	if (id && strcmp(id, REPLAY_SOURCE_ID) == 0) {
		obs_source_t *ref = obs_source_get_ref(source);
		if (ref)
			da_push_back((*list), &ref);
	}
	return true;
}

static bool source_ptr_in_list(obs_source_t *const *arr, size_t count, const obs_source_t *needle)
{
	for (size_t i = 0; i < count; i++) {
		if (arr[i] == needle)
			return true;
	}
	return false;
}

static void push_name_obj(obs_data_array_t *arr, const char *name)
{
	if (!arr)
		return;
	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "name", name ? name : "");
	obs_data_array_push_back(arr, item);
	obs_data_release(item);
}

static void push_skipped_obj(obs_data_array_t *arr, const char *name, const char *reason)
{
	if (!arr)
		return;
	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "name", name ? name : "");
	obs_data_set_string(item, "reason", reason);
	obs_data_array_push_back(arr, item);
	obs_data_release(item);
}

static void free_string_darray(string_list_t *names)
{
	for (size_t i = 0; i < names->num; i++)
		bfree(names->array[i]);
	da_free((*names));
}

static void parse_sources_string(const char *raw, string_list_t *out)
{
	if (!raw || !*raw)
		return;

	const char *p = raw;
	while (*p == ' ' || *p == '\t' || *p == '[')
		p++;

	struct dstr token;
	dstr_init(&token);

	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (!*p || *p == ']')
			break;

		dstr_copy(&token, "");
		if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1])
					p++;
				dstr_cat_ch(&token, *p++);
			}
			if (*p == '"')
				p++;
		} else {
			while (*p && *p != ',' && *p != ']') {
				dstr_cat_ch(&token, *p++);
			}
			dstr_depad(&token);
		}

		if (token.len > 0) {
			char *copy = bstrdup(token.array);
			da_push_back((*out), &copy);
		}
	}

	dstr_free(&token);
}

/* Returns false when a target filter was given but yields no names, e.g. a JSON array of strings, which
 * obs-websocket drops while converting JSON to obs_data (only arrays of objects survive). */
static bool extract_target_names(obs_data_t *request_data, string_list_t *names_out, bool *has_filter_out)
{
	*has_filter_out = false;
	if (!request_data)
		return true;

	const char *single = obs_data_get_string(request_data, "source");
	if (single && *single) {
		*has_filter_out = true;
		char *copy = bstrdup(single);
		da_push_back((*names_out), &copy);
	}

	obs_data_array_t *arr = obs_data_get_array(request_data, "sources");
	if (arr) {
		*has_filter_out = true;
		const size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			if (!item)
				continue;
			const char *name = obs_data_get_string(item, "name");
			if (!name || !*name)
				name = obs_data_get_string(item, "source");
			if (!name || !*name)
				name = obs_data_get_string(item, "value");
			if (!name || !*name) {
				obs_data_item_t *first = obs_data_first(item);
				if (first && obs_data_item_gettype(first) == OBS_DATA_STRING)
					name = obs_data_item_get_string(first);
				if (first)
					obs_data_item_release(&first);
			}
			if (name && *name) {
				char *copy = bstrdup(name);
				da_push_back((*names_out), &copy);
			}
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	} else {
		const char *sources_str = obs_data_get_string(request_data, "sources");
		if (sources_str && *sources_str) {
			*has_filter_out = true;
			parse_sources_string(sources_str, names_out);
		}
	}
	return !*has_filter_out || names_out->num > 0;
}

static void set_invalid_sources_error(obs_data_t *response_data)
{
	obs_data_set_bool(response_data, "ok", false);
	obs_data_set_string(response_data, "error",
			    "sources: use a comma-separated string (\"Cam 1,Cam 2\") or [{\"name\": \"Cam 1\"}]");
}

static void resolve_target_sources(const char *const *filter_names, size_t filter_count, bool has_filter,
				   source_list_t *matched_out, obs_data_array_t *unknown_arr,
				   size_t *unknown_count_out)
{
	if (unknown_count_out)
		*unknown_count_out = 0;

	if (!has_filter) {
		obs_enum_sources(enum_replay_sources_cb, matched_out);
		return;
	}

	for (size_t i = 0; i < filter_count; i++) {
		const char *req_name = filter_names[i];
		if (!req_name || !*req_name)
			continue;
		obs_source_t *s = obs_get_source_by_name(req_name);
		if (s) {
			const char *id = obs_source_get_unversioned_id(s);
			if (id && strcmp(id, REPLAY_SOURCE_ID) == 0) {
				if (!source_ptr_in_list(matched_out->array, matched_out->num, s)) {
					da_push_back((*matched_out), &s);
				} else {
					obs_source_release(s);
				}
				continue;
			}
			obs_source_release(s);
		}
		push_name_obj(unknown_arr, req_name);
		if (unknown_count_out)
			(*unknown_count_out)++;
	}
}

struct load_all_entry {
	obs_source_t *source;
	struct replay_source *ctx;
	char *name;
	uint64_t first_ts;
	uint64_t last_ts;
	uint64_t start_ns;
	uint64_t duration_ns;
	int64_t trim_front_ns;
	int64_t trim_end_ns;
};

/* LoadAll: an angle whose newest frame is this far behind the freshest angle has stalled. */
#define LOAD_ALL_STALE_NS 1000000000ULL

static size_t replay_control_load_all_internal(const char *const *filter_names, size_t filter_count, bool has_filter,
					       obs_data_t *response_out)
{
	const uint64_t t_end = os_gettime_ns();

	obs_data_array_t *replays_arr = response_out ? obs_data_array_create() : NULL;
	obs_data_array_t *skipped_arr = response_out ? obs_data_array_create() : NULL;
	obs_data_array_t *unknown_arr = response_out ? obs_data_array_create() : NULL;
	size_t unknown_count = 0;
	size_t skipped_count = 0;

	source_list_t sources;
	da_init(sources);
	resolve_target_sources(filter_names, filter_count, has_filter, &sources, unknown_arr, &unknown_count);

	DARRAY(struct load_all_entry) loaded;
	da_init(loaded);

	for (size_t i = 0; i < sources.num; i++) {
		obs_source_t *s = sources.array[i];
		const char *sname = obs_source_get_name(s);
		struct replay_source *ctx = obs_obj_get_data(s);
		uint64_t first_ts = 0;
		uint64_t last_ts = 0;

		if (ctx && replay_source_load_unaligned(ctx, &first_ts, &last_ts)) {
			struct load_all_entry entry = {
				.source = s,
				.ctx = ctx,
				.name = bstrdup(sname ? sname : ""),
				.first_ts = first_ts,
				.last_ts = last_ts,
				.start_ns = first_ts,
				.duration_ns = (last_ts >= first_ts) ? (last_ts - first_ts) : 0,
				.trim_front_ns = 0,
				.trim_end_ns = 0,
			};
			da_push_back(loaded, &entry);
		} else {
			skipped_count++;
			push_skipped_obj(skipped_arr, sname ? sname : "", "empty");
			blog(LOG_INFO, "[replay_control] LoadAll skipped '%s' (buffer empty or source unconfigured)",
			     sname ? sname : "");
		}
	}

	/* One stalled angle (old newest frame) or one with a short history (just reconnected) would shrink the
	 * common window of every angle to nothing. Such angles are unloaded again and reported as skipped. */
	uint64_t newest_ns = 0;
	for (size_t i = 0; i < loaded.num; i++) {
		if (loaded.array[i].last_ts > newest_ns)
			newest_ns = loaded.array[i].last_ts;
	}
	uint64_t longest_ns = 0;
	for (size_t i = 0; i < loaded.num; i++) {
		const struct load_all_entry *e = &loaded.array[i];
		if (e->last_ts + LOAD_ALL_STALE_NS >= newest_ns && e->duration_ns > longest_ns)
			longest_ns = e->duration_ns;
	}
	size_t kept = 0;
	for (size_t i = 0; i < loaded.num; i++) {
		struct load_all_entry *e = &loaded.array[i];
		const char *reason = NULL;
		if (e->last_ts + LOAD_ALL_STALE_NS < newest_ns)
			reason = "stalled";
		else if (e->duration_ns < longest_ns / 2)
			reason = "short";
		if (reason) {
			replay_source_unload_newest(e->ctx);
			skipped_count++;
			push_skipped_obj(skipped_arr, e->name, reason);
			blog(LOG_INFO,
			     "[replay_control] LoadAll skipped '%s' (%s: %.2fs history ending %.2fs before newest angle)",
			     e->name, reason, (double)e->duration_ns / 1e9, (double)(newest_ns - e->last_ts) / 1e9);
			bfree(e->name);
			continue;
		}
		loaded.array[kept++] = *e;
	}
	loaded.num = kept;

	uint64_t common_start_ns = 0;
	uint64_t common_end_ns = 0;
	for (size_t i = 0; i < loaded.num; i++) {
		const struct load_all_entry *e = &loaded.array[i];
		if (i == 0 || e->first_ts > common_start_ns)
			common_start_ns = e->first_ts;
		if (i == 0 || e->last_ts < common_end_ns)
			common_end_ns = e->last_ts;
	}

	for (size_t i = 0; i < loaded.num; i++) {
		struct load_all_entry *entry = &loaded.array[i];
		replay_source_align_loaded_replay(entry->ctx, common_start_ns, common_end_ns, &entry->start_ns,
						  &entry->duration_ns, &entry->trim_front_ns, &entry->trim_end_ns);
		if (replays_arr) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", entry->name);
			obs_data_set_int(item, "startMs", (long long)(entry->start_ns / 1000000ULL));
			obs_data_set_int(item, "durationMs", (long long)(entry->duration_ns / 1000000ULL));
			obs_data_set_int(item, "trimFrontMs", (long long)(entry->trim_front_ns / 1000000LL));
			obs_data_set_int(item, "trimEndMs", (long long)(entry->trim_end_ns / 1000000LL));
			obs_data_array_push_back(replays_arr, item);
			obs_data_release(item);
		}
	}
	for (size_t i = 0; i < loaded.num; i++) {
		replay_source_auto_save_loaded(loaded.array[i].ctx, "auto-load");
	}


	for (size_t i = 0; i < sources.num; i++)
		obs_source_release(sources.array[i]);
	da_free(sources);

	if (response_out) {
		obs_data_set_bool(response_out, "ok", true);
		obs_data_set_int(response_out, "snapshotNs", (long long)t_end);
		obs_data_set_array(response_out, "replays", replays_arr);
		obs_data_set_array(response_out, "skipped", skipped_arr);
		obs_data_set_array(response_out, "unknown", unknown_arr);
		obs_data_set_int(response_out, "loadedCount", (long long)loaded.num);
		obs_data_set_int(response_out, "skippedCount", (long long)skipped_count);
		obs_data_set_int(response_out, "unknownCount", (long long)unknown_count);
		obs_data_array_release(replays_arr);
		obs_data_array_release(skipped_arr);
		obs_data_array_release(unknown_arr);
	}

	blog(LOG_INFO,
	     "[replay_control] LoadAll at t_end=%" PRIu64 " ns: loaded=%zu skipped=%zu common_window=[%" PRIu64
	     ", %" PRIu64 "] (%.2fs)",
	     t_end, loaded.num, skipped_count, common_start_ns, common_end_ns,
	     (common_end_ns > common_start_ns) ? ((double)(common_end_ns - common_start_ns) / 1e9) : 0.0);

	/* Emit vendor events outside all source locks */
	for (size_t i = 0; i < loaded.num; i++) {
		replay_control_emit_replay_loaded(loaded.array[i].name,
						  (int64_t)(loaded.array[i].duration_ns / 1000000ULL));
		bfree(loaded.array[i].name);
	}

	const size_t count = loaded.num;
	da_free(loaded);
	return count;
}

size_t replay_control_load_all(const char *const *filter_names, size_t filter_count, obs_data_t *response_out)
{
	const bool has_filter = (filter_names != NULL && filter_count > 0);
	return replay_control_load_all_internal(filter_names, filter_count, has_filter, response_out);
}

void replay_control_set_all_speed(float speed_percent)
{
	source_list_t sources;
	da_init(sources);
	obs_enum_sources(enum_replay_sources_cb, &sources);
	for (size_t i = 0; i < sources.num; i++) {
		struct replay_source *ctx = obs_obj_get_data(sources.array[i]);
		if (ctx)
			replay_source_set_speed(ctx, speed_percent);
		obs_source_release(sources.array[i]);
	}
	da_free(sources);
}

void replay_control_restart_all(void)
{
	source_list_t sources;
	da_init(sources);
	obs_enum_sources(enum_replay_sources_cb, &sources);
	for (size_t i = 0; i < sources.num; i++) {
		struct replay_source *ctx = obs_obj_get_data(sources.array[i]);
		if (ctx)
			replay_source_restart_playback(ctx);
		obs_source_release(sources.array[i]);
	}
	da_free(sources);
}

void replay_control_pause_all(void)
{
	source_list_t sources;
	da_init(sources);
	obs_enum_sources(enum_replay_sources_cb, &sources);
	for (size_t i = 0; i < sources.num; i++) {
		struct replay_source *ctx = obs_obj_get_data(sources.array[i]);
		if (ctx)
			replay_source_toggle_pause(ctx);
		obs_source_release(sources.array[i]);
	}
	da_free(sources);
}
static obs_source_t *find_reference_source(source_list_t *sources, const char *preferred_name)
{
	if (!sources || sources->num == 0)
		return NULL;

	obs_source_t *preferred = NULL;
	if (preferred_name && *preferred_name) {
		for (size_t i = 0; i < sources->num; i++) {
			const char *name = obs_source_get_name(sources->array[i]);
			if (name && strcmp(name, preferred_name) == 0) {
				preferred = sources->array[i];
				break;
			}
		}
	}

	/* Reference selection rule:
	 * 1. The replay source currently active in program if exactly one is active with a loaded replay.
	 * 2. Otherwise: first replay source that is playing or paused with a loaded replay. */
	obs_source_t *single_active = NULL;
	size_t active_loaded_count = 0;

	for (size_t i = 0; i < sources->num; i++) {
		obs_source_t *s = sources->array[i];
		struct replay_source *ctx = obs_obj_get_data(s);
		if (!ctx)
			continue;
		struct replay_source_status st;
		if (replay_source_get_status(ctx, &st) && st.loaded_replays > 0) {
			if (obs_source_active(s) || st.active) {
				single_active = s;
				active_loaded_count++;
			}
		}
	}

	if (active_loaded_count == 1 && single_active)
		return single_active;

	if (preferred) {
		struct replay_source *ctx = obs_obj_get_data(preferred);
		struct replay_source_status st;
		if (ctx && replay_source_get_status(ctx, &st) && st.loaded_replays > 0)
			return preferred;
	}

	for (size_t i = 0; i < sources->num; i++) {
		obs_source_t *s = sources->array[i];
		struct replay_source *ctx = obs_obj_get_data(s);
		if (!ctx)
			continue;
		struct replay_source_status st;
		if (replay_source_get_status(ctx, &st) && st.loaded_replays > 0)
			return s;
	}

	return NULL;
}

void replay_control_all_mark_in(void)
{
	source_list_t sources;
	da_init(sources);
	obs_enum_sources(enum_replay_sources_cb, &sources);

	obs_source_t *ref = find_reference_source(&sources, NULL);
	uint64_t ref_ts = 0;
	if (ref) {
		struct replay_source *ref_ctx = obs_obj_get_data(ref);
		if (ref_ctx && replay_source_get_current_playback_time(ref_ctx, &ref_ts)) {
			for (size_t i = 0; i < sources.num; i++) {
				struct replay_source *ctx = obs_obj_get_data(sources.array[i]);
				if (ctx)
					replay_source_set_mark_in(ctx, ref_ts);
			}
		}
	}

	for (size_t i = 0; i < sources.num; i++)
		obs_source_release(sources.array[i]);
	da_free(sources);
}

void replay_control_all_mark_out(void)
{
	source_list_t sources;
	da_init(sources);
	obs_enum_sources(enum_replay_sources_cb, &sources);

	obs_source_t *ref = find_reference_source(&sources, NULL);
	uint64_t ref_ts = 0;
	if (ref) {
		struct replay_source *ref_ctx = obs_obj_get_data(ref);
		if (ref_ctx && replay_source_get_current_playback_time(ref_ctx, &ref_ts)) {
			for (size_t i = 0; i < sources.num; i++) {
				struct replay_source *ctx = obs_obj_get_data(sources.array[i]);
				if (ctx)
					replay_source_set_mark_out(ctx, ref_ts);
			}
		}
	}

	for (size_t i = 0; i < sources.num; i++)
		obs_source_release(sources.array[i]);
	da_free(sources);
}

void replay_control_all_mark_reset(void)
{
	source_list_t sources;
	da_init(sources);
	obs_enum_sources(enum_replay_sources_cb, &sources);

	for (size_t i = 0; i < sources.num; i++) {
		struct replay_source *ctx = obs_obj_get_data(sources.array[i]);
		if (ctx)
			replay_source_mark_reset(ctx);
		obs_source_release(sources.array[i]);
	}
	da_free(sources);
}


/* ==================== FRONTEND HOTKEY CALLBACKS ==================== */

static void load_all_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_load_all(NULL, 0, NULL);
}

static void all_normal_speed_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_set_all_speed(100.0f);
}

static void all_half_speed_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_set_all_speed(50.0f);
}

static void all_quarter_speed_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_set_all_speed(25.0f);
}

static void all_restart_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_restart_all();
}

static void all_pause_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_pause_all();
}
static void all_mark_in_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_all_mark_in();
}

static void all_mark_out_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_all_mark_out();
}

static void all_mark_reset_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_control_all_mark_reset();
}


/* ==================== VENDOR REQUEST CALLBACKS ==================== */

static void vendor_req_get_replays(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	source_list_t sources;
	da_init(sources);
	obs_enum_sources(enum_replay_sources_cb, &sources);

	obs_data_array_t *replays_arr = obs_data_array_create();
	for (size_t i = 0; i < sources.num; i++) {
		obs_source_t *s = sources.array[i];
		struct replay_source *ctx = obs_obj_get_data(s);
		struct replay_source_status st;
		memset(&st, 0, sizeof(st));
		if (ctx && replay_source_get_status(ctx, &st)) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", st.name ? st.name : "");
			obs_data_set_string(item, "videoSource", st.video_source ? st.video_source : "");
			obs_data_set_int(item, "loadedReplays", st.loaded_replays);
			obs_data_set_bool(item, "active", st.active);
			obs_data_set_bool(item, "playing", st.playing);
			obs_data_set_double(item, "speedPercent", (double)st.speed_percent);
			obs_data_set_bool(item, "backward", st.backward);
			obs_data_set_int(item, "durationMs", (long long)st.duration_ms);
			obs_data_set_int(item, "bufferedMs", (long long)st.buffered_ms);
			obs_data_set_int(item, "markInMs", (long long)st.mark_in_ms);
			obs_data_set_int(item, "markOutMs", (long long)st.mark_out_ms);
			obs_data_array_push_back(replays_arr, item);
			obs_data_release(item);
		}
		obs_source_release(s);
	}
	da_free(sources);

	obs_data_t *sq = obs_data_create();
	obs_data_set_int(sq, "pending", (long long)replay_save_queue_pending_count());
	obs_data_set_int(sq, "completed", (long long)replay_save_queue_completed_count());

	obs_data_set_bool(response_data, "ok", true);
	obs_data_set_array(response_data, "replays", replays_arr);
	obs_data_set_obj(response_data, "saveQueue", sq);

	obs_data_array_release(replays_arr);
	obs_data_release(sq);
}

static void vendor_req_action(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	const char *action = request_data ? obs_data_get_string(request_data, "action") : NULL;
	if (!action || !*action || !replay_source_execute_action(NULL, action, false)) {
		obs_data_set_bool(response_data, "ok", false);
		obs_data_set_string(response_data, "error", "Unknown or missing action");
		return;
	}

	string_list_t filter_names;
	da_init(filter_names);
	bool has_filter = false;
	if (!extract_target_names(request_data, &filter_names, &has_filter)) {
		free_string_darray(&filter_names);
		set_invalid_sources_error(response_data);
		return;
	}

	obs_data_array_t *applied_arr = obs_data_array_create();
	obs_data_array_t *unknown_arr = obs_data_array_create();
	size_t unknown_count = 0;

	source_list_t sources;
	da_init(sources);
	resolve_target_sources((const char *const *)filter_names.array, filter_names.num, has_filter, &sources,
			       unknown_arr, &unknown_count);

	size_t applied_count = 0;
	for (size_t i = 0; i < sources.num; i++) {
		obs_source_t *s = sources.array[i];
		struct replay_source *ctx = obs_obj_get_data(s);
		if (ctx && replay_source_execute_action(ctx, action, false)) {
			push_name_obj(applied_arr, obs_source_get_name(s));
			applied_count++;
		}
		obs_source_release(s);
	}
	da_free(sources);
	free_string_darray(&filter_names);

	obs_data_set_bool(response_data, "ok", true);
	obs_data_set_string(response_data, "action", action);
	obs_data_set_array(response_data, "applied", applied_arr);
	obs_data_set_array(response_data, "unknown", unknown_arr);
	obs_data_set_int(response_data, "appliedCount", (long long)applied_count);
	obs_data_set_int(response_data, "unknownCount", (long long)unknown_count);
	obs_data_array_release(applied_arr);
	obs_data_array_release(unknown_arr);
}

static void vendor_req_set_speed(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	if (!request_data || !obs_data_has_user_value(request_data, "speedPercent")) {
		obs_data_set_bool(response_data, "ok", false);
		obs_data_set_string(response_data, "error", "Missing speedPercent");
		return;
	}

	const double speed = obs_data_get_double(request_data, "speedPercent");
	if (speed < (double)SETTING_SPEED_MIN || speed > (double)SETTING_SPEED_MAX) {
		obs_data_set_bool(response_data, "ok", false);
		obs_data_set_string(response_data, "error", "speedPercent out of range");
		return;
	}

	string_list_t filter_names;
	da_init(filter_names);
	bool has_filter = false;
	if (!extract_target_names(request_data, &filter_names, &has_filter)) {
		free_string_darray(&filter_names);
		set_invalid_sources_error(response_data);
		return;
	}

	obs_data_array_t *applied_arr = obs_data_array_create();
	obs_data_array_t *unknown_arr = obs_data_array_create();
	size_t unknown_count = 0;

	source_list_t sources;
	da_init(sources);
	resolve_target_sources((const char *const *)filter_names.array, filter_names.num, has_filter, &sources,
			       unknown_arr, &unknown_count);

	size_t applied_count = 0;
	for (size_t i = 0; i < sources.num; i++) {
		obs_source_t *s = sources.array[i];
		struct replay_source *ctx = obs_obj_get_data(s);
		if (ctx) {
			replay_source_set_speed(ctx, (float)speed);
			push_name_obj(applied_arr, obs_source_get_name(s));
			applied_count++;
		}
		obs_source_release(s);
	}
	da_free(sources);
	free_string_darray(&filter_names);

	obs_data_set_bool(response_data, "ok", true);
	obs_data_set_double(response_data, "speedPercent", speed);
	obs_data_set_array(response_data, "applied", applied_arr);
	obs_data_set_array(response_data, "unknown", unknown_arr);
	obs_data_set_int(response_data, "appliedCount", (long long)applied_count);
	obs_data_set_int(response_data, "unknownCount", (long long)unknown_count);
	obs_data_array_release(applied_arr);
	obs_data_array_release(unknown_arr);
}

static void vendor_req_load_all(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	string_list_t filter_names;
	da_init(filter_names);
	bool has_filter = false;
	if (!extract_target_names(request_data, &filter_names, &has_filter)) {
		free_string_darray(&filter_names);
		set_invalid_sources_error(response_data);
		return;
	}

	replay_control_load_all_internal((const char *const *)filter_names.array, filter_names.num, has_filter,
					 response_data);
	free_string_darray(&filter_names);
}

static void vendor_req_save_all(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	const size_t queued = replay_save_all_loaded_replays();
	obs_data_set_bool(response_data, "ok", true);
	obs_data_set_int(response_data, "queued", (long long)queued);
}
static void vendor_req_mark_in(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	string_list_t filter_names;
	da_init(filter_names);
	bool has_filter = false;
	if (!extract_target_names(request_data, &filter_names, &has_filter)) {
		free_string_darray(&filter_names);
		set_invalid_sources_error(response_data);
		return;
	}

	obs_data_array_t *applied_arr = obs_data_array_create();
	obs_data_array_t *sources_arr = obs_data_array_create();
	obs_data_array_t *unknown_arr = obs_data_array_create();
	size_t unknown_count = 0;

	source_list_t targets;
	da_init(targets);
	resolve_target_sources((const char *const *)filter_names.array, filter_names.num, has_filter, &targets,
			       unknown_arr, &unknown_count);

	source_list_t all_sources;
	da_init(all_sources);
	obs_enum_sources(enum_replay_sources_cb, &all_sources);

	const char *preferred = (has_filter && filter_names.num == 1) ? filter_names.array[0] : NULL;
	obs_source_t *ref = find_reference_source(&all_sources, preferred);
	uint64_t ref_ts = 0;
	bool got_ts = false;
	if (ref) {
		struct replay_source *ref_ctx = obs_obj_get_data(ref);
		if (ref_ctx && replay_source_get_current_playback_time(ref_ctx, &ref_ts))
			got_ts = true;
	}

	size_t applied_count = 0;
	if (got_ts) {
		for (size_t i = 0; i < targets.num; i++) {
			obs_source_t *s = targets.array[i];
			struct replay_source *ctx = obs_obj_get_data(s);
			if (ctx && replay_source_set_mark_in(ctx, ref_ts)) {
				int64_t in_ms = 0, out_ms = 0, dur_ms = 0;
				replay_source_get_marks_ms(ctx, &in_ms, &out_ms, &dur_ms);
				obs_data_t *item = obs_data_create();
				obs_data_set_string(item, "name", obs_source_get_name(s));
				obs_data_set_int(item, "inMs", (long long)in_ms);
				obs_data_set_int(item, "outMs", (long long)out_ms);
				obs_data_set_int(item, "durationMs", (long long)dur_ms);
				obs_data_array_push_back(sources_arr, item);
				obs_data_release(item);

				push_name_obj(applied_arr, obs_source_get_name(s));
				applied_count++;
			}
		}
	}

	for (size_t i = 0; i < targets.num; i++)
		obs_source_release(targets.array[i]);
	da_free(targets);
	for (size_t i = 0; i < all_sources.num; i++)
		obs_source_release(all_sources.array[i]);
	da_free(all_sources);
	free_string_darray(&filter_names);

	obs_data_set_bool(response_data, "ok", true);
	obs_data_set_array(response_data, "sources", sources_arr);
	obs_data_set_array(response_data, "replays", sources_arr);
	obs_data_set_array(response_data, "applied", applied_arr);
	obs_data_set_array(response_data, "unknown", unknown_arr);
	obs_data_set_int(response_data, "appliedCount", (long long)applied_count);
	obs_data_set_int(response_data, "unknownCount", (long long)unknown_count);
	obs_data_array_release(sources_arr);
	obs_data_array_release(applied_arr);
	obs_data_array_release(unknown_arr);
}

static void vendor_req_mark_out(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	string_list_t filter_names;
	da_init(filter_names);
	bool has_filter = false;
	if (!extract_target_names(request_data, &filter_names, &has_filter)) {
		free_string_darray(&filter_names);
		set_invalid_sources_error(response_data);
		return;
	}

	obs_data_array_t *applied_arr = obs_data_array_create();
	obs_data_array_t *sources_arr = obs_data_array_create();
	obs_data_array_t *unknown_arr = obs_data_array_create();
	size_t unknown_count = 0;

	source_list_t targets;
	da_init(targets);
	resolve_target_sources((const char *const *)filter_names.array, filter_names.num, has_filter, &targets,
			       unknown_arr, &unknown_count);

	source_list_t all_sources;
	da_init(all_sources);
	obs_enum_sources(enum_replay_sources_cb, &all_sources);

	const char *preferred = (has_filter && filter_names.num == 1) ? filter_names.array[0] : NULL;
	obs_source_t *ref = find_reference_source(&all_sources, preferred);
	uint64_t ref_ts = 0;
	bool got_ts = false;
	if (ref) {
		struct replay_source *ref_ctx = obs_obj_get_data(ref);
		if (ref_ctx && replay_source_get_current_playback_time(ref_ctx, &ref_ts))
			got_ts = true;
	}

	size_t applied_count = 0;
	if (got_ts) {
		for (size_t i = 0; i < targets.num; i++) {
			obs_source_t *s = targets.array[i];
			struct replay_source *ctx = obs_obj_get_data(s);
			if (ctx && replay_source_set_mark_out(ctx, ref_ts)) {
				int64_t in_ms = 0, out_ms = 0, dur_ms = 0;
				replay_source_get_marks_ms(ctx, &in_ms, &out_ms, &dur_ms);
				obs_data_t *item = obs_data_create();
				obs_data_set_string(item, "name", obs_source_get_name(s));
				obs_data_set_int(item, "inMs", (long long)in_ms);
				obs_data_set_int(item, "outMs", (long long)out_ms);
				obs_data_set_int(item, "durationMs", (long long)dur_ms);
				obs_data_array_push_back(sources_arr, item);
				obs_data_release(item);

				push_name_obj(applied_arr, obs_source_get_name(s));
				applied_count++;
			}
		}
	}

	for (size_t i = 0; i < targets.num; i++)
		obs_source_release(targets.array[i]);
	da_free(targets);
	for (size_t i = 0; i < all_sources.num; i++)
		obs_source_release(all_sources.array[i]);
	da_free(all_sources);
	free_string_darray(&filter_names);

	obs_data_set_bool(response_data, "ok", true);
	obs_data_set_array(response_data, "sources", sources_arr);
	obs_data_set_array(response_data, "replays", sources_arr);
	obs_data_set_array(response_data, "applied", applied_arr);
	obs_data_set_array(response_data, "unknown", unknown_arr);
	obs_data_set_int(response_data, "appliedCount", (long long)applied_count);
	obs_data_set_int(response_data, "unknownCount", (long long)unknown_count);
	obs_data_array_release(sources_arr);
	obs_data_array_release(applied_arr);
	obs_data_array_release(unknown_arr);
}

static void vendor_req_mark_reset(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	string_list_t filter_names;
	da_init(filter_names);
	bool has_filter = false;
	if (!extract_target_names(request_data, &filter_names, &has_filter)) {
		free_string_darray(&filter_names);
		set_invalid_sources_error(response_data);
		return;
	}

	obs_data_array_t *applied_arr = obs_data_array_create();
	obs_data_array_t *sources_arr = obs_data_array_create();
	obs_data_array_t *unknown_arr = obs_data_array_create();
	size_t unknown_count = 0;

	source_list_t targets;
	da_init(targets);
	resolve_target_sources((const char *const *)filter_names.array, filter_names.num, has_filter, &targets,
			       unknown_arr, &unknown_count);

	size_t applied_count = 0;
	for (size_t i = 0; i < targets.num; i++) {
		obs_source_t *s = targets.array[i];
		struct replay_source *ctx = obs_obj_get_data(s);
		if (ctx) {
			replay_source_mark_reset(ctx);
			int64_t in_ms = 0, out_ms = 0, dur_ms = 0;
			replay_source_get_marks_ms(ctx, &in_ms, &out_ms, &dur_ms);
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", obs_source_get_name(s));
			obs_data_set_int(item, "inMs", (long long)in_ms);
			obs_data_set_int(item, "outMs", (long long)out_ms);
			obs_data_set_int(item, "durationMs", (long long)dur_ms);
			obs_data_array_push_back(sources_arr, item);
			obs_data_release(item);

			push_name_obj(applied_arr, obs_source_get_name(s));
			applied_count++;
		}
	}

	for (size_t i = 0; i < targets.num; i++)
		obs_source_release(targets.array[i]);
	da_free(targets);
	free_string_darray(&filter_names);

	obs_data_set_bool(response_data, "ok", true);
	obs_data_set_array(response_data, "sources", sources_arr);
	obs_data_set_array(response_data, "replays", sources_arr);
	obs_data_set_array(response_data, "applied", applied_arr);
	obs_data_set_array(response_data, "unknown", unknown_arr);
	obs_data_set_int(response_data, "appliedCount", (long long)applied_count);
	obs_data_set_int(response_data, "unknownCount", (long long)unknown_count);
	obs_data_array_release(sources_arr);
	obs_data_array_release(applied_arr);
	obs_data_array_release(unknown_arr);
}


/* ==================== VENDOR EVENTS ==================== */

void replay_control_emit_replay_loaded(const char *source_name, int64_t duration_ms)
{
	if (!g_ctrl.vendor)
		return;
	obs_data_t *ev = obs_data_create();
	obs_data_set_string(ev, "source", source_name ? source_name : "");
	obs_data_set_int(ev, "durationMs", (long long)duration_ms);
	obs_websocket_vendor_emit_event(g_ctrl.vendor, "ReplayLoaded", ev);
	obs_data_release(ev);
}

void replay_control_emit_save_started(const char *source_name, const char *path, const char *trigger)
{
	if (!g_ctrl.vendor)
		return;
	obs_data_t *ev = obs_data_create();
	obs_data_set_string(ev, "source", source_name ? source_name : "");
	obs_data_set_string(ev, "path", path ? path : "");
	obs_data_set_string(ev, "trigger", (trigger && *trigger) ? trigger : "manual");
	obs_websocket_vendor_emit_event(g_ctrl.vendor, "SaveStarted", ev);
	obs_data_release(ev);
}

void replay_control_emit_save_finished(const char *source_name, const char *path, bool ok)
{
	if (!g_ctrl.vendor)
		return;
	obs_data_t *ev = obs_data_create();
	obs_data_set_string(ev, "source", source_name ? source_name : "");
	obs_data_set_string(ev, "path", path ? path : "");
	obs_data_set_bool(ev, "ok", ok);
	/* Companion's "Custom - Vendor Event" feedback compares values as strings only */
	obs_data_set_string(ev, "status", ok ? "ok" : "failed");
	obs_websocket_vendor_emit_event(g_ctrl.vendor, "SaveFinished", ev);
	obs_data_release(ev);
}

/* ==================== MODULE LIFECYCLE ==================== */

void replay_control_init(void)
{
	if (g_ctrl.initialized)
		return;

	g_ctrl.load_all_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.LoadAll", lookup_hotkey_desc("LoadAllReplays", "Replay: load all angles"),
		load_all_hotkey_cb, NULL);
	g_ctrl.all_normal_speed_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllNormalSpeed",
		lookup_hotkey_desc("AllNormalSpeed", "Replay: all angles normal speed (100%)"),
		all_normal_speed_hotkey_cb, NULL);
	g_ctrl.all_half_speed_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllHalfSpeed",
		lookup_hotkey_desc("AllHalfSpeed", "Replay: all angles half speed (50%)"), all_half_speed_hotkey_cb,
		NULL);
	g_ctrl.all_quarter_speed_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllQuarterSpeed",
		lookup_hotkey_desc("AllQuarterSpeed", "Replay: all angles quarter speed (25%)"),
		all_quarter_speed_hotkey_cb, NULL);
	g_ctrl.all_restart_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllRestart", lookup_hotkey_desc("AllRestart", "Replay: restart all angles"),
		all_restart_hotkey_cb, NULL);
	g_ctrl.all_pause_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllPause", lookup_hotkey_desc("AllPause", "Replay: pause/play all angles"),
		all_pause_hotkey_cb, NULL);
	g_ctrl.all_mark_in_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllMarkIn", lookup_hotkey_desc("AllMarkIn", "Replay: mark in (all angles)"),
		all_mark_in_hotkey_cb, NULL);
	g_ctrl.all_mark_out_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllMarkOut", lookup_hotkey_desc("AllMarkOut", "Replay: mark out (all angles)"),
		all_mark_out_hotkey_cb, NULL);
	g_ctrl.all_mark_reset_hotkey = obs_hotkey_register_frontend(
		"ReplaySource.AllMarkReset", lookup_hotkey_desc("AllMarkReset", "Replay: reset marks (all angles)"),
		all_mark_reset_hotkey_cb, NULL);

	g_ctrl.initialized = true;
}

static void unregister_hotkey_slot(obs_hotkey_id *slot)
{
	if (*slot != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(*slot);
		*slot = OBS_INVALID_HOTKEY_ID;
	}
}

void replay_control_shutdown(void)
{
	unregister_hotkey_slot(&g_ctrl.load_all_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_normal_speed_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_half_speed_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_quarter_speed_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_restart_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_pause_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_mark_in_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_mark_out_hotkey);
	unregister_hotkey_slot(&g_ctrl.all_mark_reset_hotkey);

	g_ctrl.vendor = NULL;
	_ph = NULL;
	g_ctrl.initialized = false;
}

void replay_control_register_vendor(void)
{
	_ph = NULL;
	g_ctrl.vendor = obs_websocket_register_vendor(REPLAY_WS_VENDOR_NAME);
	if (!g_ctrl.vendor) {
		blog(LOG_INFO, "[Replay Source] obs-websocket not available; vendor API '%s' disabled",
		     REPLAY_WS_VENDOR_NAME);
		return;
	}

	obs_websocket_vendor_register_request(g_ctrl.vendor, "GetReplays", vendor_req_get_replays, NULL);
	obs_websocket_vendor_register_request(g_ctrl.vendor, "Action", vendor_req_action, NULL);
	obs_websocket_vendor_register_request(g_ctrl.vendor, "SetSpeed", vendor_req_set_speed, NULL);
	obs_websocket_vendor_register_request(g_ctrl.vendor, "LoadAll", vendor_req_load_all, NULL);
	obs_websocket_vendor_register_request(g_ctrl.vendor, "SaveAll", vendor_req_save_all, NULL);
	obs_websocket_vendor_register_request(g_ctrl.vendor, "MarkIn", vendor_req_mark_in, NULL);
	obs_websocket_vendor_register_request(g_ctrl.vendor, "MarkOut", vendor_req_mark_out, NULL);
	obs_websocket_vendor_register_request(g_ctrl.vendor, "MarkReset", vendor_req_mark_reset, NULL);

	blog(LOG_INFO,
	     "[Replay Source] Registered obs-websocket vendor '%s' (GetReplays, Action, SetSpeed, LoadAll, SaveAll, MarkIn, MarkOut, MarkReset)",
	     REPLAY_WS_VENDOR_NAME);
}
