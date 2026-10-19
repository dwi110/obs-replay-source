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

static void count_update(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	os_atomic_inc_long(data);
}

/* Video sources apply obs_source_update on the next graphics tick; wait for the "update" signal. */
static bool update_and_wait(obs_source_t *source, obs_data_t *settings)
{
	volatile long updates = 0;
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "update", count_update, (void *)&updates);
	obs_source_update(source, settings);
	for (int i = 0; i < 400 && !os_atomic_load_long(&updates); i++)
		os_sleep_ms(5);
	signal_handler_disconnect(sh, "update", count_update, (void *)&updates);
	return os_atomic_load_long(&updates) > 0;
}

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
	char *trigger;
	char *path;
};

struct fake_ws_state {
	proc_handler_t *ph;
	int vendor_token;
	char *vendor_names[4];
	size_t vendor_count;
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
	if (name && ws->vendor_count < 4)
		ws->vendor_names[ws->vendor_count++] = bstrdup(name);
	calldata_set_ptr(cd, "vendor", &ws->vendor_token);
	calldata_set_bool(cd, "success", true);
}

static bool vendor_seen(const char *name)
{
	for (size_t i = 0; i < g_fake_ws.vendor_count; i++) {
		if (g_fake_ws.vendor_names[i] && strcmp(g_fake_ws.vendor_names[i], name) == 0)
			return true;
	}
	return false;
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
	const char *trig = obs_data_get_string(ev_data, "trigger");
	const char *pth = obs_data_get_string(ev_data, "path");
	struct fake_ws_event_entry ev = {
		.type = bstrdup(type),
		.source = bstrdup(src ? src : ""),
		.duration_ms = (int64_t)obs_data_get_int(ev_data, "durationMs"),
		.trigger = bstrdup(trig ? trig : ""),
		.path = bstrdup(pth ? pth : ""),
	};
	da_push_back(ws->events, &ev);
	calldata_set_bool(cd, "success", true);
}

static void clear_fake_events(void)
{
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		bfree(g_fake_ws.events.array[i].type);
		bfree(g_fake_ws.events.array[i].source);
		bfree(g_fake_ws.events.array[i].trigger);
		bfree(g_fake_ws.events.array[i].path);
	}
	da_resize(g_fake_ws.events, 0);
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

	/* Contract hotkeys moved to the MulticamReplay vendor; only approach-specific extras stay global here. */
	const char *const expected_global_hotkeys[] = {
		"ReplaySource.SlowmoBlendOff", "ReplaySource.SlowmoBlendProgram", "ReplaySource.SlowmoBlendAlways",
	};
	for (size_t i = 0; i < sizeof(expected_global_hotkeys) / sizeof(expected_global_hotkeys[0]); i++) {
		CHECK(count_hotkey_name(&snap, expected_global_hotkeys[i]) == 1,
		      "expected global hotkey '%s' exactly once", expected_global_hotkeys[i]);
	}
	const char *const removed_contract_hotkeys[] = {
		"ReplaySource.SaveAll", "ReplaySource.LoadAll", "ReplaySource.AllNormalSpeed", "ReplaySource.AllRestart",
		"ReplaySource.AllMarkIn",
	};
	for (size_t i = 0; i < sizeof(removed_contract_hotkeys) / sizeof(removed_contract_hotkeys[0]); i++) {
		CHECK(count_hotkey_name(&snap, removed_contract_hotkeys[i]) == 0,
		      "contract hotkey '%s' must no longer be registered under ReplaySource", removed_contract_hotkeys[i]);
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
	replay_control_load_all(NULL, 0, load_all_res);
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

	/* ReplayLoaded events belong to the MulticamReplay contract, not the per-source load path. */

	/* =====================================================================
	 * 3. Remaining ReplaySource vendor plus channel speed
	 * ===================================================================== */
	CHECK(vendor_seen("ReplaySource"), "ReplaySource vendor not registered");
	CHECK(vendor_seen("MulticamReplay"), "MulticamReplay vendor not registered");

	/* GetReplays stays on the old vendor (approach extra, not a contract request). */
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

	/* Contract speed is channel-wide. Per-source speed remains a raw extra. */
	CHECK(replay_source_execute_action(ctx1, "HalfSpeed", false), "HalfSpeed action failed");
	CHECK(replay_source_get_status(ctx1, &st1) && st1.speed_percent == 50.0f,
	      "expected Replay Cam 1 speed 50.0 after HalfSpeed, got %.2f", st1.speed_percent);
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 100.0f,
	      "expected Replay Cam 2 speed unchanged at 100.0, got %.2f", st2.speed_percent);

	replay_control_set_all_speed(25.0f);
	CHECK(replay_source_get_status(ctx1, &st1) && st1.speed_percent == 25.0f,
	      "expected Replay Cam 1 speed 25.0, got %.2f", st1.speed_percent);
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 25.0f,
	      "expected Replay Cam 2 speed 25.0, got %.2f", st2.speed_percent);

	replay_source_set_speed(ctx2, 40.0f);
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 40.0f,
	      "expected Replay Cam 2 speed 40.0, got %.2f", st2.speed_percent);

	/* A settings change that leaves the stored speed alone (e.g. toggling preview_half) keeps the runtime speed;
	 * changing the stored speed applies it. */
	obs_data_t *keep = obs_source_get_settings(rs2);
	obs_data_set_bool(keep, SETTING_PREVIEW_HALF, !obs_data_get_bool(keep, SETTING_PREVIEW_HALF));
	CHECK(update_and_wait(rs2, keep), "settings update not applied");
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 40.0f,
	      "unrelated settings change reset runtime speed 40.0 to %.2f", st2.speed_percent);
	obs_data_set_double(keep, SETTING_SPEED, 75.0);
	CHECK(update_and_wait(rs2, keep), "speed update not applied");
	CHECK(replay_source_get_status(ctx2, &st2) && st2.speed_percent == 75.0f,
	      "changed stored speed not applied: %.2f", st2.speed_percent);
	obs_data_set_double(keep, SETTING_SPEED, 100.0);
	CHECK(update_and_wait(rs2, keep), "speed reset update not applied");
	obs_data_release(keep);
	/* Reset speed to 100% after Section 3 SetSpeed tests */
	replay_control_set_all_speed(100.0f);

	/* =====================================================================
	 * 4. Multi-Angle Mark In/Out & Alignment-Aware Trims
	 * ===================================================================== */
	CHECK(replay_source_get_status(ctx1, &st1) && replay_source_get_status(ctx2, &st2),
	      "replay_source_get_status failed");
	CHECK(st1.mark_in_ms == 0 && st2.mark_in_ms == 0,
	      "initial markInMs after LoadAll must be 0, got %" PRId64 " / %" PRId64, st1.mark_in_ms, st2.mark_in_ms);
	CHECK(st1.mark_out_ms == st1.duration_ms && st2.mark_out_ms == st2.duration_ms,
	      "initial markOutMs after LoadAll must match durationMs");

	/* Drive Cam 1 playback to a deterministic timestamp inside the common aligned window */
	const uint64_t mark_in_ts = cam2_start + 6ULL * interval_ns;
	replay_source_test_set_playback_pos(ctx1, mark_in_ts, cam1_start);

	/* Trigger AllMarkIn: reference is Cam 1 */
	replay_control_all_mark_in();
	CHECK(replay_source_get_status(ctx1, &st1) && replay_source_get_status(ctx2, &st2),
	      "replay_source_get_status failed after AllMarkIn");
	CHECK(st1.effective_start_ns == st2.effective_start_ns,
	      "AllMarkIn effective_start_ns mismatch: %" PRIu64 " vs %" PRIu64, st1.effective_start_ns,
	      st2.effective_start_ns);
	CHECK(st1.mark_in_ms == st2.mark_in_ms, "AllMarkIn markInMs mismatch: %" PRId64 " vs %" PRId64, st1.mark_in_ms,
	      st2.mark_in_ms);
	CHECK(st1.mark_in_ms > 0, "AllMarkIn markInMs must be > 0");

	/* Trigger AllMarkOut at another deterministic position */
	const uint64_t mark_out_ts = cam2_start + 22ULL * interval_ns;
	replay_source_test_set_playback_pos(ctx1, mark_out_ts, cam1_start);
	replay_control_all_mark_out();
	CHECK(replay_source_get_status(ctx1, &st1) && replay_source_get_status(ctx2, &st2),
	      "replay_source_get_status failed after AllMarkOut");
	CHECK(st1.effective_end_ns == st2.effective_end_ns,
	      "AllMarkOut effective_end_ns mismatch: %" PRIu64 " vs %" PRIu64, st1.effective_end_ns,
	      st2.effective_end_ns);
	CHECK(st1.mark_out_ms == st2.mark_out_ms, "AllMarkOut markOutMs mismatch: %" PRId64 " vs %" PRId64,
	      st1.mark_out_ms, st2.mark_out_ms);

	/* MarkReset restores the aligned window, not the full buffer. */
	replay_control_all_mark_reset();

	CHECK(replay_source_get_status(ctx1, &st1) && replay_source_get_status(ctx2, &st2),
	      "replay_source_get_status failed after MarkReset");
	CHECK(st1.effective_start_ns == st2.effective_start_ns, "MarkReset must restore aligned base start");
	CHECK(st1.effective_start_ns > cam1_start, "MarkReset must restore aligned base, not reset to full buffer 0");
	CHECK(st1.mark_in_ms == 0 && st2.mark_in_ms == 0, "markInMs after MarkReset must be 0");

	/* Mark clamping: In outside (before) the aligned window is clamped to aligned base */
	replay_source_test_set_playback_pos(ctx1, cam1_start, cam1_start);
	replay_control_all_mark_in();
	CHECK(replay_source_get_status(ctx1, &st1) && replay_source_get_status(ctx2, &st2),
	      "replay_source_get_status failed after clamped In");
	CHECK(st1.effective_start_ns == st2.effective_start_ns, "effective_start_ns after clamped In must match");
	CHECK(st1.mark_in_ms == 0 && st2.mark_in_ms == 0, "clamped In must clamp to aligned start (markInMs == 0)");

	/* Channel MarkIn uses the reference source playback position. */
	replay_source_test_set_playback_pos(ctx1, mark_in_ts, cam1_start);
	replay_control_all_mark_in();
	CHECK(replay_source_get_status(ctx1, &st1) && st1.mark_in_ms > 0, "MarkIn markInMs must be > 0, got %" PRId64,
	      st1.mark_in_ms);

	/* =====================================================================
	 * 5. Auto-Save Tests (ON_LOAD, ON_RELEASE, OFF)
	 * ===================================================================== */
	/* 5a: AUTO_SAVE_ON_LOAD */
	obs_data_t *s1_as = obs_source_get_settings(rs1);
	obs_data_set_int(s1_as, SETTING_AUTO_SAVE, AUTO_SAVE_ON_LOAD);
	obs_source_update(rs1, s1_as);
	obs_source_load(rs1);
	obs_data_release(s1_as);

	obs_data_t *s2_as = obs_source_get_settings(rs2);
	obs_data_set_int(s2_as, SETTING_AUTO_SAVE, AUTO_SAVE_ON_LOAD);
	obs_source_update(rs2, s2_as);
	obs_source_load(rs2);
	obs_data_release(s2_as);

	clear_fake_events();
	const uint64_t as_base_ts = mark_out_ts + 10ULL * interval_ns;
	feed_nv12_frames(cam1, 320, 180, 30, as_base_ts, interval_ns);
	feed_nv12_frames(cam2, 320, 180, 30, as_base_ts + 4ULL * interval_ns, interval_ns);

	obs_data_t *load_as_res = obs_data_create();
	replay_control_load_all(NULL, 0, load_as_res);
	CHECK(obs_data_get_bool(load_as_res, "ok"), "LoadAll ok != true");
	obs_data_release(load_as_res);

	CHECK(replay_save_queue_wait_idle(5000), "replay_save_queue_wait_idle failed for auto-load");

	size_t auto_load_events = 0;
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		if (strcmp(g_fake_ws.events.array[i].type, "SaveStarted") == 0 &&
		    strcmp(g_fake_ws.events.array[i].trigger, "auto-load") == 0) {
			auto_load_events++;
		}
	}
	CHECK(auto_load_events >= 2, "expected at least 2 SaveStarted events with trigger 'auto-load', got %zu",
	      auto_load_events);

	/* 5b: AUTO_SAVE_ON_RELEASE */
	obs_data_t *s1_rel = obs_source_get_settings(rs1);
	obs_data_set_int(s1_rel, SETTING_AUTO_SAVE, AUTO_SAVE_ON_RELEASE);
	obs_source_update(rs1, s1_rel);
	obs_source_load(rs1);
	obs_data_release(s1_rel);

	clear_fake_events();
	const uint64_t rel_base_ts = as_base_ts + 50ULL * interval_ns;
	feed_nv12_frames(cam1, 320, 180, 30, rel_base_ts, interval_ns);

	CHECK(replay_source_execute_action(ctx1, "Load", false), "Load action failed");

	replay_source_test_set_playback_pos(ctx1, rel_base_ts + 5ULL * interval_ns, rel_base_ts);
	obs_data_t *mark_act_req = obs_data_create();
	CHECK(replay_source_execute_action(ctx1, "MarkIn", false), "MarkIn action failed");
	obs_data_release(mark_act_req);

	clear_fake_events();
	obs_data_t *clr_act_req = obs_data_create();
	CHECK(replay_source_execute_action(ctx1, "Clear", false), "Clear action failed");
	obs_data_release(clr_act_req);

	CHECK(replay_save_queue_wait_idle(5000), "replay_save_queue_wait_idle failed for auto-release");
	size_t auto_rel_events = 0;
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		if (strcmp(g_fake_ws.events.array[i].type, "SaveStarted") == 0 &&
		    strcmp(g_fake_ws.events.array[i].trigger, "auto-release") == 0) {
			auto_rel_events++;
		}
	}
	CHECK(auto_rel_events == 1, "expected 1 SaveStarted event with trigger 'auto-release', got %zu",
	      auto_rel_events);

	/* Second Clear of the same replay must not enqueue again */
	clear_fake_events();
	obs_data_t *clr2_act_req = obs_data_create();
	CHECK(replay_source_execute_action(ctx1, "Clear", false), "second Clear action failed");
	obs_data_release(clr2_act_req);
	CHECK(replay_save_queue_pending_count() == 0, "second Clear must not enqueue job");
	CHECK(g_fake_ws.events.num == 0, "second Clear must emit no SaveStarted events");

	/* 5c: AUTO_SAVE_OFF -> Clear does not enqueue */
	obs_data_t *s1_off = obs_source_get_settings(rs1);
	obs_data_set_int(s1_off, SETTING_AUTO_SAVE, AUTO_SAVE_OFF);
	obs_source_update(rs1, s1_off);
	obs_source_load(rs1);
	obs_data_release(s1_off);

	feed_nv12_frames(cam1, 320, 180, 30, rel_base_ts + 100ULL * interval_ns, interval_ns);
	obs_data_t *load3_req = obs_data_create();
	CHECK(replay_source_execute_action(ctx1, "Load", false), "Load action failed for AUTO_SAVE_OFF");
	obs_data_release(load3_req);

	clear_fake_events();
	obs_data_t *clr3_req = obs_data_create();
	CHECK(replay_source_execute_action(ctx1, "Clear", false), "Clear action failed for AUTO_SAVE_OFF");
	obs_data_release(clr3_req);
	CHECK(replay_save_queue_pending_count() == 0, "AUTO_SAVE_OFF Clear must not enqueue job");
	CHECK(g_fake_ws.events.num == 0, "AUTO_SAVE_OFF Clear must emit no SaveStarted events");

	/* =====================================================================
	 * 6. LoadAll staleness uses the receive clock, not the cameras' timestamps:
	 *    - Cam 5 stopped delivering 1.3 s before the trigger (its timestamps still end with the others),
	 *    - Cam 6 just reconnected (67 ms history),
	 *    - Cam 7 is healthy but its clock runs 1.5 s ahead of Cam 4 (real cameras differ by up to
	 *      MAX_TS_VAR before the capture path rebases); it must not make Cam 4 look stalled.
	 *    Cam 5 and Cam 6 must be skipped and unloaded, Cam 4 and Cam 7 loaded.
	 * ===================================================================== */
	obs_source_t *stall_cams[4];
	obs_source_t *stall_rs[4];
	const char *stall_cam[4] = {"Cam 4", "Cam 5", "Cam 6", "Cam 7"};
	const char *stall_rs_name[4] = {"Replay Cam 4", "Replay Cam 5", "Replay Cam 6", "Replay Cam 7"};
	for (int i = 0; i < 4; i++) {
		stall_cams[i] = obs_source_create("dummy_async_cam", stall_cam[i], NULL, NULL);
		stall_rs[i] = obs_source_create(REPLAY_SOURCE_ID, stall_rs_name[i], NULL, NULL);
		CHECK(stall_cams[i] && stall_rs[i], "failed to create stall-test sources");
		obs_data_t *st = obs_source_get_settings(stall_rs[i]);
		obs_data_set_string(st, SETTING_SOURCE, stall_cam[i]);
		obs_data_set_int(st, SETTING_DURATION, 5000);
		obs_source_update(stall_rs[i], st);
		obs_source_load(stall_rs[i]);
		obs_data_release(st);
	}

	/* Timestamps stay within +-2 s of the capture clock so the capture path keeps them unchanged. */
	uint64_t stall_now = obs_get_video_frame_time();
	if (!stall_now)
		stall_now = os_gettime_ns();
	const uint64_t trigger_ts = stall_now + 1300000000ULL;
	feed_nv12_frames(stall_cams[1], 320, 180, 180, trigger_ts - 180ULL * interval_ns, interval_ns);
	os_sleep_ms(1300);
	feed_nv12_frames(stall_cams[0], 320, 180, 180, trigger_ts - 180ULL * interval_ns, interval_ns);
	feed_nv12_frames(stall_cams[2], 320, 180, 5, trigger_ts - 5ULL * interval_ns, interval_ns);
	feed_nv12_frames(stall_cams[3], 320, 180, 180, trigger_ts + 1500000000ULL - 180ULL * interval_ns, interval_ns);

	clear_fake_events();
	const char *stall_filter[] = {"Replay Cam 4", "Replay Cam 5", "Replay Cam 6", "Replay Cam 7"};
	obs_data_t *stall_res = obs_data_create();
	replay_control_load_all(stall_filter, 4, stall_res);
	CHECK(obs_data_get_int(stall_res, "loadedCount") == 2 && obs_data_get_int(stall_res, "skippedCount") == 2,
	      "expected 2 loaded / 2 skipped, got %lld / %lld", obs_data_get_int(stall_res, "loadedCount"),
	      obs_data_get_int(stall_res, "skippedCount"));

	obs_data_array_t *stall_loaded = obs_data_get_array(stall_res, "replays");
	for (size_t i = 0; i < obs_data_array_count(stall_loaded); i++) {
		obs_data_t *item = obs_data_array_item(stall_loaded, i);
		const char *nm = obs_data_get_string(item, "name");
		CHECK(strcmp(nm, "Replay Cam 4") == 0 || strcmp(nm, "Replay Cam 7") == 0,
		      "'%s' loaded, expected only the healthy Cam 4 and the clock-offset Cam 7", nm);
		obs_data_release(item);
	}
	obs_data_array_release(stall_loaded);

	obs_data_array_t *stall_skipped = obs_data_get_array(stall_res, "skipped");
	CHECK(obs_data_array_count(stall_skipped) == 2, "expected 2 skipped entries");
	for (size_t i = 0; i < 2; i++) {
		obs_data_t *sk = obs_data_array_item(stall_skipped, i);
		const char *nm = obs_data_get_string(sk, "name");
		const char *reason = obs_data_get_string(sk, "reason");
		const char *want = strcmp(nm, "Replay Cam 5") == 0 ? "stalled" : "short";
		CHECK(strcmp(nm, "Replay Cam 5") == 0 || strcmp(nm, "Replay Cam 6") == 0, "unexpected skipped '%s'", nm);
		CHECK(strcmp(reason, want) == 0, "'%s' skipped as '%s', expected '%s'", nm, reason, want);
		obs_data_release(sk);
	}
	obs_data_array_release(stall_skipped);
	obs_data_release(stall_res);

	for (int i = 1; i < 3; i++) {
		struct replay_source_status sst = {0};
		CHECK(replay_source_get_status(obs_obj_get_data(stall_rs[i]), &sst), "status failed");
		CHECK(sst.loaded_replays == 0, "skipped angle '%s' still has %d replay(s) loaded", stall_cam[i],
		      sst.loaded_replays);
	}
	size_t stall_loaded_events = 0;
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		if (strcmp(g_fake_ws.events.array[i].type, "ReplayLoaded") == 0)
			stall_loaded_events++;
	}
	CHECK(stall_loaded_events == 2, "expected ReplayLoaded only for the two healthy angles, got %zu",
	      stall_loaded_events);

	for (int i = 0; i < 4; i++) {
		obs_source_release(stall_rs[i]);
		obs_source_release(stall_cams[i]);
	}

	/* =====================================================================
	 * 7. MulticamReplay contract: status, trigger, transport, marks, hotkeys
	 * ===================================================================== */
	CHECK(!find_ws_request("LoadAll") && !find_ws_request("Action") && !find_ws_request("SaveAll"),
	      "contract requests must not stay registered on the old vendor");

	struct hotkey_snapshot contract_keys;
	memset(&contract_keys, 0, sizeof contract_keys);
	obs_enum_hotkeys(collect_hotkeys_cb, &contract_keys);
	const char *const contract_hotkeys[] = {
		"MulticamReplay.Trigger", "MulticamReplay.Release", "MulticamReplay.TogglePlay",
		"MulticamReplay.Restart", "MulticamReplay.Speed100", "MulticamReplay.Speed75",
		"MulticamReplay.Speed50", "MulticamReplay.Speed25", "MulticamReplay.ToggleDirection",
		"MulticamReplay.StepForward", "MulticamReplay.StepBackward", "MulticamReplay.ToggleLoop",
		"MulticamReplay.MarkIn", "MulticamReplay.MarkOut", "MulticamReplay.MarkReset",
		"MulticamReplay.SaveHighlight", "MulticamReplay.SaveScoringPlay", "MulticamReplay.SaveFoul",
		"MulticamReplay.SaveOther", "MulticamReplay.SaveAllAngles", "MulticamReplay.PlaylistAll",
		"MulticamReplay.PlaylistTogglePlay", "MulticamReplay.PlaylistNext",
		"MulticamReplay.PlaylistPrevious", "MulticamReplay.PlaylistStop",
	};
	for (size_t i = 0; i < sizeof contract_hotkeys / sizeof contract_hotkeys[0]; i++) {
		CHECK(count_hotkey_name(&contract_keys, contract_hotkeys[i]) == 1,
		      "expected contract hotkey '%s' exactly once", contract_hotkeys[i]);
	}
	free_hotkey_snapshot(&contract_keys);

	obs_data_t *idle_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, idle_res), "GetStatus not registered");
	CHECK(obs_data_get_bool(idle_res, "ok") && obs_data_get_int(idle_res, "contract") == 1,
	      "GetStatus contract field");
	CHECK(strcmp(obs_data_get_string(idle_res, "implementation"), "raw-nv12") == 0, "implementation must be raw-nv12");
	CHECK(strcmp(obs_data_get_string(idle_res, "state"), "idle") == 0, "state must be idle before a clip, got '%s'",
	      obs_data_get_string(idle_res, "state"));
	CHECK(!obs_data_get_bool(idle_res, "ready"), "ready must be false without a 12 s window");
	obs_data_release(idle_res);

	obs_data_t *noclip_req = obs_data_create();
	obs_data_t *noclip_res = obs_data_create();
	CHECK(call_ws_request("Play", noclip_req, noclip_res), "Play not registered");
	CHECK(!obs_data_get_bool(noclip_res, "ok") && strcmp(obs_data_get_string(noclip_res, "code"), "NoClipLoaded") == 0,
	      "Play without a clip must return NoClipLoaded");
	obs_data_release(noclip_req);
	obs_data_release(noclip_res);
	obs_data_t *bad_speed_req = obs_data_create();
	obs_data_t *bad_speed_res = obs_data_create();
	obs_data_set_double(bad_speed_req, "speedPercent", 1000.0);
	CHECK(call_ws_request("SetSpeed", bad_speed_req, bad_speed_res), "SetSpeed not registered");
	CHECK(!obs_data_get_bool(bad_speed_res, "ok") &&
		      strcmp(obs_data_get_string(bad_speed_res, "code"), "InvalidParameter") == 0,
	      "invalid speed without a clip must be InvalidParameter, got '%s'",
	      obs_data_get_string(bad_speed_res, "code"));
	obs_data_release(bad_speed_req);
	obs_data_release(bad_speed_res);
	obs_data_t *bad_step_req = obs_data_create();
	obs_data_t *bad_step_res = obs_data_create();
	obs_data_set_int(bad_step_req, "frames", 0);
	CHECK(call_ws_request("Step", bad_step_req, bad_step_res) && !obs_data_get_bool(bad_step_res, "ok") &&
		      strcmp(obs_data_get_string(bad_step_res, "code"), "InvalidParameter") == 0,
	      "step 0 without a clip must be InvalidParameter");
	obs_data_release(bad_step_req);
	obs_data_release(bad_step_res);
	obs_data_t *bad_dir_req = obs_data_create();
	obs_data_t *bad_dir_res = obs_data_create();
	obs_data_set_string(bad_dir_req, "direction", "up");
	CHECK(call_ws_request("SetDirection", bad_dir_req, bad_dir_res) && !obs_data_get_bool(bad_dir_res, "ok") &&
		      strcmp(obs_data_get_string(bad_dir_res, "code"), "InvalidParameter") == 0,
	      "invalid direction without a clip must be InvalidParameter");
	obs_data_release(bad_dir_req);
	obs_data_release(bad_dir_res);


	clear_fake_events();
	obs_data_t *bad_look_req = obs_data_create();
	obs_data_t *bad_look_res = obs_data_create();
	obs_data_set_int(bad_look_req, "lookbackMs", 500);
	CHECK(call_ws_request("Trigger", bad_look_req, bad_look_res), "Trigger not registered");
	CHECK(!obs_data_get_bool(bad_look_res, "ok") &&
		      strcmp(obs_data_get_string(bad_look_res, "code"), "InvalidParameter") == 0,
	      "lookbackMs 500 must be InvalidParameter");
	size_t trigger_failed = 0;
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		if (strcmp(g_fake_ws.events.array[i].type, "TriggerFailed") == 0)
			trigger_failed++;
	}
	CHECK(trigger_failed == 1, "invalid Trigger must emit TriggerFailed, got %zu", trigger_failed);
	obs_data_release(bad_look_req);
	obs_data_release(bad_look_res);

	obs_source_t *cam_a = obs_source_create("dummy_async_cam", "Cam A", NULL, NULL);
	obs_source_t *rs_a = obs_source_create(REPLAY_SOURCE_ID, "Raw A", NULL, NULL);
	CHECK(cam_a && rs_a, "failed to create contract camera");
	obs_data_t *sa = obs_source_get_settings(rs_a);
	obs_data_set_string(sa, SETTING_SOURCE, "Cam A");
	obs_data_set_int(sa, SETTING_DURATION, 5000);
	obs_source_update(rs_a, sa);
	obs_source_load(rs_a);
	obs_data_release(sa);
	obs_scene_t *scene_a = obs_scene_create("Replay Cam A");
	CHECK(scene_a && obs_scene_add(scene_a, rs_a), "failed to create replay scene");

	const uint64_t contract_interval = 1000000000ULL / 60ULL;
	uint64_t contract_now = os_gettime_ns();
	feed_nv12_frames(cam_a, 320, 180, 90, contract_now - 90ULL * contract_interval, contract_interval);

	clear_fake_events();
	obs_data_t *trig_req = obs_data_create();
	obs_data_t *trig_res = obs_data_create();
	obs_data_set_int(trig_req, "lookbackMs", 1000);
	obs_data_set_string(trig_req, "label", "Ereignis");
	CHECK(call_ws_request("Trigger", trig_req, trig_res), "Trigger call failed");
	CHECK(obs_data_get_bool(trig_res, "ok"), "Trigger failed: %s", obs_data_get_string(trig_res, "message"));
	const char *clip_id = obs_data_get_string(trig_res, "clipId");
	CHECK(clip_id && clip_id[0], "Trigger must return a clipId");
	obs_data_release(trig_req);
	obs_data_release(trig_res);

	size_t clip_loaded = 0;
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		if (strcmp(g_fake_ws.events.array[i].type, "ClipLoaded") == 0)
			clip_loaded++;
	}
	CHECK(clip_loaded == 1, "Trigger must emit one ClipLoaded, got %zu", clip_loaded);

	obs_data_t *st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after trigger failed");
	obs_data_t *clip = obs_data_get_obj(st_res, "clip");
	CHECK(clip, "GetStatus clip missing after trigger");
	CHECK(strcmp(obs_data_get_string(st_res, "state"), "playing") == 0, "trigger must start playback, got '%s'",
	      obs_data_get_string(st_res, "state"));
	CHECK(obs_data_get_int(clip, "durationMs") == 1000, "clip durationMs %lld, expected 1000",
	      obs_data_get_int(clip, "durationMs"));
	CHECK(obs_data_get_bool(clip, "loop"), "trigger must enable loop");
	CHECK(strcmp(obs_data_get_string(clip, "direction"), "forward") == 0, "trigger must restore forward");
	CHECK(strcmp(obs_data_get_string(clip, "origin"), "trigger") == 0, "origin must be trigger");
	CHECK(strcmp(obs_data_get_string(clip, "label"), "Ereignis") == 0, "label not kept");
	CHECK(obs_data_get_int(clip, "speedPercent") == 100, "default speed must stay 100");
	obs_data_release(clip);
	obs_data_release(st_res);

	obs_data_t *spd_req = obs_data_create();
	obs_data_t *spd_res = obs_data_create();
	obs_data_set_double(spd_req, "speedPercent", 50.0);
	CHECK(call_ws_request("SetSpeed", spd_req, spd_res) && obs_data_get_bool(spd_res, "ok"), "SetSpeed 50 failed");
	obs_data_release(spd_req);
	obs_data_release(spd_res);
	obs_data_set_double(spd_req = obs_data_create(), "speedPercent", 1.0);
	spd_res = obs_data_create();
	CHECK(call_ws_request("SetSpeed", spd_req, spd_res), "SetSpeed call failed");
	CHECK(!obs_data_get_bool(spd_res, "ok") && strcmp(obs_data_get_string(spd_res, "code"), "InvalidParameter") == 0,
	      "speed 1 must be rejected");
	obs_data_release(spd_req);
	obs_data_release(spd_res);

	st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after rejected speed failed");
	clip = obs_data_get_obj(st_res, "clip");
	CHECK(clip && obs_data_get_int(clip, "speedPercent") == 50, "rejected SetSpeed changed speed to %lld",
	      clip ? obs_data_get_int(clip, "speedPercent") : -1);
	obs_data_release(clip);
	obs_data_release(st_res);

	/* Restart while no replay source renders (live scene in program): the transport position must already be
	 * Mark In, not the old position until the next render. */
	obs_data_t *seek_req = obs_data_create();
	obs_data_t *seek_res = obs_data_create();
	obs_data_set_int(seek_req, "positionMs", 600);
	CHECK(call_ws_request("Seek", seek_req, seek_res) && obs_data_get_bool(seek_res, "ok"), "Seek 600 failed");
	obs_data_release(seek_req);
	obs_data_release(seek_res);
	obs_data_t *rst_res = obs_data_create();
	CHECK(call_ws_request("Restart", NULL, rst_res) && obs_data_get_bool(rst_res, "ok"), "Restart failed");
	obs_data_release(rst_res);
	st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after Restart failed");
	clip = obs_data_get_obj(st_res, "clip");
	CHECK(clip && obs_data_get_int(clip, "playheadMs") <= obs_data_get_int(clip, "markInMs") + 50,
	      "Restart must report Mark In while hidden, playhead %lld markIn %lld",
	      clip ? obs_data_get_int(clip, "playheadMs") : -1, clip ? obs_data_get_int(clip, "markInMs") : -1);
	obs_data_release(clip);
	obs_data_release(st_res);

	obs_data_t *mark_req = obs_data_create();
	obs_data_t *mark_res = obs_data_create();
	obs_data_set_int(mark_req, "positionMs", 1000);
	CHECK(call_ws_request("MarkIn", mark_req, mark_res), "MarkIn call failed");
	CHECK(!obs_data_get_bool(mark_res, "ok") && strcmp(obs_data_get_string(mark_res, "code"), "InvalidParameter") == 0,
	      "MarkIn at mark out must be rejected");
	obs_data_release(mark_req);
	obs_data_release(mark_res);

	obs_data_t *angle_req = obs_data_create();
	obs_data_t *angle_res = obs_data_create();
	obs_data_set_string(angle_req, "angle", "Cam A");
	CHECK(call_ws_request("SetSaveAngle", angle_req, angle_res) && obs_data_get_bool(angle_res, "ok"),
	      "SetSaveAngle failed: %s", obs_data_get_string(angle_res, "message"));
	obs_data_release(angle_req);
	obs_data_release(angle_res);

	obs_data_t *bad_tag_req = obs_data_create();
	obs_data_t *bad_tag_res = obs_data_create();
	obs_data_set_string(bad_tag_req, "tags", "Not A Tag");
	CHECK(call_ws_request("Save", bad_tag_req, bad_tag_res), "Save not registered");
	CHECK(!obs_data_get_bool(bad_tag_res, "ok") &&
		      strcmp(obs_data_get_string(bad_tag_res, "code"), "InvalidParameter") == 0,
	      "invalid tags must be rejected");
	obs_data_release(bad_tag_req);
	obs_data_release(bad_tag_res);

	obs_data_t *reset_game_req = obs_data_create();
	obs_data_t *reset_game_res = obs_data_create();
	obs_data_set_string(reset_game_req, "gameId", "game_default");
	CHECK(call_ws_request("SetActiveGame", reset_game_req, reset_game_res) && obs_data_get_bool(reset_game_res, "ok"),
	      "could not select game_default");
	obs_data_release(reset_game_req);
	obs_data_release(reset_game_res);
	obs_data_t *save_game_req = obs_data_create();
	obs_data_t *save_game_res = obs_data_create();
	obs_data_set_string(save_game_req, "name", "Vertragslauf");
	CHECK(call_ws_request("CreateGame", save_game_req, save_game_res) && obs_data_get_bool(save_game_res, "ok"),
	      "CreateGame before save failed");
	char save_game[64];
	snprintf(save_game, sizeof save_game, "%s", obs_data_get_string(save_game_res, "gameId"));
	CHECK(strncmp(save_game, "game-", 5) == 0, "save game id missing");
	obs_data_release(save_game_req);
	obs_data_release(save_game_res);
	st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after early CreateGame failed");
	CHECK(strcmp(obs_data_get_string(st_res, "activeGameId"), "game_default") == 0,
	      "CreateGame activated '%s'", obs_data_get_string(st_res, "activeGameId"));
	obs_data_release(st_res);


	obs_data_t *save_req = obs_data_create();
	obs_data_t *save_res = obs_data_create();
	obs_data_set_string(save_req, "gameId", save_game);
	obs_data_set_string(save_req, "tags", "highlight");
	CHECK(call_ws_request("Save", save_req, save_res), "Save call failed");
	CHECK(obs_data_get_bool(save_res, "ok"), "Save failed: %s", obs_data_get_string(save_res, "message"));
	const char *library_id = obs_data_get_string(save_res, "libraryId");
	CHECK(library_id && library_id[0] && obs_data_get_string(save_res, "jobId")[0],
	      "Save must return jobId and libraryId");
	char saved_library_id[64];
	snprintf(saved_library_id, sizeof saved_library_id, "%s", library_id);
	obs_data_release(save_req);
	obs_data_release(save_res);

	obs_data_t *lib_res = obs_data_create();
	CHECK(call_ws_request("GetLibrary", NULL, lib_res) && obs_data_get_bool(lib_res, "ok"), "GetLibrary failed");
	obs_data_array_t *entries = obs_data_get_array(lib_res, "entries");
	bool found_entry = false;
	for (size_t i = 0; entries && i < obs_data_array_count(entries); i++) {
		obs_data_t *entry = obs_data_array_item(entries, i);
		if (strcmp(obs_data_get_string(entry, "libraryId"), saved_library_id) == 0 &&
		    strcmp(obs_data_get_string(entry, "angle"), "Cam A") == 0 &&
		    strcmp(obs_data_get_string(entry, "tags"), "highlight") == 0)
			found_entry = true;
		obs_data_release(entry);
	}
	CHECK(found_entry, "saved library entry missing");
	obs_data_array_release(entries);
	obs_data_release(lib_res);

	obs_data_t *game_req = obs_data_create();
	obs_data_t *game_res = obs_data_create();
	obs_data_set_string(game_req, "gameId", "missing-game");
	CHECK(call_ws_request("SetActiveGame", game_req, game_res), "SetActiveGame not registered");
	CHECK(!obs_data_get_bool(game_res, "ok") && strcmp(obs_data_get_string(game_res, "code"), "GameNotFound") == 0,
	      "unknown game must be GameNotFound");
	obs_data_release(game_req);
	obs_data_release(game_res);


	CHECK(replay_save_queue_wait_idle(20000), "contract save did not finish");
	lib_res = obs_data_create();
	CHECK(call_ws_request("GetLibrary", NULL, lib_res) && obs_data_get_bool(lib_res, "ok"), "GetLibrary after save failed");
	entries = obs_data_get_array(lib_res, "entries");
	bool save_ready = false;
	for (size_t i = 0; entries && i < obs_data_array_count(entries); i++) {
		obs_data_t *entry = obs_data_array_item(entries, i);
		if (strcmp(obs_data_get_string(entry, "libraryId"), saved_library_id) == 0 &&
		    strcmp(obs_data_get_string(entry, "state"), "ready") == 0 &&
		    obs_data_get_string(entry, "file")[0])
			save_ready = true;
		obs_data_release(entry);
	}
	CHECK(save_ready, "saved entry did not become ready");
	obs_data_array_release(entries);
	obs_data_release(lib_res);

	obs_data_t *merge_req = obs_data_create();
	obs_data_t *merge_res = obs_data_create();
	obs_data_set_string(merge_req, "gameId", save_game);
	obs_data_set_string(merge_req, "tags", "foul");
	CHECK(call_ws_request("Save", merge_req, merge_res) && obs_data_get_bool(merge_res, "ok"), "tag merge save failed");
	CHECK(strcmp(obs_data_get_string(merge_res, "libraryId"), saved_library_id) == 0,
	      "same angle and range must keep libraryId '%s', got '%s'", saved_library_id,
	      obs_data_get_string(merge_res, "libraryId"));
	obs_data_release(merge_req);
	obs_data_release(merge_res);

	obs_data_t *tags_req = obs_data_create();
	obs_data_t *tags_res = obs_data_create();
	obs_data_set_string(tags_req, "libraryId", saved_library_id);
	obs_data_set_string(tags_req, "tags", "highlight,scoring_play");
	CHECK(call_ws_request("SetTags", tags_req, tags_res) && obs_data_get_bool(tags_res, "ok"), "SetTags failed");
	obs_data_release(tags_req);
	obs_data_release(tags_res);

	obs_data_t *create_req = obs_data_create();
	obs_data_t *create_res = obs_data_create();
	obs_data_set_string(create_req, "name", "Vertragsspiel");
	CHECK(call_ws_request("CreateGame", create_req, create_res) && obs_data_get_bool(create_res, "ok"),
	      "CreateGame failed");
	const char *new_game = obs_data_get_string(create_res, "gameId");
	CHECK(new_game && strncmp(new_game, "game-", 5) == 0, "CreateGame must return a gameId");
	char created_game[64];
	snprintf(created_game, sizeof created_game, "%s", new_game);
	obs_data_release(create_req);
	obs_data_release(create_res);
	st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after CreateGame failed");
	CHECK(strcmp(obs_data_get_string(st_res, "activeGameId"), "game_default") == 0,
	      "CreateGame must not activate the game, got '%s'", obs_data_get_string(st_res, "activeGameId"));
	obs_data_release(st_res);

	obs_data_t *all_req = obs_data_create();
	obs_data_t *all_res = obs_data_create();
	obs_data_set_string(all_req, "tags", "other");
	obs_data_set_string(all_req, "gameId", save_game);
	CHECK(call_ws_request("SaveAllAngles", all_req, all_res) && obs_data_get_bool(all_res, "ok"),
	      "SaveAllAngles failed: %s", obs_data_get_string(all_res, "message"));
	obs_data_array_t *jobs = obs_data_get_array(all_res, "jobs");
	CHECK(obs_data_array_count(jobs) == 1, "SaveAllAngles expected 1 job, got %zu", obs_data_array_count(jobs));
	obs_data_t *job0 = obs_data_array_item(jobs, 0);
	CHECK(strcmp(obs_data_get_string(job0, "libraryId"), saved_library_id) == 0, "SaveAllAngles did not merge");
	obs_data_release(job0);
	obs_data_array_release(jobs);
	obs_data_release(all_req);
	obs_data_release(all_res);

	obs_data_t *active_req = obs_data_create();
	obs_data_t *active_res = obs_data_create();
	obs_data_set_string(active_req, "gameId", created_game);
	CHECK(call_ws_request("SetActiveGame", active_req, active_res) && obs_data_get_bool(active_res, "ok"),
	      "SetActiveGame failed");
	obs_data_release(active_req);
	obs_data_release(active_res);

	obs_data_t *audio_req = obs_data_create();
	obs_data_t *audio_res = obs_data_create();
	obs_data_set_string(audio_req, "mode", "stereo");
	CHECK(call_ws_request("SetReplayAudio", audio_req, audio_res), "SetReplayAudio not registered");
	CHECK(!obs_data_get_bool(audio_res, "ok") && strcmp(obs_data_get_string(audio_res, "code"), "InvalidParameter") == 0,
	      "invalid audio mode must be rejected");
	obs_data_release(audio_req);
	obs_data_release(audio_res);
	audio_req = obs_data_create();
	audio_res = obs_data_create();
	obs_data_set_string(audio_req, "mode", "source");
	obs_data_set_string(audio_req, "sourceName", "Missing Audio");
	CHECK(call_ws_request("SetReplayAudio", audio_req, audio_res) && !obs_data_get_bool(audio_res, "ok"),
	      "missing audio source must be rejected");
	obs_data_release(audio_req);
	obs_data_release(audio_res);
	audio_req = obs_data_create();
	audio_res = obs_data_create();
	obs_data_set_string(audio_req, "mode", "angle");
	obs_data_set_string(audio_req, "angle", "Cam A");
	CHECK(call_ws_request("SetReplayAudio", audio_req, audio_res) && obs_data_get_bool(audio_res, "ok"),
	      "angle audio mode failed");
	obs_data_release(audio_req);
	obs_data_release(audio_res);
	st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after audio failed");
	obs_data_t *audio = obs_data_get_obj(st_res, "audio");
	clip = obs_data_get_obj(st_res, "clip");
	CHECK(audio && strcmp(obs_data_get_string(audio, "mode"), "angle") == 0, "audio.mode must be angle");
	CHECK(clip && strcmp(obs_data_get_string(clip, "audioMode"), "off") == 0,
	      "SetReplayAudio must not change the loaded clip");
	obs_data_release(audio);
	obs_data_release(clip);
	obs_data_release(st_res);
	audio_req = obs_data_create();
	audio_res = obs_data_create();
	obs_data_set_string(audio_req, "mode", "off");
	CHECK(call_ws_request("SetReplayAudio", audio_req, audio_res) && obs_data_get_bool(audio_res, "ok"),
	      "audio off failed");
	obs_data_release(audio_req);
	obs_data_release(audio_res);

	obs_data_t *stop_req = obs_data_create();
	obs_data_t *stop_res = obs_data_create();
	obs_data_set_string(stop_req, "action", "stop");
	CHECK(call_ws_request("PlaylistControl", stop_req, stop_res), "PlaylistControl not registered");
	CHECK(!obs_data_get_bool(stop_res, "ok") && strcmp(obs_data_get_string(stop_res, "code"), "NoActivePlaylist") == 0,
	      "stop without a playlist must be NoActivePlaylist");
	obs_data_release(stop_req);
	obs_data_release(stop_res);

	obs_data_t *missing_req = obs_data_create();
	obs_data_t *missing_res = obs_data_create();
	obs_data_set_string(missing_req, "libraryId", "lib-missing");
	CHECK(call_ws_request("LoadLibraryEntry", missing_req, missing_res), "LoadLibraryEntry not registered");
	CHECK(!obs_data_get_bool(missing_res, "ok") &&
		      strcmp(obs_data_get_string(missing_res, "code"), "EntryNotFound") == 0,
	      "missing library entry must be EntryNotFound");
	obs_data_release(missing_req);
	obs_data_release(missing_res);

	obs_data_t *load_req = obs_data_create();
	obs_data_t *load_res = obs_data_create();
	obs_data_set_string(load_req, "libraryId", saved_library_id);
	CHECK(call_ws_request("LoadLibraryEntry", load_req, load_res) && obs_data_get_bool(load_res, "ok"),
	      "LoadLibraryEntry failed: %s", obs_data_get_string(load_res, "message"));
	CHECK(obs_data_get_string(load_res, "jobId")[0], "LoadLibraryEntry must return a jobId");
	obs_data_release(load_req);
	obs_data_release(load_res);
	bool library_playing = false;
	for (int attempt = 0; attempt < 50 && !library_playing; attempt++) {
		st_res = obs_data_create();
		CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus during library load failed");
		clip = obs_data_get_obj(st_res, "clip");
		library_playing = clip && strcmp(obs_data_get_string(clip, "origin"), "library") == 0 &&
				  !obs_data_get_bool(clip, "loop") &&
				  strcmp(obs_data_get_string(st_res, "state"), "playing") == 0;
		obs_data_release(clip);
		obs_data_release(st_res);
		if (!library_playing)
			os_sleep_ms(100);
	}
	CHECK(library_playing, "library entry did not start playing with loop off");

	obs_data_t *play_req = obs_data_create();
	obs_data_t *play_res = obs_data_create();
	obs_data_set_string(play_req, "gameId", save_game);
	obs_data_set_string(play_req, "tags", "highlight");
	CHECK(call_ws_request("PlayPlaylist", play_req, play_res) && obs_data_get_bool(play_res, "ok"),
	      "PlayPlaylist failed: %s", obs_data_get_string(play_res, "message"));
	obs_data_release(play_req);
	obs_data_release(play_res);
	bool playlist_playing = false;
	for (int attempt = 0; attempt < 50 && !playlist_playing; attempt++) {
		st_res = obs_data_create();
		CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus during playlist failed");
		clip = obs_data_get_obj(st_res, "clip");
		obs_data_t *playlist = obs_data_get_obj(st_res, "playlist");
		playlist_playing = clip && strcmp(obs_data_get_string(clip, "origin"), "playlist") == 0 && playlist &&
				   strcmp(obs_data_get_string(playlist, "state"), "playing") == 0;
		obs_data_release(clip);
		obs_data_release(playlist);
		obs_data_release(st_res);
		if (!playlist_playing)
			os_sleep_ms(100);
	}
	CHECK(playlist_playing, "playlist did not start");
	stop_req = obs_data_create();
	stop_res = obs_data_create();
	obs_data_set_string(stop_req, "action", "stop");
	CHECK(call_ws_request("PlaylistControl", stop_req, stop_res) && obs_data_get_bool(stop_res, "ok"),
	      "playlist stop failed");
	obs_data_release(stop_req);
	obs_data_release(stop_res);
	st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after playlist stop failed");
	obs_data_t *playlist = obs_data_get_obj(st_res, "playlist");
	CHECK(playlist && strcmp(obs_data_get_string(playlist, "state"), "idle") == 0, "playlist stop must set idle");
	obs_data_release(playlist);
	obs_data_release(st_res);


	clear_fake_events();
	obs_data_t *rel_res = obs_data_create();
	CHECK(call_ws_request("Release", NULL, rel_res) && obs_data_get_bool(rel_res, "ok"), "Release failed");
	obs_data_release(rel_res);
	size_t released = 0;
	for (size_t i = 0; i < g_fake_ws.events.num; i++) {
		if (strcmp(g_fake_ws.events.array[i].type, "ClipReleased") == 0)
			released++;
	}
	CHECK(released == 1, "Release must emit ClipReleased, got %zu", released);
	st_res = obs_data_create();
	CHECK(call_ws_request("GetStatus", NULL, st_res), "GetStatus after release failed");
	CHECK(strcmp(obs_data_get_string(st_res, "state"), "idle") == 0, "state after release is '%s'",
	      obs_data_get_string(st_res, "state"));
	/* Clients read the JSON: an idle status must not carry "clip" (an empty object there is not null). */
	const char *idle_json = obs_data_get_json(st_res);
	CHECK(idle_json && !strstr(idle_json, "\"clip\""), "idle GetStatus must omit clip, got %s",
	      idle_json ? idle_json : "(null)");
	obs_data_release(st_res);

	obs_source_release(obs_scene_get_source(scene_a));
	obs_source_release(rs_a);
	obs_source_release(cam_a);

	/* Capture slack: with Duration 1000 ms every filter keeps 2 s. Two angles delivering 300 ms apart still share
	 * a 1 s window, so a full-duration Trigger takes both; a single-source Load stays at the 1 s duration. A third
	 * angle still receiving (not stale) but 1.2 s behind must be skipped as history, not pull the window back past
	 * the history of the healthy angles. All timestamps stay within MAX_TS_VAR of now, otherwise the filter rebases
	 * them as a clock jump. */
	const char *slack_names[3][3] = {{"Slack Cam X", "Slack Raw X", "Replay Slack X"},
					 {"Slack Cam Y", "Slack Raw Y", "Replay Slack Y"},
					 {"Slack Cam Z", "Slack Raw Z", "Replay Slack Z"}};
	obs_source_t *slack_cams[3];
	obs_source_t *slack_rs[3];
	obs_scene_t *slack_scenes[3];
	for (int i = 0; i < 3; i++) {
		slack_cams[i] = obs_source_create("dummy_async_cam", slack_names[i][0], NULL, NULL);
		slack_rs[i] = obs_source_create(REPLAY_SOURCE_ID, slack_names[i][1], NULL, NULL);
		CHECK(slack_cams[i] && slack_rs[i], "failed to create slack sources");
		obs_data_t *ss = obs_source_get_settings(slack_rs[i]);
		obs_data_set_string(ss, SETTING_SOURCE, slack_names[i][0]);
		obs_data_set_int(ss, SETTING_DURATION, 1000);
		obs_source_update(slack_rs[i], ss);
		obs_source_load(slack_rs[i]);
		obs_data_release(ss);
		slack_scenes[i] = obs_scene_create(slack_names[i][2]);
		CHECK(slack_scenes[i] && obs_scene_add(slack_scenes[i], slack_rs[i]), "failed to create slack scene");
	}
	const uint64_t slack_now = os_gettime_ns();
	feed_nv12_frames(slack_cams[0], 320, 180, 99, slack_now - 99ULL * contract_interval, contract_interval);
	feed_nv12_frames(slack_cams[1], 320, 180, 99, slack_now - 300000000ULL - 99ULL * contract_interval,
			 contract_interval);
	feed_nv12_frames(slack_cams[2], 320, 180, 30, slack_now - 1200000000ULL - 30ULL * contract_interval,
			 contract_interval);

	trig_req = obs_data_create();
	trig_res = obs_data_create();
	obs_data_set_int(trig_req, "lookbackMs", 1000);
	CHECK(call_ws_request("Trigger", trig_req, trig_res) && obs_data_get_bool(trig_res, "ok"),
	      "full-duration Trigger over offset angles failed: %s", obs_data_get_string(trig_res, "message"));
	obs_data_array_t *slack_skipped = obs_data_get_array(trig_res, "skipped");
	const size_t slack_skipped_count = slack_skipped ? obs_data_array_count(slack_skipped) : 99;
	obs_data_t *slack_skip0 = slack_skipped_count == 1 ? obs_data_array_item(slack_skipped, 0) : NULL;
	CHECK(slack_skip0 && strcmp(obs_data_get_string(slack_skip0, "angle"), "Slack Z") == 0 &&
		      strcmp(obs_data_get_string(slack_skip0, "reason"), "history") == 0,
	      "only the lagging angle may be skipped (as history), %zu skipped, first %s", slack_skipped_count,
	      slack_skip0 ? obs_data_get_string(slack_skip0, "angle") : "-");
	obs_data_release(slack_skip0);
	obs_data_array_release(slack_skipped);
	obs_data_release(trig_req);
	obs_data_release(trig_res);
	rel_res = obs_data_create();
	CHECK(call_ws_request("Release", NULL, rel_res) && obs_data_get_bool(rel_res, "ok"), "slack Release failed");
	obs_data_release(rel_res);

	struct replay_source *slack_ctx = obs_obj_get_data(slack_rs[0]);
	CHECK(replay_source_execute_action(slack_ctx, "Load", false), "single-source Load failed");
	struct replay_source_status slack_st;
	CHECK(replay_source_get_status(slack_ctx, &slack_st), "slack status failed");
	CHECK(slack_st.duration_ms >= 950 && slack_st.duration_ms <= 1000,
	      "single-source Load must keep the configured 1000 ms, got %lld", (long long)slack_st.duration_ms);

	for (int i = 0; i < 3; i++) {
		obs_source_release(obs_scene_get_source(slack_scenes[i]));
		obs_source_release(slack_rs[i]);
		obs_source_release(slack_cams[i]);
	}

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
	clear_fake_events();
	da_free(g_fake_ws.events);
	for (size_t i = 0; i < g_fake_ws.vendor_count; i++)
		bfree(g_fake_ws.vendor_names[i]);
	proc_handler_destroy(g_fake_ws.ph);

	obs_shutdown();
	printf("[test-control] ALL CONTROL TESTS PASSED!\n");
	return 0;
}