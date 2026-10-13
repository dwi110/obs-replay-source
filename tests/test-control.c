#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <media-io/video-frame.h>

#include "replay.h"
#include "replay-control.h"
#include "replay-save-queue.h"
#include "obs-websocket-api.h"

extern bool obs_module_load(void);
extern void obs_module_post_load(void);
extern void obs_module_unload(void);

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			return 1;                                              \
		}                                                              \
	} while (0)

static const char *const g_expected_legacy_hotkeys[] = {
	"ReplaySource.Replay",
	"ReplaySource.Next",
	"ReplaySource.Previous",
	"ReplaySource.First",
	"ReplaySource.Last",
	"ReplaySource.Remove",
	"ReplaySource.Clear",
	"ReplaySource.Save",
	"ReplaySource.Restart",
	"ReplaySource.Pause",
	"ReplaySource.Faster",
	"ReplaySource.Slower",
	"ReplaySource.FasterBy5",
	"ReplaySource.SlowerBy5",
	"ReplaySource.NormalOrFaster",
	"ReplaySource.NormalOrSlower",
	"ReplaySource.NormalSpeed",
	"ReplaySource.HalfSpeed",
	"ReplaySource.DoubleSpeed",
	"ReplaySource.TrimFront",
	"ReplaySource.TrimEnd",
	"ReplaySource.TrimReset",
	"ReplaySource.Reverse",
	"ReplaySource.Forward",
	"ReplaySource.Backward",
	"ReplaySource.ForwardOrFaster",
	"ReplaySource.BackwardOrFaster",
	"ReplaySource.Disable",
	"ReplaySource.Enable",
	"ReplaySource.DisableNextScene",
	"ReplaySource.EnableNextScene",
	"ReplaySource.NextSceneCurrent",
	"ReplaySource.NextScene",
	"ReplaySource.PrevFrame",
	"ReplaySource.NextFrame",
	"ReplaySource.PrevNFrames",
	"ReplaySource.NextNFrames",
};

#define EXPECTED_LEGACY_HOTKEY_COUNT (sizeof(g_expected_legacy_hotkeys) / sizeof(g_expected_legacy_hotkeys[0]))

/* Dummy async camera source for feeding synthetic frames into replay_filter_async */
static const char *dummy_cam_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Dummy Async Camera";
}

static void *dummy_cam_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	return source;
}

static void dummy_cam_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static struct obs_source_info dummy_cam_info = {
	.id = "dummy_async_cam",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = dummy_cam_get_name,
	.create = dummy_cam_create,
	.destroy = dummy_cam_destroy,
};

/* Fake obs-websocket proc handler */
struct fake_ws_req_entry {
	char *type;
	obs_websocket_request_callback_function callback;
	void *priv_data;
};

struct fake_ws_event_entry {
	char *type;
	char *source;
	int64_t duration_ms;
};

struct fake_ws_state {
	proc_handler_t *ph;
	int vendor_token;
	char *vendor_name;
	DARRAY(struct fake_ws_req_entry) requests;
	DARRAY(struct fake_ws_event_entry) events;
};

static struct fake_ws_state g_fake_ws;

static void fake_ws_get_ph_proc(void *data, calldata_t *cd)
{
	struct fake_ws_state *ws = data;
	calldata_set_ptr(cd, "ph", ws->ph);
}

static void fake_ws_vendor_register_proc(void *data, calldata_t *cd)
{
	struct fake_ws_state *ws = data;
	const char *name = calldata_string(cd, "name");
	bfree(ws->vendor_name);
	ws->vendor_name = bstrdup(name ? name : "");
	calldata_set_ptr(cd, "vendor", &ws->vendor_token);
	calldata_set_bool(cd, "success", true);
}

static void fake_ws_vendor_request_register_proc(void *data, calldata_t *cd)
{
	struct fake_ws_state *ws = data;
	void *vendor = calldata_ptr(cd, "vendor");
	const char *type = calldata_string(cd, "type");
	struct obs_websocket_request_callback *cb = calldata_ptr(cd, "callback");
	if (vendor != &ws->vendor_token || !type || !cb || !cb->callback) {
		calldata_set_bool(cd, "success", false);
		return;
	}
	struct fake_ws_req_entry entry = {
		.type = bstrdup(type),
		.callback = cb->callback,
		.priv_data = cb->priv_data,
	};
	da_push_back(ws->requests, &entry);
	calldata_set_bool(cd, "success", true);
}

static void fake_ws_vendor_event_emit_proc(void *data, calldata_t *cd)
{
	struct fake_ws_state *ws = data;
	void *vendor = calldata_ptr(cd, "vendor");
	const char *type = calldata_string(cd, "type");
	obs_data_t *ev_data = calldata_ptr(cd, "data");
	if (vendor != &ws->vendor_token || !type || !ev_data) {
		calldata_set_bool(cd, "success", false);
		return;
	}
	const char *src = obs_data_get_string(ev_data, "source");
	struct fake_ws_event_entry ev = {
		.type = bstrdup(type),
		.source = bstrdup(src ? src : ""),
		.duration_ms = (int64_t)obs_data_get_int(ev_data, "durationMs"),
	};
	da_push_back(ws->events, &ev);
	calldata_set_bool(cd, "success", true);
}

static const struct fake_ws_req_entry *find_ws_request(const char *type)
{
	for (size_t i = 0; i < g_fake_ws.requests.num; i++) {
		if (strcmp(g_fake_ws.requests.array[i].type, type) == 0)
			return &g_fake_ws.requests.array[i];
	}
	return NULL;
}

static bool call_ws_request(const char *type, obs_data_t *req, obs_data_t *res)
{
	const struct fake_ws_req_entry *entry = find_ws_request(type);
	if (!entry)
		return false;
	entry->callback(req, res, entry->priv_data);
	return true;
}

/* Hotkey enumeration helpers */
struct hotkey_snapshot {
	DARRAY(char *) names;
	DARRAY(obs_hotkey_id) ids;
};

static bool collect_hotkeys_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	struct hotkey_snapshot *snap = data;
	const char *name = obs_hotkey_get_name(key);
	char *copy = bstrdup(name ? name : "");
	da_push_back(snap->names, &copy);
	da_push_back(snap->ids, &id);
	return true;
}

static void free_hotkey_snapshot(struct hotkey_snapshot *snap)
{
	for (size_t i = 0; i < snap->names.num; i++)
		bfree(snap->names.array[i]);
	da_free(snap->names);
	da_free(snap->ids);
}

static size_t count_hotkey_name(const struct hotkey_snapshot *snap, const char *name)
{
	size_t count = 0;
	for (size_t i = 0; i < snap->names.num; i++) {
		if (strcmp(snap->names.array[i], name) == 0)
			count++;
	}
	return count;
}

static obs_hotkey_id find_hotkey_id_by_name(const struct hotkey_snapshot *snap, const char *name)
{
	for (size_t i = 0; i < snap->names.num; i++) {
		if (strcmp(snap->names.array[i], name) == 0)
			return snap->ids.array[i];
	}
	return OBS_INVALID_HOTKEY_ID;
}

struct binding_query {
	obs_hotkey_id target_id;
	size_t count;
	obs_key_combination_t first_combo;
};

static bool query_binding_cb(void *data, size_t idx, obs_hotkey_binding_t *binding)
{
	UNUSED_PARAMETER(idx);
	struct binding_query *q = data;
	if (obs_hotkey_binding_get_hotkey_id(binding) == q->target_id) {
		if (q->count == 0)
			q->first_combo = obs_hotkey_binding_get_key_combination(binding);
		q->count++;
	}
	return true;
}

extern struct obs_source_info replay_filter_async_info;

static void feed_async_filter_cb(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	struct obs_source_frame *frame = param;
	const char *id = obs_source_get_unversioned_id(filter);
	if (id && strcmp(id, REPLAY_FILTER_ASYNC_ID) == 0) {
		void *data = obs_obj_get_data(filter);
		if (data && replay_filter_async_info.filter_video)
			replay_filter_async_info.filter_video(data, frame);
	}
}

static void feed_nv12_frames(obs_source_t *cam, uint32_t width, uint32_t height, uint64_t count, uint64_t start_ts,
			     uint64_t interval_ns)
{
	struct obs_source_frame frame;
	memset(&frame, 0, sizeof(frame));
	video_frame_init((struct video_frame *)&frame, VIDEO_FORMAT_NV12, width, height);
	frame.width = width;
	frame.height = height;
	frame.format = VIDEO_FORMAT_NV12;
	memset(frame.data[0], 0x50, (size_t)frame.linesize[0] * height);
	memset(frame.data[1], 0x80, (size_t)frame.linesize[1] * (height / 2));

	for (uint64_t i = 0; i < count; i++) {
		frame.timestamp = start_ts + i * interval_ns;
		obs_source_enum_filters(cam, feed_async_filter_cb, &frame);
	}

	video_frame_free((struct video_frame *)&frame);
}

int main(int argc, char **argv)
{
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("[test-control] No DISPLAY set, re-executing under xvfb-run -a...\n");
		char **new_argv = calloc((size_t)argc + 3, sizeof(char *));
		new_argv[0] = "xvfb-run";
		new_argv[1] = "-a";
		for (int i = 0; i < argc; i++)
			new_argv[i + 2] = argv[i];
		new_argv[argc + 2] = NULL;
		execvp("xvfb-run", new_argv);
		perror("execvp xvfb-run");
		return 1;
	}

	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);

	CHECK(obs_startup("en-US", NULL, NULL), "obs_startup failed");
	obs_add_data_path("/usr/share/obs/libobs/");

	struct obs_video_info ovi = {
		.graphics_module = "libobs-opengl.so.30",
		.fps_num = 60,
		.fps_den = 1,
		.base_width = 320,
		.base_height = 180,
		.output_width = 320,
		.output_height = 180,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = 0,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BILINEAR,
	};
	int vret = obs_reset_video(&ovi);
	if (vret != OBS_VIDEO_SUCCESS) {
		ovi.graphics_module = "libobs-opengl";
		vret = obs_reset_video(&ovi);
	}
	CHECK(vret == OBS_VIDEO_SUCCESS, "obs_reset_video failed: %d", vret);

	struct obs_audio_info oai = {
		.samples_per_sec = 48000,
		.speakers = SPEAKERS_STEREO,
	};
	CHECK(obs_reset_audio(&oai), "obs_reset_audio failed");

	obs_register_source(&dummy_cam_info);

	/* Set up fake obs-websocket proc handler before loading module */
	memset(&g_fake_ws, 0, sizeof(g_fake_ws));
	g_fake_ws.ph = proc_handler_create();
	g_fake_ws.vendor_token = 0x52504C59;
	da_init(g_fake_ws.requests);
	da_init(g_fake_ws.events);

	proc_handler_add(obs_get_proc_handler(), "void obs_websocket_api_get_ph(out ptr ph)", fake_ws_get_ph_proc,
			 &g_fake_ws);
	proc_handler_add(g_fake_ws.ph, "void vendor_register(in string name, out ptr vendor, out bool success)",
			 fake_ws_vendor_register_proc, &g_fake_ws);
	proc_handler_add(g_fake_ws.ph,
			 "void vendor_request_register(in ptr vendor, in string type, in ptr callback, out bool success)",
			 fake_ws_vendor_request_register_proc, &g_fake_ws);
	proc_handler_add(g_fake_ws.ph,
			 "void vendor_event_emit(in ptr vendor, in string type, in ptr data, out bool success)",
			 fake_ws_vendor_event_emit_proc, &g_fake_ws);

	CHECK(obs_module_load(), "obs_module_load failed");
	obs_module_post_load();

	/* =====================================================================
	 * 1. Hotkeys: 3 replay sources -> unique names, rename, and migration
	 * ===================================================================== */
	obs_source_t *rs1 = obs_source_create(REPLAY_SOURCE_ID, "Replay Cam 1", NULL, NULL);
	obs_source_t *rs2 = obs_source_create(REPLAY_SOURCE_ID, "Replay Cam 2", NULL, NULL);

	/* Prepare legacy hotkey_data with a binding under "ReplaySource.Save" for rs3 */
	obs_data_t *legacy_hk_data = obs_data_create();
	obs_data_array_t *legacy_save_bindings = obs_data_array_create();
	obs_data_t *binding_item = obs_data_create();
	obs_data_set_string(binding_item, "key", "OBS_KEY_F9");
	obs_data_set_bool(binding_item, "control", true);
	obs_data_array_push_back(legacy_save_bindings, binding_item);
	obs_data_release(binding_item);
	obs_data_set_array(legacy_hk_data, "ReplaySource.Save", legacy_save_bindings);
	obs_data_array_release(legacy_save_bindings);

	obs_source_t *rs3 = obs_source_create(REPLAY_SOURCE_ID, "Replay Cam 3", NULL, legacy_hk_data);
	obs_data_release(legacy_hk_data);

	CHECK(rs1 && rs2 && rs3, "failed to create 3 replay sources");

	struct hotkey_snapshot snap = {0};
	obs_enum_hotkeys(collect_hotkeys_cb, &snap);

	/* Verify global frontend hotkeys exist exactly once */
	const char *const expected_global_hotkeys[] = {
		"ReplaySource.SaveAll",        "ReplaySource.LoadAll",         "ReplaySource.AllNormalSpeed",
		"ReplaySource.AllHalfSpeed",   "ReplaySource.AllQuarterSpeed", "ReplaySource.AllRestart",
		"ReplaySource.AllPause",
	};
	for (size_t i = 0; i < sizeof(expected_global_hotkeys) / sizeof(expected_global_hotkeys[0]); i++) {
		CHECK(count_hotkey_name(&snap, expected_global_hotkeys[i]) == 1,
		      "expected global hotkey '%s' exactly once", expected_global_hotkeys[i]);
	}

	/* Verify every per-source hotkey exists with <legacy>.<source_name>, is unique, and no bare legacy name remains */
	const char *const source_names[3] = {"Replay Cam 1", "Replay Cam 2", "Replay Cam 3"};
	struct dstr expected_name;
	dstr_init(&expected_name);
	for (size_t k = 0; k < EXPECTED_LEGACY_HOTKEY_COUNT; k++) {
		const char *legacy = g_expected_legacy_hotkeys[k];
		CHECK(count_hotkey_name(&snap, legacy) == 0, "legacy hotkey '%s' must not remain registered", legacy);
		for (size_t s = 0; s < 3; s++) {
			dstr_printf(&expected_name, "%s.%s", legacy, source_names[s]);
			CHECK(count_hotkey_name(&snap, expected_name.array) == 1,
			      "expected per-source hotkey '%s' to be registered exactly once", expected_name.array);
		}
	}
	dstr_free(&expected_name);

	/* Verify migration: ReplaySource.Save.Replay Cam 3 inherited the Ctrl+F9 binding from ReplaySource.Save */
	obs_hotkey_id migrated_save_id = find_hotkey_id_by_name(&snap, "ReplaySource.Save.Replay Cam 3");
	CHECK(migrated_save_id != OBS_INVALID_HOTKEY_ID, "ReplaySource.Save.Replay Cam 3 hotkey not found");
	struct binding_query bq = {.target_id = migrated_save_id, .count = 0};
	obs_enum_hotkey_bindings(query_binding_cb, &bq);
	CHECK(bq.count == 1, "expected 1 migrated binding on ReplaySource.Save.Replay Cam 3, got %zu", bq.count);
	CHECK(bq.first_combo.key == obs_key_from_name("OBS_KEY_F9"),
	      "expected migrated key OBS_KEY_F9, got %d", (int)bq.first_combo.key);
	CHECK((bq.first_combo.modifiers & INTERACT_CONTROL_KEY) != 0, "expected control modifier on migrated binding");
	free_hotkey_snapshot(&snap);

	/* Verify rename: rename Replay Cam 2 -> Replay Angle B and check all 37 hotkey names follow */
	obs_source_set_name(rs2, "Replay Angle B");
	memset(&snap, 0, sizeof(snap));
	obs_enum_hotkeys(collect_hotkeys_cb, &snap);
	dstr_init(&expected_name);
	for (size_t k = 0; k < EXPECTED_LEGACY_HOTKEY_COUNT; k++) {
		const char *legacy = g_expected_legacy_hotkeys[k];
		dstr_printf(&expected_name, "%s.Replay Cam 2", legacy);
		CHECK(count_hotkey_name(&snap, expected_name.array) == 0,
		      "old hotkey name '%s' should not exist after rename", expected_name.array);
		dstr_printf(&expected_name, "%s.Replay Angle B", legacy);
		CHECK(count_hotkey_name(&snap, expected_name.array) == 1,
		      "renamed hotkey '%s' should exist once after rename", expected_name.array);
	}
	dstr_free(&expected_name);
	free_hotkey_snapshot(&snap);

	/* Rename back to Replay Cam 2 for subsequent tests */
	obs_source_set_name(rs2, "Replay Cam 2");

	/* =====================================================================
	 * 2. LoadAll alignment test with synthetic offset async frames
	 * ===================================================================== */
	obs_source_t *cam1 = obs_source_create("dummy_async_cam", "Cam 1", NULL, NULL);
	obs_source_t *cam2 = obs_source_create("dummy_async_cam", "Cam 2", NULL, NULL);
	CHECK(cam1 && cam2, "failed to create dummy async cameras");

	obs_data_t *s1 = obs_source_get_settings(rs1);
	obs_data_set_string(s1, SETTING_SOURCE, "Cam 1");
	obs_data_set_int(s1, SETTING_DURATION, 5000);
	obs_source_update(rs1, s1);
	obs_source_load(rs1);
	obs_data_release(s1);

	obs_data_t *s2 = obs_source_get_settings(rs2);
	obs_data_set_string(s2, SETTING_SOURCE, "Cam 2");
	obs_data_set_int(s2, SETTING_DURATION, 5000);
	obs_source_update(rs2, s2);
	obs_source_load(rs2);
	obs_data_release(s2);

	/* Feed 30 frames at 60 fps; Cam 2 timestamps are offset by 4 frames + 3ms relative to Cam 1 */
	const uint64_t interval_ns = 1000000000ULL / 60ULL;
	uint64_t base_ts = obs_get_video_frame_time();
	if (!base_ts)
		base_ts = os_gettime_ns();
	const uint64_t cam1_start = base_ts;
	const uint64_t cam2_start = base_ts + 4ULL * interval_ns + 3000000ULL;

	feed_nv12_frames(cam1, 320, 180, 30, cam1_start, interval_ns);
	feed_nv12_frames(cam2, 320, 180, 30, cam2_start, interval_ns);

	/* rs3 has no camera configured (empty buffer -> should be skipped by LoadAll) */
	obs_data_t *load_all_res = obs_data_create();
	CHECK(call_ws_request("LoadAll", NULL, load_all_res), "LoadAll vendor request not registered");
	CHECK(obs_data_get_bool(load_all_res, "ok"), "LoadAll response ok != true");
	CHECK(obs_data_get_int(load_all_res, "loadedCount") == 2, "expected loadedCount == 2, got %lld",
	      obs_data_get_int(load_all_res, "loadedCount"));
	CHECK(obs_data_get_int(load_all_res, "skippedCount") == 1, "expected skippedCount == 1, got %lld",
	      obs_data_get_int(load_all_res, "skippedCount"));

	obs_data_array_t *loaded_arr = obs_data_get_array(load_all_res, "replays");
	CHECK(obs_data_array_count(loaded_arr) == 2, "expected 2 loaded items in LoadAll response");
	obs_data_t *l0 = obs_data_array_item(loaded_arr, 0);
	obs_data_t *l1 = obs_data_array_item(loaded_arr, 1);
	CHECK(obs_data_get_int(l0, "startMs") == obs_data_get_int(l1, "startMs"),
	      "LoadAll startMs mismatch: %lld vs %lld", obs_data_get_int(l0, "startMs"),
	      obs_data_get_int(l1, "startMs"));
	CHECK(obs_data_get_int(l0, "durationMs") == obs_data_get_int(l1, "durationMs") &&
		      obs_data_get_int(l0, "durationMs") > 0,
	      "LoadAll durationMs mismatch: %lld vs %lld", obs_data_get_int(l0, "durationMs"),
	      obs_data_get_int(l1, "durationMs"));
	obs_data_release(l0);
	obs_data_release(l1);
	obs_data_array_release(loaded_arr);
	obs_data_release(load_all_res);

	/* Check per-replay effective windows and active frame timestamps */
	struct replay_source *ctx1 = obs_obj_get_data(rs1);
	struct replay_source *ctx2 = obs_obj_get_data(rs2);
	struct replay_source_status st1 = {0}, st2 = {0};
	CHECK(replay_source_get_status(ctx1, &st1) && replay_source_get_status(ctx2, &st2),
	      "replay_source_get_status failed");
	CHECK(st1.effective_start_ns == st2.effective_start_ns,
	      "effective_start_ns mismatch: %" PRIu64 " vs %" PRIu64, st1.effective_start_ns, st2.effective_start_ns);
	CHECK(st1.effective_end_ns == st2.effective_end_ns, "effective_end_ns mismatch: %" PRIu64 " vs %" PRIu64,
	      st1.effective_end_ns, st2.effective_end_ns);

	uint64_t f1_first = 0, f1_last = 0, f2_first = 0, f2_last = 0;
	CHECK(replay_source_get_active_window_frames(ctx1, &f1_first, &f1_last),
	      "get_active_window_frames failed on rs1");
	CHECK(replay_source_get_active_window_frames(ctx2, &f2_first, &f2_last),
	      "get_active_window_frames failed on rs2");
	uint64_t start_diff = (f1_first > f2_first) ? (f1_first - f2_first) : (f2_first - f1_first);
	uint64_t end_diff = (f1_last > f2_last) ? (f1_last - f2_last) : (f2_last - f1_last);
	CHECK(start_diff <= interval_ns, "aligned first frame diff %" PRIu64 " exceeds 1 frame interval %" PRIu64,
	      start_diff, interval_ns);
	CHECK(end_diff <= interval_ns, "aligned last frame diff %" PRIu64 " exceeds 1 frame interval %" PRIu64,
	      end_diff, interval_ns);

	/* Verify ReplayLoaded vendor events were emitted for both loaded sources */
	CHECK(g_fake_ws.events.num >= 2, "expected at least 2 ReplayLoaded vendor events, got %zu",
	      g_fake_ws.events.num);
	CHECK(strcmp(g_fake_ws.events.array[0].type, "ReplayLoaded") == 0, "expected event type ReplayLoaded");

	/* =====================================================================
	 * 3. Vendor API: GetReplays, Action (unknown + valid), SetSpeed
	 * ===================================================================== */
	CHECK(g_fake_ws.vendor_name && strcmp(g_fake_ws.vendor_name, "ReplaySource") == 0,
	      "expected vendor 'ReplaySource', got '%s'", g_fake_ws.vendor_name ? g_fake_ws.vendor_name : "(null)");

	/* GetReplays */
	obs_data_t *get_res = obs_data_create();
	CHECK(call_ws_request("GetReplays", NULL, get_res), "GetReplays request failed");
	CHECK(obs_data_get_bool(get_res, "ok"), "GetReplays ok != true");
	obs_data_array_t *replays_list = obs_data_get_array(get_res, "replays");
	CHECK(obs_data_array_count(replays_list) == 3, "expected 3 replays in GetReplays, got %zu",
	      obs_data_array_count(replays_list));
	obs_data_array_release(replays_list);
	obs_data_t *sq = obs_data_get_obj(get_res, "saveQueue");
	CHECK(sq != NULL, "GetReplays missing saveQueue object");
	obs_data_release(sq);
	obs_data_release(get_res);

	/* Action: unknown action -> ok:false, error */
	obs_data_t *act_bad_req = obs_data_create();
	obs_data_t *act_bad_res = obs_data_create();
	obs_data_set_string(act_bad_req, "action", "BogusNonExistentAction");
	CHECK(call_ws_request("Action", act_bad_req, act_bad_res), "Action request not registered");
	CHECK(!obs_data_get_bool(act_bad_res, "ok"), "expected ok == false for unknown action");
	CHECK(strlen(obs_data_get_string(act_bad_res, "error")) > 0, "expected error message for unknown action");
	obs_data_release(act_bad_req);
	obs_data_release(act_bad_res);

	/* Action: valid action ("HalfSpeed") on Replay Cam 1 + one unknown source */
	obs_data_t *act_ok_req = obs_data_create();
	obs_data_t *act_ok_res = obs_data_create();
	obs_data_set_string(act_ok_req, "action", "HalfSpeed");
	obs_data_array_t *target_sources = obs_data_array_create();
	obs_data_t *t_cam1 = obs_data_create();
	obs_data_set_string(t_cam1, "name", "Replay Cam 1");
	obs_data_array_push_back(target_sources, t_cam1);
	obs_data_release(t_cam1);
	obs_data_t *t_missing = obs_data_create();
	obs_data_set_string(t_missing, "name", "NoSuchReplaySource");
	obs_data_array_push_back(target_sources, t_missing);
	obs_data_release(t_missing);
	obs_data_set_array(act_ok_req, "sources", target_sources);
	obs_data_array_release(target_sources);

	CHECK(call_ws_request("Action", act_ok_req, act_ok_res), "Action request call failed");
	CHECK(obs_data_get_bool(act_ok_res, "ok"), "expected ok == true for HalfSpeed action");
	CHECK(obs_data_get_int(act_ok_res, "appliedCount") == 1, "expected appliedCount == 1");
	CHECK(obs_data_get_int(act_ok_res, "unknownCount") == 1, "expected unknownCount == 1");
	obs_data_release(act_ok_req);
	obs_data_release(act_ok_res);

	CHECK(replay_source_get_status(ctx1, &st1) && st1.speed_percent == 50.0f,
	      "expected Replay Cam 1 speed 50.0 after HalfSpeed action, got %.2f", st1.speed_percent);
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 100.0f,
	      "expected Replay Cam 2 speed unchanged at 100.0, got %.2f", st2.speed_percent);

	/* SetSpeed: set all replay sources to 25% */
	obs_data_t *spd_req = obs_data_create();
	obs_data_t *spd_res = obs_data_create();
	obs_data_set_double(spd_req, "speedPercent", 25.0);
	CHECK(call_ws_request("SetSpeed", spd_req, spd_res), "SetSpeed request call failed");
	CHECK(obs_data_get_bool(spd_res, "ok"), "expected SetSpeed ok == true");
	CHECK(obs_data_get_int(spd_res, "appliedCount") == 3, "expected SetSpeed applied to 3 sources");
	obs_data_release(spd_req);
	obs_data_release(spd_res);

	CHECK(replay_source_get_status(ctx1, &st1) && st1.speed_percent == 25.0f,
	      "expected Replay Cam 1 speed 25.0, got %.2f", st1.speed_percent);
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 25.0f,
	      "expected Replay Cam 2 speed 25.0, got %.2f", st2.speed_percent);

	/* A JSON string array ("sources": ["Replay Cam 1"]) reaches the plugin as an empty obs_data array because
	 * obs-websocket keeps only object items; that must be rejected instead of silently targeting nothing. */
	obs_data_t *bad_req = obs_data_create();
	obs_data_t *bad_res = obs_data_create();
	obs_data_array_t *dropped_strings = obs_data_array_create();
	obs_data_set_array(bad_req, "sources", dropped_strings);
	obs_data_array_release(dropped_strings);
	obs_data_set_double(bad_req, "speedPercent", 50.0);
	CHECK(call_ws_request("SetSpeed", bad_req, bad_res), "SetSpeed (empty sources) request call failed");
	CHECK(!obs_data_get_bool(bad_res, "ok"), "expected SetSpeed with empty sources array to fail");
	CHECK(replay_source_get_status(ctx1, &st1) && st1.speed_percent == 25.0f,
	      "speed must stay 25.0 after rejected request, got %.2f", st1.speed_percent);
	obs_data_release(bad_req);
	obs_data_release(bad_res);

	/* Comma-separated string targets exactly the listed sources */
	obs_data_t *csv_req = obs_data_create();
	obs_data_t *csv_res = obs_data_create();
	obs_data_set_string(csv_req, "sources", "Replay Cam 1, Replay Cam 2");
	obs_data_set_double(csv_req, "speedPercent", 40.0);
	CHECK(call_ws_request("SetSpeed", csv_req, csv_res), "SetSpeed (csv) request call failed");
	CHECK(obs_data_get_bool(csv_res, "ok") && obs_data_get_int(csv_res, "appliedCount") == 2,
	      "expected SetSpeed csv applied to 2 sources");
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 40.0f,
	      "expected Replay Cam 2 speed 40.0, got %.2f", st2.speed_percent);
	obs_data_release(csv_req);
	obs_data_release(csv_res);

	/* Cleanup */
	obs_source_release(rs1);
	obs_source_release(rs2);
	obs_source_release(rs3);
	obs_source_release(cam1);
	obs_source_release(cam2);

	obs_module_unload();

	for (size_t i = 0; i < g_fake_ws.requests.num; i++)
		bfree(g_fake_ws.requests.array[i].type);
	da_free(g_fake_ws.requests);
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		bfree(g_fake_ws.events.array[i].type);
		bfree(g_fake_ws.events.array[i].source);
	}
	da_free(g_fake_ws.events);
	bfree(g_fake_ws.vendor_name);
	proc_handler_destroy(g_fake_ws.ph);

	obs_shutdown();
	printf("[test-control] ALL CONTROL TESTS PASSED!\n");
	return 0;
}
