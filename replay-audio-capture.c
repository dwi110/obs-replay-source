#include "replay-audio-capture.h"
#include "replay.h"
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>

static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static obs_weak_source_t *g_weak_source = NULL;
static char g_source_name[256] = "";
static bool g_configured = false;
static int64_t g_offset_ns = 0;
static uint64_t g_history_ns = 30000000000ULL;
static int64_t g_timing_adjust = 0;
static uint64_t g_prev_raw_end = 0;
static struct audio_convert_info g_oai = {0};

static struct obs_audio_data *g_packets = NULL;
static size_t g_head = 0;
static size_t g_count = 0;
static size_t g_cap = 0;

static void audio_capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted);

static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ? (ts2 - ts1) : (ts1 - ts2);
}

static inline uint64_t packet_duration_ns(uint32_t frames, uint32_t rate)
{
	if (rate == 0)
		rate = 48000;
	return util_mul_div64((uint64_t)frames, 1000000000ULL, (uint64_t)rate);
}

static void clear_buffer_locked(void)
{
	for (size_t i = 0; i < g_count; i++)
		free_audio_packet(&g_packets[(g_head + i) % g_cap]);
	g_head = 0;
	g_count = 0;
	g_prev_raw_end = 0;
}

static void trim_history_locked(uint32_t rate)
{
	if (g_count <= 1)
		return;
	size_t newest_idx = (g_head + g_count - 1) % g_cap;
	uint64_t newest_dur = packet_duration_ns(g_packets[newest_idx].frames, rate);
	uint64_t newest_end = g_packets[newest_idx].timestamp + newest_dur;
	uint64_t hist = g_history_ns ? g_history_ns : 30000000000ULL;
	while (g_count > 1) {
		uint64_t oldest_dur = packet_duration_ns(g_packets[g_head].frames, rate);
		uint64_t oldest_end = g_packets[g_head].timestamp + oldest_dur;
		if (oldest_end + hist <= newest_end) {
			free_audio_packet(&g_packets[g_head]);
			g_head = (g_head + 1) % g_cap;
			g_count--;
		} else {
			break;
		}
	}
}

static void push_packet_locked(const struct obs_audio_data *pkt)
{
	if (g_count == g_cap) {
		size_t new_cap = g_cap ? g_cap * 2 : 256;
		struct obs_audio_data *new_pkts = bmalloc(new_cap * sizeof(struct obs_audio_data));
		for (size_t i = 0; i < g_count; i++)
			new_pkts[i] = g_packets[(g_head + i) % g_cap];
		bfree(g_packets);
		g_packets = new_pkts;
		g_head = 0;
		g_cap = new_cap;
	}
	size_t push_idx = (g_head + g_count) % g_cap;
	g_packets[push_idx] = *pkt;
	g_count++;
}

static void capture_push_internal(const struct audio_data *audio_data, bool muted)
{
	if (!audio_data || !audio_data->data[0] || audio_data->frames == 0)
		return;

	pthread_mutex_lock(&capture_mutex);
	if (!g_configured) {
		pthread_mutex_unlock(&capture_mutex);
		return;
	}

	if (g_oai.samples_per_sec == 0 || g_oai.format != AUDIO_FORMAT_FLOAT_PLANAR) {
		struct obs_audio_info oai;
		if (obs_get_audio_info(&oai) && oai.samples_per_sec > 0) {
			g_oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
			g_oai.speakers = oai.speakers;
			g_oai.samples_per_sec = oai.samples_per_sec;
		} else {
			g_oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
			g_oai.speakers = SPEAKERS_STEREO;
			g_oai.samples_per_sec = 48000;
		}
	}

	struct obs_audio_data in;
	memset(&in, 0, sizeof(in));
	in.frames = audio_data->frames;
	in.timestamp = audio_data->timestamp;
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		in.data[i] = audio_data->data[i];

	struct obs_audio_data cached;
	replay_audio_packet_copy(&cached, &in);
	if (muted) {
		for (size_t i = 0; i < MAX_AV_PLANES && cached.data[i]; i++)
			memset(cached.data[i], 0, (size_t)cached.frames * sizeof(float));
	}

	const uint64_t timestamp = cached.timestamp;
	uint64_t adjusted_time = timestamp + g_timing_adjust;
	const uint64_t os_time = os_gettime_ns();
	if (g_timing_adjust && uint64_diff(os_time, timestamp) < MAX_TS_VAR) {
		adjusted_time = timestamp;
		g_timing_adjust = 0;
	} else if (uint64_diff(os_time, adjusted_time) > MAX_TS_VAR) {
		g_timing_adjust = (int64_t)os_time - (int64_t)timestamp;
		adjusted_time = os_time;
	}

	int64_t with_offset = (int64_t)adjusted_time + g_offset_ns;
	if (with_offset < 0)
		with_offset = 0;
	cached.timestamp = (uint64_t)with_offset;

	uint32_t rate = g_oai.samples_per_sec ? g_oai.samples_per_sec : 48000;
	uint64_t raw_dur = packet_duration_ns(audio_data->frames, rate);
	uint64_t raw_end = audio_data->timestamp + raw_dur;

	if (g_count > 0) {
		size_t last_idx = (g_head + g_count - 1) % g_cap;
		uint64_t last_dur = packet_duration_ns(g_packets[last_idx].frames, rate);
		uint64_t prev_buf_end = g_packets[last_idx].timestamp + last_dur;
		bool raw_jump = (g_prev_raw_end > 0 && audio_data->timestamp + 1000000ULL < g_prev_raw_end);
		bool buf_jump = (prev_buf_end > 0 && cached.timestamp + 1000000ULL < prev_buf_end);
		if (raw_jump || buf_jump) {
			blog(LOG_INFO,
			     "replay-audio-capture: backwards jump detected (raw %" PRIu64 " < %" PRIu64
			     " - 1ms, buf %" PRIu64 " < %" PRIu64 " - 1ms), clearing history",
			     audio_data->timestamp, g_prev_raw_end, cached.timestamp, prev_buf_end);
			clear_buffer_locked();
			g_timing_adjust = 0;
		}
	}
	g_prev_raw_end = raw_end;

	push_packet_locked(&cached);
	trim_history_locked(rate);
	pthread_mutex_unlock(&capture_mutex);
}

static void audio_capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(source);
	capture_push_internal(audio_data, muted);
}

void replay_audio_capture_push_for_test(const struct audio_data *audio_data, bool muted)
{
	pthread_mutex_lock(&capture_mutex);
	if (!g_configured) {
		g_configured = true;
		if (g_history_ns == 0)
			g_history_ns = 30000000000ULL;
	}
	pthread_mutex_unlock(&capture_mutex);
	capture_push_internal(audio_data, muted);
}

bool replay_audio_capture_configure(const char *source_name, int64_t offset_ns, uint64_t history_ns, char *err,
				    size_t err_size)
{
	if (err && err_size > 0)
		err[0] = '\0';

	if (offset_ns < -5000000000LL)
		offset_ns = -5000000000LL;
	else if (offset_ns > 5000000000LL)
		offset_ns = 5000000000LL;

	if (history_ns == 0)
		history_ns = 30000000000ULL;

	if (!source_name || source_name[0] == '\0') {
		obs_weak_source_t *weak = NULL;
		pthread_mutex_lock(&capture_mutex);
		weak = g_weak_source;
		g_weak_source = NULL;
		g_source_name[0] = '\0';
		g_configured = false;
		g_timing_adjust = 0;
		g_offset_ns = 0;
		g_history_ns = history_ns;
		clear_buffer_locked();
		pthread_mutex_unlock(&capture_mutex);

		if (weak) {
			obs_source_t *src = obs_weak_source_get_source(weak);
			if (src) {
				obs_source_remove_audio_capture_callback(src, audio_capture_callback, NULL);
				obs_source_release(src);
			}
			obs_weak_source_release(weak);
		}
		return true;
	}

	obs_source_t *source = obs_get_source_by_name(source_name);
	if (!source) {
		if (err && err_size > 0)
			snprintf(err, err_size, "source not found");
		return false;
	}

	obs_weak_source_t *prev_weak = NULL;
	pthread_mutex_lock(&capture_mutex);
	prev_weak = g_weak_source;
	g_weak_source = obs_source_get_weak_source(source);
	snprintf(g_source_name, sizeof(g_source_name), "%s", source_name);
	g_configured = true;
	g_offset_ns = offset_ns;
	g_history_ns = history_ns;
	g_timing_adjust = 0;
	clear_buffer_locked();

	struct obs_audio_info oai;
	if (obs_get_audio_info(&oai) && oai.samples_per_sec > 0) {
		g_oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
		g_oai.speakers = oai.speakers;
		g_oai.samples_per_sec = oai.samples_per_sec;
	} else {
		g_oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
		g_oai.speakers = SPEAKERS_STEREO;
		g_oai.samples_per_sec = 48000;
	}
	pthread_mutex_unlock(&capture_mutex);

	if (prev_weak) {
		obs_source_t *prev_src = obs_weak_source_get_source(prev_weak);
		if (prev_src) {
			obs_source_remove_audio_capture_callback(prev_src, audio_capture_callback, NULL);
			obs_source_release(prev_src);
		}
		obs_weak_source_release(prev_weak);
	}

	obs_source_add_audio_capture_callback(source, audio_capture_callback, NULL);
	obs_source_release(source);
	return true;
}

void replay_audio_capture_shutdown(void)
{
	obs_weak_source_t *weak = NULL;
	pthread_mutex_lock(&capture_mutex);
	weak = g_weak_source;
	g_weak_source = NULL;
	g_source_name[0] = '\0';
	g_configured = false;
	g_timing_adjust = 0;
	g_offset_ns = 0;
	g_history_ns = 0;
	clear_buffer_locked();
	bfree(g_packets);
	g_packets = NULL;
	g_cap = 0;
	pthread_mutex_unlock(&capture_mutex);

	if (weak) {
		obs_source_t *src = obs_weak_source_get_source(weak);
		if (src) {
			obs_source_remove_audio_capture_callback(src, audio_capture_callback, NULL);
			obs_source_release(src);
		}
		obs_weak_source_release(weak);
	}
}

bool replay_audio_capture_window(uint64_t start_ts, uint64_t end_ts, struct obs_audio_data **packets_out,
				 uint64_t *count_out, struct audio_convert_info *oai_out)
{
	pthread_mutex_lock(&capture_mutex);
	if (!g_configured) {
		if (packets_out)
			*packets_out = NULL;
		if (count_out)
			*count_out = 0;
		pthread_mutex_unlock(&capture_mutex);
		return false;
	}

	if (oai_out)
		*oai_out = g_oai;

	if (g_count == 0) {
		if (packets_out)
			*packets_out = NULL;
		if (count_out)
			*count_out = 0;
		pthread_mutex_unlock(&capture_mutex);
		return true;
	}

	uint32_t rate = g_oai.samples_per_sec ? g_oai.samples_per_sec : 48000;
	size_t matching = 0;
	for (size_t i = 0; i < g_count; i++) {
		struct obs_audio_data *pkt = &g_packets[(g_head + i) % g_cap];
		uint64_t pkt_start = pkt->timestamp;
		uint64_t pkt_dur = packet_duration_ns(pkt->frames, rate);
		uint64_t pkt_end = pkt_start + pkt_dur;
		bool overlaps = (start_ts == end_ts) ? (pkt_start <= start_ts && pkt_end > start_ts)
						     : (pkt_start < end_ts && pkt_end > start_ts);
		if (overlaps)
			matching++;
	}

	if (matching == 0) {
		if (packets_out)
			*packets_out = NULL;
		if (count_out)
			*count_out = 0;
		pthread_mutex_unlock(&capture_mutex);
		return true;
	}

	struct obs_audio_data *out = bzalloc(matching * sizeof(struct obs_audio_data));
	size_t idx = 0;
	for (size_t i = 0; i < g_count && idx < matching; i++) {
		struct obs_audio_data *pkt = &g_packets[(g_head + i) % g_cap];
		uint64_t pkt_start = pkt->timestamp;
		uint64_t pkt_dur = packet_duration_ns(pkt->frames, rate);
		uint64_t pkt_end = pkt_start + pkt_dur;
		bool overlaps = (start_ts == end_ts) ? (pkt_start <= start_ts && pkt_end > start_ts)
						     : (pkt_start < end_ts && pkt_end > start_ts);
		if (overlaps) {
			out[idx] = *pkt;
			replay_audio_packet_addref(&out[idx]);
			idx++;
		}
	}

	if (count_out)
		*count_out = (uint64_t)matching;
	if (packets_out)
		*packets_out = out;

	pthread_mutex_unlock(&capture_mutex);
	return true;
}

bool replay_audio_capture_coverage(uint64_t *oldest_ts, uint64_t *newest_end_ts, bool *gap_out)
{
	pthread_mutex_lock(&capture_mutex);
	if (!g_configured || g_count == 0) {
		if (oldest_ts)
			*oldest_ts = 0;
		if (newest_end_ts)
			*newest_end_ts = 0;
		if (gap_out)
			*gap_out = false;
		pthread_mutex_unlock(&capture_mutex);
		return false;
	}

	uint32_t rate = g_oai.samples_per_sec ? g_oai.samples_per_sec : 48000;
	struct obs_audio_data *first = &g_packets[g_head];
	size_t last_idx = (g_head + g_count - 1) % g_cap;
	struct obs_audio_data *last = &g_packets[last_idx];
	uint64_t last_dur = packet_duration_ns(last->frames, rate);

	if (oldest_ts)
		*oldest_ts = first->timestamp;
	if (newest_end_ts)
		*newest_end_ts = last->timestamp + last_dur;

	bool has_gap = false;
	for (size_t i = 0; i + 1 < g_count; i++) {
		struct obs_audio_data *p1 = &g_packets[(g_head + i) % g_cap];
		struct obs_audio_data *p2 = &g_packets[(g_head + i + 1) % g_cap];
		uint64_t p1_dur = packet_duration_ns(p1->frames, rate);
		uint64_t p1_end = p1->timestamp + p1_dur;
		if (p2->timestamp > p1_end + 2000000ULL) {
			has_gap = true;
			break;
		}
	}

	if (gap_out)
		*gap_out = has_gap;

	pthread_mutex_unlock(&capture_mutex);
	return true;
}

const char *replay_audio_capture_source_name(void)
{
	pthread_mutex_lock(&capture_mutex);
	const char *name = (g_configured && g_source_name[0] != '\0') ? g_source_name : "";
	pthread_mutex_unlock(&capture_mutex);
	return name;
}
