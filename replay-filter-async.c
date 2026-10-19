#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/darray.h>
#include "media-io/audio-math.h"
#include "obs-frame-tap.h"
#include "replay.h"
#include "replay-pool.h"
#include "replay-convert.h"

static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplayFilterAsync");
}

static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ? (ts2 - ts1) : (ts1 - ts2);
}

static inline uint64_t safe_video_frame_time(void)
{
	if (obs_initialized()) {
		uint64_t t = obs_get_video_frame_time();
		if (t)
			return t;
	}
	return os_gettime_ns();
}

/* Live delivery lateness (replay_filter_live_snapshot). The reference decays by 1 ms per second so clock drift
 * between camera and PC never accumulates; lateness lasting longer than LIVE_REBASE_NS is taken as a new
 * steady offset (e.g. a camera timecode step below MAX_TS_VAR), not as a delivery delay. Silence itself is not
 * lateness (NDI sources may send only on change); a value older than LIVE_STALE_NS no longer counts. */
#define LIVE_REF_DECAY_DIV 1000
#define LIVE_REBASE_NS 10000000000ULL
#define LIVE_LATE_NOTICE_NS 100000000LL
#define LIVE_STALE_NS 2000000000ULL

static pthread_once_t live_registry_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t live_registry_mutex;
static DARRAY(struct replay_filter *) live_filters;

static void live_registry_init(void)
{
	pthread_mutex_init(&live_registry_mutex, NULL);
	da_init(live_filters);
}

static void live_registry_add(struct replay_filter *filter)
{
	pthread_once(&live_registry_once, live_registry_init);
	pthread_mutex_lock(&live_registry_mutex);
	da_push_back(live_filters, &filter);
	pthread_mutex_unlock(&live_registry_mutex);
}

static void live_registry_remove(struct replay_filter *filter)
{
	pthread_once(&live_registry_once, live_registry_init);
	pthread_mutex_lock(&live_registry_mutex);
	da_erase_item(live_filters, &filter);
	if (!live_filters.num)
		da_free(live_filters);
	pthread_mutex_unlock(&live_registry_mutex);
}

uint64_t replay_filter_live_snapshot(uint64_t now_ns, struct replay_live_entry *out, size_t max, size_t *count_out)
{
	uint64_t worst = 0;
	size_t n = 0;
	pthread_once(&live_registry_once, live_registry_init);
	pthread_mutex_lock(&live_registry_mutex);
	for (size_t i = 0; i < live_filters.num; i++) {
		struct replay_filter *filter = live_filters.array[i];
		pthread_mutex_lock(&filter->mutex);
		const bool valid = filter->live_ref_valid;
		const uint64_t last = filter->live_last_ns;
		uint64_t late = filter->live_late_ns > 0 ? (uint64_t)filter->live_late_ns : 0;
		pthread_mutex_unlock(&filter->mutex);
		if (!valid)
			continue;
		if (now_ns > last && now_ns - last >= LIVE_STALE_NS)
			late = 0;
		if (late > worst)
			worst = late;
		if (out && n < max) {
			const char *name = filter->src ? obs_source_get_name(filter->src) : NULL;
			snprintf(out[n].name, sizeof(out[n].name), "%s", name ? name : "");
			out[n].late_ns = late;
			n++;
		}
	}
	pthread_mutex_unlock(&live_registry_mutex);
	if (count_out)
		*count_out = n;
	return worst;
}

static void update_live_lateness_locked(struct replay_filter *filter, int64_t ts_arrival_ns, uint64_t arrival_ns,
					bool rebased)
{
	filter->live_last_ns = arrival_ns;
	if (filter->live_ref_valid && !rebased) {
		const int64_t decay = (int64_t)((arrival_ns - filter->live_ref_at_ns) / LIVE_REF_DECAY_DIV);
		const int64_t ref = filter->live_ref_ns - decay;
		if (ts_arrival_ns < ref) {
			const int64_t late = ref - ts_arrival_ns;
			if (late >= LIVE_LATE_NOTICE_NS && filter->live_late_ns < LIVE_LATE_NOTICE_NS)
				filter->live_late_since_ns = arrival_ns;
			if (late < LIVE_LATE_NOTICE_NS || arrival_ns - filter->live_late_since_ns < LIVE_REBASE_NS) {
				filter->live_late_ns = late;
				return;
			}
		}
	}
	filter->live_ref_ns = ts_arrival_ns;
	filter->live_ref_at_ns = arrival_ns;
	filter->live_late_ns = 0;
	filter->live_ref_valid = true;
}

static uint32_t estimate_pool_frames(const struct replay_filter *filter)
{
	if (filter->duration == 0)
		return 0;
	struct obs_video_info ovi;
	uint32_t fps_num = 60;
	uint32_t fps_den = 1;
	if (obs_initialized() && obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0) {
		fps_num = ovi.fps_num;
		fps_den = ovi.fps_den;
	}
	uint64_t buffer_frames = (filter->duration * (uint64_t)fps_num) / (SEC_TO_NSEC * (uint64_t)fps_den) + 4;
	if (buffer_frames > 36000)
		buffer_frames = 36000;

	uint64_t multiplier = 1;
	if (filter->reserve_mode == RESERVE_BUFFER_PLUS_ONE) {
		multiplier = 2;
	} else if (filter->reserve_mode == RESERVE_BUFFER_PLUS_MAX) {
		const int max_r = filter->max_replays > 0 ? filter->max_replays : 1;
		multiplier = 1 + (uint64_t)max_r;
	}
	return (uint32_t)(buffer_frames * multiplier);
}

static void log_reserve_target_locked(struct replay_filter *filter, uint32_t target_frames)
{
	if (!filter->pool)
		return;
	if (filter->logged_reserve_mode == filter->reserve_mode && filter->logged_reserve_target == target_frames)
		return;
	filter->logged_reserve_mode = filter->reserve_mode;
	filter->logged_reserve_target = target_frames;

	const size_t frame_bytes = replay_frame_byte_size(replay_pool_get_format(filter->pool),
							  replay_pool_get_width(filter->pool),
							  replay_pool_get_height(filter->pool));
	const double target_gb = ((double)target_frames * (double)frame_bytes) / (1024.0 * 1024.0 * 1024.0);
	const char *name = filter->src ? obs_source_get_name(filter->src) : "";
	blog(LOG_INFO, "[replay-filter] '%s' reserve: mode=%d target=%u frames (%.2f GB)", name ? name : "",
	     filter->reserve_mode, target_frames, target_gb);
}

static struct replay_pool *ensure_filter_pool_locked(struct replay_filter *filter, enum video_format dst_format,
						     uint32_t dst_width, uint32_t dst_height)
{
	if (!filter->pool || replay_pool_get_format(filter->pool) != dst_format ||
	    replay_pool_get_width(filter->pool) != dst_width || replay_pool_get_height(filter->pool) != dst_height) {
		replay_pool_close(filter->pool);
		const uint32_t target = estimate_pool_frames(filter);
		filter->pool = replay_pool_create(dst_format, dst_width, dst_height, target);
		filter->logged_reserve_mode = -1;
		filter->logged_reserve_target = 0;
		log_reserve_target_locked(filter, target);
	}
	return filter->pool;
}

static void record_capture_timing_locked(struct replay_filter *filter, uint64_t t0_ns, uint64_t elapsed_ns,
					 int64_t ts_arrival_ns)
{
	if (filter->capture_count == 0 || ts_arrival_ns < filter->ts_arrival_min_ns)
		filter->ts_arrival_min_ns = ts_arrival_ns;
	if (filter->capture_count == 0 || ts_arrival_ns > filter->ts_arrival_max_ns)
		filter->ts_arrival_max_ns = ts_arrival_ns;
	filter->capture_count++;
	filter->capture_sum_ns += elapsed_ns;
	if (elapsed_ns > filter->capture_max_ns)
		filter->capture_max_ns = elapsed_ns;
	if (filter->capture_log_ts == 0) {
		filter->capture_log_ts = t0_ns;
	} else if (t0_ns - filter->capture_log_ts >= 60ULL * SEC_TO_NSEC) {
		struct replay_pool_stats stats = {0};
		if (filter->pool)
			replay_pool_get_stats(filter->pool, &stats);
		const double avg_ms = (double)filter->capture_sum_ns / (double)filter->capture_count / 1e6;
		const double max_ms = (double)filter->capture_max_ns / 1e6;
		const double pool_gb = (double)stats.allocated_bytes / (1024.0 * 1024.0 * 1024.0);
		const char *name = filter->src ? obs_source_get_name(filter->src) : "";
		blog(LOG_INFO,
		     "[replay-filter] '%s' capture: frames=%u avg=%.3fms max=%.3fms pool=%u frames %.2f GB peak=%u "
		     "ts-arrival=%+.0f..%+.0fms",
		     name ? name : "", filter->capture_count, avg_ms, max_ms, stats.allocated_frames, pool_gb,
		     stats.peak_frames, (double)filter->ts_arrival_min_ns / 1e6, (double)filter->ts_arrival_max_ns / 1e6);
		filter->capture_count = 0;
		filter->capture_sum_ns = 0;
		filter->capture_max_ns = 0;
		filter->capture_log_ts = t0_ns;
	}
}

static bool capture_single_frame_locked(struct replay_filter *filter, const struct obs_source_frame *src_frame,
					uint64_t os_time, uint64_t *last_timestamp)
{
	if (!src_frame || !src_frame->data[0] || src_frame->width == 0 || src_frame->height == 0)
		return false;

	if (src_frame->timestamp + filter->timing_adjust <= *last_timestamp)
		return false;

	const uint64_t t0 = os_gettime_ns();

	enum video_format dst_format;
	uint32_t dst_width;
	uint32_t dst_height;
	replay_convert_target_format(src_frame->format, src_frame->width, src_frame->height, filter->storage_format,
				     filter->downscale == DOWNSCALE_MAX_1080P, &dst_format, &dst_width, &dst_height);

	struct replay_pool *pool = ensure_filter_pool_locked(filter, dst_format, dst_width, dst_height);
	if (!pool)
		return false;

	struct obs_source_frame *new_frame = replay_pool_acquire(pool);
	if (!new_frame)
		return false;

	if (!replay_convert_frame(new_frame, src_frame)) {
		replay_frame_release(new_frame);
		return false;
	}

	const uint64_t timestamp = src_frame->timestamp;
	const int64_t prev_adjust = filter->timing_adjust;
	uint64_t adjusted_time = timestamp + filter->timing_adjust;
	if (filter->timing_adjust && uint64_diff(os_time, timestamp) < MAX_TS_VAR) {
		adjusted_time = timestamp;
		filter->timing_adjust = 0;
	} else if (uint64_diff(os_time, adjusted_time) > MAX_TS_VAR) {
		filter->timing_adjust = os_time - timestamp;
		adjusted_time = os_time;
	}
	new_frame->timestamp = adjusted_time;
	*last_timestamp = adjusted_time;
	circlebuf_push_back(&filter->video_frames, &new_frame, sizeof(struct obs_source_frame *));
	filter->last_arrival_ns = t0;
	update_live_lateness_locked(filter, (int64_t)(adjusted_time - t0), t0, filter->timing_adjust != prev_adjust);

	const uint64_t elapsed = os_gettime_ns() - t0;
	record_capture_timing_locked(filter, t0, elapsed, (int64_t)(adjusted_time - t0));
	return true;
}

static void evict_expired_frames_locked(struct replay_filter *filter, uint64_t last_timestamp)
{
	if (!last_timestamp || !filter->video_frames.size)
		return;

	struct obs_source_frame *output = NULL;
	circlebuf_peek_front(&filter->video_frames, &output, sizeof(struct obs_source_frame *));

	uint64_t cur_duration = last_timestamp - output->timestamp;
	while (cur_duration > 0 && cur_duration > filter->duration) {
		circlebuf_pop_front(&filter->video_frames, NULL, sizeof(struct obs_source_frame *));
		replay_frame_release(output);
		output = NULL;
		if (filter->video_frames.size) {
			circlebuf_peek_front(&filter->video_frames, &output, sizeof(struct obs_source_frame *));
			cur_duration = last_timestamp - output->timestamp;
		} else {
			cur_duration = 0;
		}
	}
}

static void replay_filter_tap_cb(void *param, obs_source_t *source, const struct obs_source_frame *frame)
{
	UNUSED_PARAMETER(source);
	struct replay_filter *filter = param;
	if (!filter || !frame)
		return;

	const uint64_t os_time = safe_video_frame_time();

	pthread_mutex_lock(&filter->mutex);
	uint64_t last_timestamp = 0;
	if (filter->video_frames.size) {
		struct obs_source_frame *output = NULL;
		circlebuf_peek_back(&filter->video_frames, &output, sizeof(struct obs_source_frame *));
		last_timestamp = output->timestamp;
	}
	if (capture_single_frame_locked(filter, frame, os_time, &last_timestamp))
		evict_expired_frames_locked(filter, last_timestamp);
	pthread_mutex_unlock(&filter->mutex);
}

static void try_attach_frame_tap(struct replay_filter *filter)
{
	if (!filter || filter->tap_checked || !filter->src)
		return;

	pthread_mutex_lock(&filter->tap_mutex);
	obs_source_t *parent = NULL;
	if (!filter->tap_checked && !filter->tap_closed)
		parent = obs_filter_get_parent(filter->src);
	proc_handler_t *ph = parent ? obs_source_get_proc_handler(parent) : NULL;
	if (parent)
		filter->tap_checked = true;
	if (!ph) {
		pthread_mutex_unlock(&filter->tap_mutex);
		return;
	}

	struct calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "callback", (void *)(uintptr_t)replay_filter_tap_cb);
	calldata_set_ptr(&cd, "param", filter);

	if (proc_handler_call(ph, FRAME_TAP_PROC_ADD, &cd)) {
		filter->tap_parent = obs_source_get_weak_source(parent);
		os_atomic_set_bool(&filter->tap_active, true);
		const char *fname = obs_source_get_name(filter->src);
		const char *pname = obs_source_get_name(parent);
		blog(LOG_INFO, "[replay-filter] '%s' frame tap active on source '%s'", fname ? fname : "",
		     pname ? pname : "");
	}
	pthread_mutex_unlock(&filter->tap_mutex);
}

/* Terminal: called on filter removal and destroy; the filter never attaches again afterwards. The tap
 * provider's remove waits for an in-flight callback, so no tap frame reaches the filter after this. */
static void detach_frame_tap(struct replay_filter *filter, obs_source_t *explicit_parent)
{
	if (!filter)
		return;

	pthread_mutex_lock(&filter->tap_mutex);
	filter->tap_closed = true;
	if (!os_atomic_set_bool(&filter->tap_active, false)) {
		if (filter->tap_parent) {
			obs_weak_source_release(filter->tap_parent);
			filter->tap_parent = NULL;
		}
		pthread_mutex_unlock(&filter->tap_mutex);
		return;
	}

	obs_source_t *strong_parent = NULL;
	obs_source_t *parent = explicit_parent;
	if (!parent && filter->tap_parent) {
		strong_parent = obs_weak_source_get_source(filter->tap_parent);
		parent = strong_parent;
	}
	if (!parent && filter->src)
		parent = obs_filter_get_parent(filter->src);

	proc_handler_t *ph = parent ? obs_source_get_proc_handler(parent) : NULL;
	if (ph) {
		struct calldata cd;
		uint8_t stack[128];
		calldata_init_fixed(&cd, stack, sizeof(stack));
		calldata_set_ptr(&cd, "callback", (void *)(uintptr_t)replay_filter_tap_cb);
		calldata_set_ptr(&cd, "param", filter);
		proc_handler_call(ph, FRAME_TAP_PROC_REMOVE, &cd);
	} else {
		const char *fname = filter->src ? obs_source_get_name(filter->src) : NULL;
		blog(LOG_WARNING, "[replay-filter] '%s' frame tap could not be removed: parent source gone",
		     fname ? fname : "");
	}

	if (strong_parent)
		obs_source_release(strong_parent);
	if (filter->tap_parent) {
		obs_weak_source_release(filter->tap_parent);
		filter->tap_parent = NULL;
	}
	pthread_mutex_unlock(&filter->tap_mutex);
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;

	const uint64_t new_duration =
		replay_capture_duration_ns((uint64_t)obs_data_get_int(settings, SETTING_DURATION) * MSEC_TO_NSEC);
	const int new_storage = (int)obs_data_get_int(settings, SETTING_STORAGE_FORMAT);
	const int new_downscale = (int)obs_data_get_int(settings, SETTING_DOWNSCALE);
	const int new_reserve = (int)obs_data_get_int(settings, SETTING_RESERVE_MODE);
	int new_max_replays = (int)obs_data_get_int(settings, SETTING_REPLAYS);
	if (new_max_replays < 1)
		new_max_replays = 1;

	pthread_mutex_lock(&filter->mutex);
	const bool format_or_scale_changed =
		(new_storage != filter->storage_format || new_downscale != filter->downscale);
	if (new_duration < filter->duration || format_or_scale_changed) {
		free_video_data(filter);
	}
	if (format_or_scale_changed && filter->pool) {
		replay_pool_close(filter->pool);
		filter->pool = NULL;
		filter->logged_reserve_mode = -1;
		filter->logged_reserve_target = 0;
	}
	filter->duration = new_duration;
	filter->storage_format = new_storage;
	filter->downscale = new_downscale;
	filter->reserve_mode = new_reserve;
	filter->max_replays = new_max_replays;
	/* Changing reserve_mode or duration only grows the pool in the background warm-up
	 * thread; the pool never shrinks so in-flight frames and existing allocations stay valid. */
	if (filter->pool) {
		const uint32_t target = estimate_pool_frames(filter);
		replay_pool_warmup(filter->pool, target);
		log_reserve_target_locked(filter, target);
	}
	pthread_mutex_unlock(&filter->mutex);

	filter->internal_frames = obs_data_get_bool(settings, SETTING_INTERNAL_FRAMES);
	const double db = obs_data_get_double(settings, SETTING_AUDIO_THRESHOLD);
	filter->threshold = db_to_mul((float)db);

	try_attach_frame_tap(filter);
}

static void *replay_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct replay_filter *context = bzalloc(sizeof(struct replay_filter));
	context->src = source;
	context->logged_reserve_mode = -1;
	pthread_mutex_init(&context->mutex, NULL);
	pthread_mutex_init(&context->tap_mutex, NULL);
	context->last_check = safe_video_frame_time();

	obs_data_set_default_int(settings, SETTING_RESERVE_MODE, RESERVE_BUFFER_PLUS_ONE);
	obs_data_set_default_int(settings, SETTING_REPLAYS, 1);

	replay_filter_update(context, settings);
	live_registry_add(context);

	return context;
}

static void replay_filter_destroy(void *data)
{
	struct replay_filter *filter = data;

	live_registry_remove(filter);
	detach_frame_tap(filter, NULL);

	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	replay_pool_close(filter->pool);
	filter->pool = NULL;
	pthread_mutex_unlock(&filter->mutex);
	circlebuf_free(&filter->video_frames);
	circlebuf_free(&filter->audio_frames);
	pthread_mutex_destroy(&filter->mutex);
	pthread_mutex_destroy(&filter->tap_mutex);
	bfree(data);
}

static void replay_filter_remove(void *data, obs_source_t *parent)
{
	struct replay_filter *filter = data;

	detach_frame_tap(filter, parent);

	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	replay_pool_close(filter->pool);
	filter->pool = NULL;
	filter->logged_reserve_mode = -1;
	filter->logged_reserve_target = 0;
	pthread_mutex_unlock(&filter->mutex);
}

struct async_frame {
	struct obs_source_frame *frame;
	long unused_count;
	bool used;
};

static struct obs_source_frame *replay_filter_video(void *data, struct obs_source_frame *frame)
{
	struct replay_filter *filter = data;
	if (!filter->tap_checked)
		try_attach_frame_tap(filter);
	if (os_atomic_load_bool(&filter->tap_active))
		return frame;

	struct obs_source_frame *output;
	uint64_t last_timestamp = 0;

	obs_source_t *target = filter->internal_frames ? obs_filter_get_parent(filter->src) : NULL;
	const uint64_t os_time = safe_video_frame_time();

	pthread_mutex_lock(&filter->mutex);
	if (filter->video_frames.size) {
		circlebuf_peek_back(&filter->video_frames, &output, sizeof(struct obs_source_frame *));
		last_timestamp = output->timestamp;
	}
	if (target) {
		if (filter->target_offset == 0) {
			// (uint8_t *)&source->async_cache - (uint8_t *)source
			uint32_t obs_version = obs_get_version();
			if (obs_version < MAKE_SEMANTIC_VERSION(30, 0, 0)) {
				filter->target_offset = 2000;
			} else if (obs_version < MAKE_SEMANTIC_VERSION(30, 2, 0)) {
				filter->target_offset = 2008;
			} else if (obs_version >= MAKE_SEMANTIC_VERSION(32, 0, 0)) {
				filter->target_offset = 2016;
			} else {
				filter->target_offset = 2000;
			}
		}
		struct darray *async_cache = (struct darray *)((uint8_t *)target + filter->target_offset);
		pthread_mutex_t *async_mutex =
			(pthread_mutex_t *)((uint8_t *)target + filter->target_offset + sizeof(struct darray) * 2);
		pthread_mutex_lock(async_mutex);
		for (size_t i = 0; i < async_cache->num; i++) {
			struct obs_source_frame *extra_frame = ((struct async_frame *)async_cache->array)[i].frame;
			capture_single_frame_locked(filter, extra_frame, os_time, &last_timestamp);
		}
		pthread_mutex_unlock(async_mutex);
	}
	capture_single_frame_locked(filter, frame, os_time, &last_timestamp);
	evict_expired_frames_locked(filter, last_timestamp);
	pthread_mutex_unlock(&filter->mutex);
	return frame;
}

static obs_properties_t *replay_filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_int(props, SETTING_DURATION, obs_module_text("Duration"), SETTING_DURATION_MIN,
						      SETTING_DURATION_MAX, 1000);
	obs_property_int_set_suffix(prop, "ms");
	obs_properties_add_bool(props, SETTING_INTERNAL_FRAMES, obs_module_text("CaptureInternalFrames"));
	obs_properties_add_float_slider(props, SETTING_AUDIO_THRESHOLD, obs_module_text("ThresholdDb"), SETTING_AUDIO_THRESHOLD_MIN,
					SETTING_AUDIO_THRESHOLD_MAX, 0.1);

	return props;
}

static void replay_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	try_attach_frame_tap(data);
	replay_filter_check(data);
}

struct obs_source_info replay_filter_async_info = {
	.id = REPLAY_FILTER_ASYNC_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC,
	.create = replay_filter_create,
	.destroy = replay_filter_destroy,
	.update = replay_filter_update,
	.load = replay_filter_update,
	.video_tick = replay_filter_tick,
	.get_name = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_video = replay_filter_video,
	.filter_audio = replay_filter_audio,
	.filter_remove = replay_filter_remove,
};
