#include "version.h"
#include <math.h>
#include <util/platform.h>
#include "replay.h"
#include "replay-pool.h"

void free_audio_data(struct replay_filter *filter)
{
	while (filter->audio_frames.size) {
		struct obs_audio_data audio;

		circlebuf_pop_front(&filter->audio_frames, &audio, sizeof(struct obs_audio_data));
		free_audio_packet(&audio);
	}
}

void free_video_data(struct replay_filter *filter)
{
	while (filter->video_frames.size) {
		struct obs_source_frame *frame;

		circlebuf_pop_front(&filter->video_frames, &frame, sizeof(struct obs_source_frame *));

		replay_frame_release(frame);
	}
}

void replay_frame_addref(struct obs_source_frame *frame)
{
	if (frame)
		os_atomic_inc_long(&frame->refs);
}

void replay_frame_release(struct obs_source_frame *frame)
{
	if (frame && os_atomic_dec_long(&frame->refs) <= 0)
		replay_pool_release_frame(frame);
}
static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ? (ts2 - ts1) : (ts1 - ts2);
}

EXPORT uint64_t os_gettime_ns(void);

struct obs_audio_data *replay_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct replay_filter *filter = data;
	struct obs_audio_data cached = *audio;
	bool threshold_trigger = !filter->trigger_threshold;
	if (filter->oai.samples_per_sec == 0 || filter->oai.format != AUDIO_FORMAT_FLOAT_PLANAR) {
		struct obs_audio_info oai;
		obs_get_audio_info(&oai);
		filter->oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
		filter->oai.speakers = oai.speakers;
		filter->oai.samples_per_sec = oai.samples_per_sec;
	}

	replay_audio_packet_copy(&cached, audio);

	for (size_t i = 0; !threshold_trigger && i < MAX_AV_PLANES; i++) {
		if (!audio->data[i])
			break;

		for (size_t j = 0; !threshold_trigger && j < audio->frames; j++) {
			if (fabsf(((float *)audio->data[i])[j]) > filter->threshold)
				threshold_trigger = true;
		}
	}
	if (filter->trigger_threshold && threshold_trigger) {
		filter->trigger_threshold(filter->threshold_data);
	}
	const uint64_t timestamp = cached.timestamp;
	uint64_t adjusted_time = timestamp + filter->timing_adjust;
	const uint64_t os_time = os_gettime_ns();
	if (filter->timing_adjust && uint64_diff(os_time, timestamp) < MAX_TS_VAR) {
		adjusted_time = timestamp;
		filter->timing_adjust = 0;
	} else if (uint64_diff(os_time, adjusted_time) > MAX_TS_VAR) {
		filter->timing_adjust = os_time - timestamp;
		adjusted_time = os_time;
	}
	cached.timestamp = adjusted_time;

	pthread_mutex_lock(&filter->mutex);

	circlebuf_push_back(&filter->audio_frames, &cached, sizeof(cached));
	filter->last_arrival_ns = os_time;

	circlebuf_peek_front(&filter->audio_frames, &cached, sizeof(cached));

	uint64_t cur_duration = adjusted_time - cached.timestamp;
	while (filter->audio_frames.size > sizeof(cached) && cur_duration >= filter->duration + MAX_TS_VAR) {

		circlebuf_pop_front(&filter->audio_frames, NULL, sizeof(cached));

		free_audio_packet(&cached);
		circlebuf_peek_front(&filter->audio_frames, &cached, sizeof(cached));
		cur_duration = adjusted_time - cached.timestamp;
	}
	pthread_mutex_unlock(&filter->mutex);
	return audio;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("replay-source", "en-US")

/* One allocation per packet: [refcount header][plane 0][plane 1]...; the header size keeps every plane
 * at the 32-byte alignment bmalloc guarantees. */
#define REPLAY_AUDIO_HEADER_SIZE 64

struct replay_audio_block {
	volatile long refs;
};

static inline struct replay_audio_block *replay_audio_block_of(const struct obs_audio_data *audio)
{
	return (struct replay_audio_block *)(audio->data[0] - REPLAY_AUDIO_HEADER_SIZE);
}

void replay_audio_packet_copy(struct obs_audio_data *dst, const struct obs_audio_data *src)
{
	*dst = *src;
	size_t planes = 0;
	while (planes < MAX_AV_PLANES && src->data[planes])
		planes++;
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		dst->data[i] = NULL;
	if (!planes)
		return;

	const size_t plane_bytes = (size_t)src->frames * sizeof(float);
	const size_t stride = (plane_bytes + 31) & ~(size_t)31;
	uint8_t *mem = bmalloc(REPLAY_AUDIO_HEADER_SIZE + stride * planes);
	((struct replay_audio_block *)mem)->refs = 1;
	for (size_t i = 0; i < planes; i++) {
		dst->data[i] = mem + REPLAY_AUDIO_HEADER_SIZE + stride * i;
		memcpy(dst->data[i], src->data[i], plane_bytes);
	}
}

void replay_audio_packet_addref(struct obs_audio_data *audio)
{
	if (audio && audio->data[0])
		os_atomic_inc_long(&replay_audio_block_of(audio)->refs);
}

void free_audio_packet(struct obs_audio_data *audio)
{
	if (audio->data[0]) {
		struct replay_audio_block *block = replay_audio_block_of(audio);
		if (os_atomic_dec_long(&block->refs) == 0)
			bfree(block);
	}
	memset(audio, 0, sizeof(*audio));
}

const char *obs_module_name(void)
{
	return obs_module_text("ReplaySource");
}

const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

const char *obs_module_author(void)
{
	return "Exeldro";
}

void replay_filter_check(void *data)
{
	struct replay_filter *filter = data;
	if (!obs_initialized())
		return;
	if (filter->last_check && filter->last_check + 3 * SEC_TO_NSEC > obs_get_video_frame_time())
		return;
	filter->last_check = obs_get_video_frame_time();
	obs_source_t *s = obs_get_source_by_name(obs_source_get_name(filter->src));
	if (s) {
		if (!filter->trigger_threshold) {
			obs_data_t *settings = obs_source_get_settings(s);
			if (obs_data_get_bool(settings, SETTING_SOUND_TRIGGER)) {
				filter->threshold_data = obs_obj_get_data(s);
				filter->trigger_threshold = replay_trigger_threshold;
			}
			obs_data_release(settings);
		}
		obs_source_release(s);
	} else {
		obs_source_filter_remove(obs_filter_get_parent(filter->src), filter->src);
	}
}
