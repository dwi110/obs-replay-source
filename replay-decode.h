#pragma once

#include <obs.h>
#include <media-io/audio-io.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct replay_decoded_clip {
	struct obs_source_frame **video_frames; /* NV12 (I422/UYVY sources stay 4:2:2 as UYVY? -> always NV12 is fine), pool frames */
	uint64_t video_frame_count;
	struct obs_audio_data *audio_frames;   /* planar float, refcounted packets */
	uint64_t audio_frame_count;
	struct audio_convert_info oai;          /* samples_per_sec, speakers, AUDIO_FORMAT_FLOAT_PLANAR */
	uint64_t first_frame_timestamp;         /* ns; video and audio share one base */
	uint64_t last_frame_timestamp;
	uint64_t duration;                      /* last - first */
};

/* Blocking; call from a worker thread. should_abort may be NULL. Returns false with a short reason in err. */
bool replay_decode_file(const char *path, uint64_t base_ts, struct replay_decoded_clip *out,
			bool (*should_abort)(void *param), void *param, char *err, size_t err_size);

void replay_decoded_clip_free(struct replay_decoded_clip *clip); /* releases every frame/packet, zeroes */

bool replay_decode_supported(void); /* compiled with REPLAY_FAST_SAVE and FFmpeg DLLs loadable (reuse replay_save_fast_available() from replay-save-fast.h if linking it is cheap, otherwise same os_dlopen check) */

#ifdef __cplusplus
}
#endif
