#include "replay-save-queue.h"
#include "replay-control.h"
#include <inttypes.h>
#include <string.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>
#include <media-io/audio-io.h>

struct replay_save_job {
	struct replay_save_job *next;

	char *source_name;
	char *output_path;
	char *save_encoder;
	char *save_container;
	bool lossless;

	struct obs_source_frame **video_frames;
	uint64_t video_frame_count;

	struct obs_audio_data *audio_frames;
	uint64_t audio_frame_count;
	struct audio_convert_info oai;

	uint64_t first_frame_timestamp;
	uint64_t last_frame_timestamp;
	uint64_t duration_ns;
	size_t pinned_bytes;
};

struct replay_save_queue_state {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_cond_t idle_cond;
	pthread_t worker_thread;
	bool initialized;
	bool worker_running;
	volatile bool shutting_down;

	struct replay_save_job *head;
	struct replay_save_job *tail;
	struct replay_save_job *active_job;
	size_t pending_count;
	size_t pinned_bytes;
	uint64_t completed_count;
	uint64_t enqueued_count;

	volatile long active_encoders;
	volatile long max_concurrent_encoders;

	obs_hotkey_id save_all_hotkey;
};

static struct replay_save_queue_state g_queue = {
	.save_all_hotkey = OBS_INVALID_HOTKEY_ID,
};

static inline bool is_shutting_down(void)
{
	return os_atomic_load_bool(&g_queue.shutting_down);
}

static size_t estimate_frame_bytes(const struct obs_source_frame *frame)
{
	if (!frame || !frame->height)
		return 0;

	const uint32_t h = frame->height;
	const uint32_t half_h = (h + 1) / 2;

	switch (frame->format) {
	case VIDEO_FORMAT_NONE:
		return 0;
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I010:
		return (size_t)frame->linesize[0] * h + (size_t)frame->linesize[1] * half_h +
		       (size_t)frame->linesize[2] * half_h;
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_P010:
		return (size_t)frame->linesize[0] * h + (size_t)frame->linesize[1] * half_h;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I412:
		return ((size_t)frame->linesize[0] + (size_t)frame->linesize[1] + (size_t)frame->linesize[2]) * h;
	case VIDEO_FORMAT_I40A:
		return (size_t)frame->linesize[0] * h + (size_t)frame->linesize[1] * half_h +
		       (size_t)frame->linesize[2] * half_h + (size_t)frame->linesize[3] * h;
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
	case VIDEO_FORMAT_YA2L:
		return ((size_t)frame->linesize[0] + (size_t)frame->linesize[1] + (size_t)frame->linesize[2] +
			(size_t)frame->linesize[3]) *
		       h;
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		return ((size_t)frame->linesize[0] + (size_t)frame->linesize[1]) * h;
	default:
		return (size_t)frame->linesize[0] * h;
	}
}

static void free_save_job(struct replay_save_job *job)
{
	if (!job)
		return;

	if (job->video_frames) {
		for (uint64_t i = 0; i < job->video_frame_count; i++) {
			if (job->video_frames[i]) {
				replay_frame_release(job->video_frames[i]);
				job->video_frames[i] = NULL;
			}
		}
		bfree(job->video_frames);
	}

	if (job->audio_frames) {
		for (uint64_t i = 0; i < job->audio_frame_count; i++) {
			free_audio_packet(&job->audio_frames[i]);
		}
		bfree(job->audio_frames);
	}

	bfree(job->source_name);
	bfree(job->output_path);
	bfree(job->save_encoder);
	bfree(job->save_container);
	bfree(job);
}

static bool encoder_type_registered(const char *id)
{
	if (!id || !*id)
		return false;

	const char *type_id = NULL;
	size_t idx = 0;
	while (obs_enum_encoder_types(idx++, &type_id)) {
		if (type_id && strcmp(type_id, id) == 0)
			return true;
	}
	return false;
}

static bool encoder_accepts_raw_frames(const char *id)
{
	if (!encoder_type_registered(id))
		return false;
	if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
		return false;
	const uint32_t caps = obs_get_encoder_caps(id);
	if ((caps & OBS_ENCODER_CAP_PASS_TEXTURE) != 0)
		return false;
	return true;
}

const char *replay_save_select_encoder(const char *save_encoder)
{
	static const char *const auto_candidates[] = {
		"obs_nvenc_hevc_soft",
		"obs_nvenc_h264_soft",
		"obs_nvenc_hevc_cuda",
		"obs_nvenc_h264_cuda",
		"jim_hevc_nvenc",
		"jim_nvenc",
		"ffmpeg_hevc_nvenc",
		"ffmpeg_nvenc",
		"obs_x264",
		NULL,
	};
	static const char *const hevc_candidates[] = {
		"obs_nvenc_hevc_soft",
		"obs_nvenc_hevc_cuda",
		"jim_hevc_nvenc",
		"ffmpeg_hevc_nvenc",
		NULL,
	};
	static const char *const h264_candidates[] = {
		"obs_nvenc_h264_soft",
		"obs_nvenc_h264_cuda",
		"jim_nvenc",
		"ffmpeg_nvenc",
		NULL,
	};

	if (save_encoder && astrcmpi(save_encoder, SAVE_ENCODER_X264) == 0) {
		if (encoder_accepts_raw_frames("obs_x264"))
			return "obs_x264";
	} else if (save_encoder && astrcmpi(save_encoder, SAVE_ENCODER_HEVC_NVENC) == 0) {
		for (size_t i = 0; hevc_candidates[i]; i++) {
			if (encoder_accepts_raw_frames(hevc_candidates[i]))
				return hevc_candidates[i];
		}
		blog(LOG_WARNING, "[replay_save_queue] Requested encoder '%s' unavailable, falling back to auto",
		     save_encoder);
	} else if (save_encoder && astrcmpi(save_encoder, SAVE_ENCODER_H264_NVENC) == 0) {
		for (size_t i = 0; h264_candidates[i]; i++) {
			if (encoder_accepts_raw_frames(h264_candidates[i]))
				return h264_candidates[i];
		}
		blog(LOG_WARNING, "[replay_save_queue] Requested encoder '%s' unavailable, falling back to auto",
		     save_encoder);
	}

	for (size_t i = 0; auto_candidates[i]; i++) {
		if (encoder_accepts_raw_frames(auto_candidates[i]))
			return auto_candidates[i];
	}

	return "obs_x264";
}

static void sanitize_filename_component(const char *in, struct dstr *out)
{
	dstr_init(out);
	if (!in || !*in) {
		dstr_copy(out, "Replay");
		return;
	}

	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		unsigned char c = *p;
		if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
		    c == '?' || c == '*') {
			dstr_cat_ch(out, '_');
		} else {
			dstr_cat_ch(out, (char)c);
		}
	}

	dstr_depad(out);
	while (out->len > 0 && out->array[out->len - 1] == '.') {
		dstr_resize(out, out->len - 1);
	}

	if (dstr_is_empty(out))
		dstr_copy(out, "Replay");
}

static bool path_in_use_locked(const char *path)
{
	if (os_file_exists(path))
		return true;

	if (g_queue.active_job && g_queue.active_job->output_path &&
	    strcmp(g_queue.active_job->output_path, path) == 0)
		return true;

	for (struct replay_save_job *it = g_queue.head; it; it = it->next) {
		if (it->output_path && strcmp(it->output_path, path) == 0)
			return true;
	}
	return false;
}

static char *build_unique_output_path_locked(const char *directory, const char *file_format, const char *source_name,
					     const char *ext)
{
	const char *fmt = (file_format && *file_format) ? file_format : "%CCYY-%MM-%DD %hh.%mm.%ss";
	const char *dir = (directory && *directory) ? directory : ".";

	struct dstr clean_source;
	sanitize_filename_component(source_name, &clean_source);

	char *generated = os_generate_formatted_filename(NULL, true, fmt);
	struct dstr base_name;
	dstr_init(&base_name);
	if (generated && *generated) {
		dstr_copy(&base_name, generated);
	} else {
		dstr_copy(&base_name, "Replay");
	}
	bfree(generated);

	if (strstr(base_name.array, clean_source.array) == NULL) {
		dstr_cat(&base_name, " - ");
		dstr_cat_dstr(&base_name, &clean_source);
	}
	dstr_free(&clean_source);

	struct dstr full_dir;
	dstr_init_copy(&full_dir, dir);
	dstr_replace(&full_dir, "\\", "/");
	if (!dstr_is_empty(&full_dir) && dstr_end(&full_dir) != '/')
		dstr_cat_ch(&full_dir, '/');

	struct dstr candidate;
	dstr_init_copy_dstr(&candidate, &full_dir);
	dstr_cat_dstr(&candidate, &base_name);
	dstr_cat_ch(&candidate, '.');
	dstr_cat(&candidate, ext);

	int suffix = 2;
	while (path_in_use_locked(candidate.array)) {
		dstr_copy_dstr(&candidate, &full_dir);
		dstr_cat_dstr(&candidate, &base_name);
		dstr_catf(&candidate, " (%d).%s", suffix++, ext);
	}

	dstr_free(&full_dir);
	dstr_free(&base_name);
	return candidate.array;
}

struct save_audio_cb_ctx {
	const struct replay_save_job *job;
	audio_t *audio_output;
	volatile uint64_t start_save_timestamp;
	volatile bool audio_connected;
	volatile bool start_ready;
};

static inline size_t ns_to_frames(size_t sample_rate, uint64_t t_ns)
{
	return (size_t)(t_ns * (uint64_t)sample_rate / 1000000000ULL);
}

static bool save_audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts,
				      uint32_t mixers, struct audio_output_data *mixes)
{
	struct save_audio_cb_ctx *ctx = param;
	*out_ts = start_ts_in;

	if (mixers != 0)
		os_atomic_set_bool(&ctx->audio_connected, true);

	if (!ctx || !ctx->audio_output || !os_atomic_load_bool(&ctx->start_ready))
		return true;

	const struct replay_save_job *job = ctx->job;
	if (!job || !job->audio_frames || job->audio_frame_count == 0)
		return true;

	const uint64_t start_save_ts = ctx->start_save_timestamp;
	if (!start_save_ts || start_ts_in < start_save_ts)
		return true;

	const uint64_t end_timestamp = start_save_ts + job->duration_ns;
	if (start_ts_in > end_timestamp)
		return true;

	const size_t channels = audio_output_get_channels(ctx->audio_output);
	const size_t sample_rate = audio_output_get_sample_rate(ctx->audio_output);
	if (!channels || !sample_rate)
		return true;

	const uint64_t duration_start = start_ts_in - start_save_ts;
	const uint64_t duration_end = end_ts_in - start_save_ts;

	uint64_t i = 0;
	while (i < job->audio_frame_count && job->audio_frames[i].timestamp < job->first_frame_timestamp) {
		i++;
	}
	if (i == job->audio_frame_count)
		return true;

	while (i < job->audio_frame_count &&
	       (job->audio_frames[i].timestamp - job->first_frame_timestamp) < duration_start) {
		i++;
	}
	if (i > 0 && job->audio_frames[i - 1].timestamp >= job->first_frame_timestamp)
		i--;
	if (i >= job->audio_frame_count)
		return true;

	while (i < job->audio_frame_count) {
		const struct obs_audio_data *pkt = &job->audio_frames[i];
		if (pkt->timestamp < job->first_frame_timestamp) {
			i++;
			continue;
		}
		const uint64_t pkt_rel_ts = pkt->timestamp - job->first_frame_timestamp;
		if (pkt_rel_ts > duration_end)
			break;

		size_t total_floats = AUDIO_OUTPUT_FRAMES;
		size_t mix_offset = 0;
		size_t pkt_offset = 0;

		if (pkt_rel_ts > duration_start) {
			mix_offset = ns_to_frames(sample_rate, pkt_rel_ts - duration_start);
			if (mix_offset >= AUDIO_OUTPUT_FRAMES)
				break;
			total_floats -= mix_offset;
		} else if (pkt_rel_ts < duration_start) {
			pkt_offset = ns_to_frames(sample_rate, duration_start - pkt_rel_ts);
			if (pkt_offset >= pkt->frames) {
				i++;
				continue;
			}
		}

		if (pkt->frames - pkt_offset < total_floats)
			total_floats = pkt->frames - pkt_offset;

		for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
			for (size_t ch = 0; ch < channels; ch++) {
				float *mix = mixes[mix_idx].data[ch];
				const float *aud = (const float *)pkt->data[ch];
				if (!mix || !aud)
					continue;

				aud += pkt_offset;
				mix += mix_offset;
				const float *end = aud + total_floats;
				while (aud < end)
					*(mix++) += *(aud++);
			}
		}
		i++;
	}

	return true;
}

static void sleep_until_interruptible(uint64_t target_ns)
{
	for (;;) {
		if (is_shutting_down())
			return;
		const uint64_t now = os_gettime_ns();
		if (now >= target_ns)
			return;
		const uint64_t diff_ns = target_ns - now;
		if (diff_ns > 15000000ULL) {
			os_sleep_ms(10);
		} else {
			os_sleepto_ns_fast(target_ns);
			return;
		}
	}
}

static bool write_frame_to_output(video_t *video_output, const struct video_output_info *vi, video_scaler_t **scaler,
				  enum video_format *scaler_src_fmt, uint32_t *scaler_src_w, uint32_t *scaler_src_h,
				  const struct obs_source_frame *frame, uint64_t timestamp)
{
	if (!video_output || !frame)
		return false;

	if (frame->format != vi->format || frame->width != vi->width || frame->height != vi->height) {
		if (!*scaler || *scaler_src_fmt != frame->format || *scaler_src_w != frame->width ||
		    *scaler_src_h != frame->height) {
			if (*scaler) {
				video_scaler_destroy(*scaler);
				*scaler = NULL;
			}
			struct video_scale_info ssi = {
				.format = frame->format,
				.width = frame->width,
				.height = frame->height,
				.colorspace = vi->colorspace,
				.range = frame->full_range ? VIDEO_RANGE_FULL : vi->range,
			};
			struct video_scale_info dsi = {
				.format = vi->format,
				.width = vi->width,
				.height = vi->height,
				.colorspace = vi->colorspace,
				.range = vi->range,
			};
			if (video_scaler_create(scaler, &dsi, &ssi, VIDEO_SCALE_DEFAULT) != VIDEO_SCALER_SUCCESS) {
				blog(LOG_ERROR, "[replay_save_queue] Failed to create video scaler (%d -> %d)",
				     (int)frame->format, (int)vi->format);
				return false;
			}
			*scaler_src_fmt = frame->format;
			*scaler_src_w = frame->width;
			*scaler_src_h = frame->height;
		}
	}

	struct video_frame output_frame;
	if (!video_output_lock_frame(video_output, &output_frame, 1, timestamp))
		return false;

	if (frame->format == vi->format && frame->width == vi->width && frame->height == vi->height) {
		struct video_frame src_vf = {0};
		for (size_t p = 0; p < MAX_AV_PLANES; p++) {
			src_vf.data[p] = frame->data[p];
			src_vf.linesize[p] = frame->linesize[p];
		}
		video_frame_copy(&output_frame, &src_vf, vi->format, vi->height);
	} else {
		video_scaler_scale(*scaler, output_frame.data, output_frame.linesize,
				   (const uint8_t *const *)frame->data, frame->linesize);
	}

	video_output_unlock_frame(video_output);
	return true;
}

static void configure_video_encoder(obs_encoder_t *venc, const char *enc_id)
{
	obs_data_t *settings = obs_data_create();
	if (strcmp(enc_id, "obs_x264") == 0) {
		obs_data_set_int(settings, "crf", 18);
		obs_data_set_bool(settings, "use_bufsize", true);
		obs_data_set_string(settings, "rate_control", "CRF");
		obs_data_set_string(settings, "profile", "high");
		obs_data_set_string(settings, "preset", "veryfast");
	} else {
		const bool is_hevc = strstr(enc_id, "hevc") != NULL;
		obs_data_set_string(settings, "rate_control", "CQP");
		obs_data_set_int(settings, "cqp", 18);
		obs_data_set_string(settings, "preset", "p5");
		obs_data_set_string(settings, "preset2", "p5");
		obs_data_set_string(settings, "tune", "hq");
		obs_data_set_string(settings, "multipass", "qres");
		obs_data_set_string(settings, "profile", is_hevc ? "main" : "high");
		obs_data_set_bool(settings, "psycho_aq", true);
		obs_data_set_bool(settings, "adaptive_quantization", true);
		obs_data_set_int(settings, "bf", 2);
	}
	obs_encoder_update(venc, settings);
	obs_data_release(settings);
}

static void ensure_parent_dir(const char *file_path)
{
	if (!file_path || !*file_path)
		return;
	struct dstr dir;
	dstr_init_copy(&dir, file_path);
	dstr_replace(&dir, "\\", "/");
	char *slash = strrchr(dir.array, '/');
	if (slash && slash != dir.array) {
		*slash = '\0';
		os_mkdirs(dir.array);
	}
	dstr_free(&dir);
}

static bool run_save_job(struct replay_save_job *job, bool *aborted_out)
{
	*aborted_out = false;
	if (!job || job->video_frame_count == 0 || !job->video_frames || !job->video_frames[0])
		return false;

	ensure_parent_dir(job->output_path);

	const uint32_t width = job->video_frames[0]->width;
	const uint32_t height = job->video_frames[0]->height;
	if (!width || !height)
		return false;

	video_t *base_video = obs_get_video();
	const struct video_output_info *ovi = base_video ? video_output_get_info(base_video) : NULL;

	struct video_output_info vi = {0};
	vi.name = "ReplaySaveVideo";
	vi.format = ovi ? ovi->format : VIDEO_FORMAT_NV12;
	vi.width = width;
	vi.height = height;
	vi.fps_num = (ovi && ovi->fps_num) ? ovi->fps_num : 60;
	vi.fps_den = (ovi && ovi->fps_den) ? ovi->fps_den : 1;
	vi.cache_size = 16;
	vi.colorspace = ovi ? ovi->colorspace : VIDEO_CS_709;
	vi.range = ovi ? ovi->range : VIDEO_RANGE_PARTIAL;

	video_t *video_output = NULL;
	if (video_output_open(&video_output, &vi) != VIDEO_OUTPUT_SUCCESS || !video_output) {
		blog(LOG_ERROR, "[replay_save_queue] Failed to open video_output (%ux%u)", width, height);
		return false;
	}

	struct obs_audio_info base_ai = {0};
	obs_get_audio_info(&base_ai);

	struct save_audio_cb_ctx audio_ctx = {
		.job = job,
		.audio_output = NULL,
		.start_save_timestamp = 0,
		.audio_connected = false,
		.start_ready = false,
	};

	struct audio_output_info oi = {0};
	oi.name = "ReplaySaveAudio";
	oi.samples_per_sec = job->oai.samples_per_sec
				     ? job->oai.samples_per_sec
				     : (base_ai.samples_per_sec ? base_ai.samples_per_sec : 48000);
	oi.speakers = (job->oai.speakers != SPEAKERS_UNKNOWN)
			      ? job->oai.speakers
			      : (base_ai.speakers != SPEAKERS_UNKNOWN ? base_ai.speakers : SPEAKERS_STEREO);
	oi.format = (job->oai.format != AUDIO_FORMAT_UNKNOWN) ? job->oai.format : AUDIO_FORMAT_FLOAT_PLANAR;
	oi.input_param = &audio_ctx;
	oi.input_callback = save_audio_input_callback;

	audio_t *audio_output = NULL;
	if (audio_output_open(&audio_output, &oi) != AUDIO_OUTPUT_SUCCESS || !audio_output) {
		blog(LOG_ERROR, "[replay_save_queue] Failed to open audio_output");
		video_output_close(video_output);
		return false;
	}
	audio_ctx.audio_output = audio_output;

	obs_output_t *file_output = NULL;
	obs_encoder_t *video_encoder = NULL;
	obs_encoder_t *audio_encoder = NULL;
	video_scaler_t *scaler = NULL;
	enum video_format scaler_src_fmt = VIDEO_FORMAT_NONE;
	uint32_t scaler_src_w = 0;
	uint32_t scaler_src_h = 0;
	bool success = false;

	const long cur_enc = os_atomic_inc_long(&g_queue.active_encoders);
	long prev_max = os_atomic_load_long(&g_queue.max_concurrent_encoders);
	while (cur_enc > prev_max) {
		os_atomic_compare_exchange_long(&g_queue.max_concurrent_encoders, &prev_max, cur_enc);
	}

	if (job->lossless) {
		blog(LOG_INFO, "[replay_save_queue] Using lossless utvideo/pcm_s16le AVI output for '%s'",
		     job->output_path);
		file_output = obs_output_create("ffmpeg_output", "replay_save_ffmpeg_output", NULL, NULL);
		if (!file_output) {
			blog(LOG_ERROR, "[replay_save_queue] Failed to create ffmpeg_output");
			goto cleanup;
		}
		obs_data_t *fo_settings = obs_data_create();
		obs_data_set_string(fo_settings, "format_name", "avi");
		obs_data_set_string(fo_settings, "video_encoder", "utvideo");
		obs_data_set_string(fo_settings, "audio_encoder", "pcm_s16le");
		obs_data_set_string(fo_settings, "path", job->output_path);
		obs_data_set_string(fo_settings, "url", job->output_path);
		obs_output_update(file_output, fo_settings);
		obs_data_release(fo_settings);
		obs_output_set_media(file_output, video_output, audio_output);
		obs_output_set_mixer(file_output, 0);
	} else {
		const char *enc_id = replay_save_select_encoder(job->save_encoder);
		blog(LOG_INFO, "[replay_save_queue] Selected video encoder '%s' (setting '%s') for '%s'", enc_id,
		     job->save_encoder ? job->save_encoder : SAVE_ENCODER_AUTO, job->output_path);

		file_output = obs_output_create("ffmpeg_muxer", "replay_save_ffmpeg_muxer", NULL, NULL);
		if (!file_output) {
			blog(LOG_ERROR, "[replay_save_queue] Failed to create ffmpeg_muxer");
			goto cleanup;
		}

		video_encoder = obs_video_encoder_create(enc_id, "replay_save_video_encoder", NULL, NULL);
		if (!video_encoder && strcmp(enc_id, "obs_x264") != 0) {
			blog(LOG_WARNING,
			     "[replay_save_queue] Failed to create encoder '%s', falling back to 'obs_x264'", enc_id);
			enc_id = "obs_x264";
			video_encoder = obs_video_encoder_create(enc_id, "replay_save_video_encoder", NULL, NULL);
		}
		if (!video_encoder) {
			blog(LOG_ERROR, "[replay_save_queue] Failed to create video encoder '%s'", enc_id);
			goto cleanup;
		}
		configure_video_encoder(video_encoder, enc_id);
		obs_encoder_set_video(video_encoder, video_output);
		obs_output_set_video_encoder(file_output, video_encoder);

		audio_encoder = obs_audio_encoder_create("ffmpeg_aac", "replay_save_aac_encoder", NULL, 0, NULL);
		if (!audio_encoder) {
			blog(LOG_ERROR, "[replay_save_queue] Failed to create ffmpeg_aac audio encoder");
			goto cleanup;
		}
		obs_encoder_set_audio(audio_encoder, audio_output);
		obs_output_set_audio_encoder(file_output, audio_encoder, 0);

		obs_data_t *fo_settings = obs_data_create();
		obs_data_set_string(fo_settings, "path", job->output_path);
		obs_data_set_string(fo_settings, "url", job->output_path);
		obs_output_update(file_output, fo_settings);
		obs_data_release(fo_settings);
	}

	if (!obs_output_start(file_output)) {
		const char *err = obs_output_get_last_error(file_output);
		blog(LOG_ERROR, "[replay_save_queue] obs_output_start failed for '%s': %s", job->output_path,
		     err ? err : "unknown error");
		goto cleanup;
	}

	/* Wait for output to become active and audio_thread to connect its first callback */
	const uint64_t wait_start_ns = os_gettime_ns();
	while (!is_shutting_down()) {
		if (obs_output_active(file_output) && os_atomic_load_bool(&audio_ctx.audio_connected))
			break;
		if (os_gettime_ns() - wait_start_ns > 2000000000ULL)
			break;
		os_sleep_ms(5);
	}
	if (is_shutting_down()) {
		*aborted_out = true;
		obs_output_force_stop(file_output);
		goto cleanup;
	}
	if (!obs_output_active(file_output)) {
		const char *err = obs_output_get_last_error(file_output);
		blog(LOG_ERROR, "[replay_save_queue] Output did not become active for '%s': %s", job->output_path,
		     err ? err : "unknown error");
		goto cleanup;
	}

	/* Allow audio_thread to finish delivering its initial packet to receive_audio */
	os_sleep_ms(5);

	const uint64_t frame_interval_ns = util_mul_div64(1000000000ULL, vi.fps_den, vi.fps_num);
	const uint64_t base_ts = os_gettime_ns();
	audio_ctx.start_save_timestamp = base_ts;
	os_atomic_set_bool(&audio_ctx.start_ready, true);

	uint64_t prev_target_ts = base_ts;
	uint64_t locked_frames = 0;

	for (uint64_t idx = 0; idx < job->video_frame_count; idx++) {
		if (is_shutting_down()) {
			*aborted_out = true;
			obs_output_force_stop(file_output);
			goto cleanup;
		}
		if (!obs_output_active(file_output)) {
			const char *err = obs_output_get_last_error(file_output);
			blog(LOG_ERROR, "[replay_save_queue] Output stopped unexpectedly at frame %" PRIu64 ": %s", idx,
			     err ? err : "unknown error");
			goto cleanup;
		}

		const struct obs_source_frame *frame = job->video_frames[idx];
		uint64_t rel_ts = (frame->timestamp >= job->first_frame_timestamp)
					  ? (frame->timestamp - job->first_frame_timestamp)
					  : (idx * frame_interval_ns);
		uint64_t target_ts = base_ts + rel_ts;
		if (idx > 0 && target_ts <= prev_target_ts)
			target_ts = prev_target_ts + 1000;
		prev_target_ts = target_ts;

		sleep_until_interruptible(target_ts);
		if (is_shutting_down()) {
			*aborted_out = true;
			obs_output_force_stop(file_output);
			goto cleanup;
		}

		/* Ensure video_output cache has free slots so no frame is ever skipped */
		const uint64_t cache_wait_ns = os_gettime_ns();
		while (!is_shutting_down()) {
			uint32_t consumed = video_output_get_total_frames(video_output);
			if (locked_frames <= consumed || (locked_frames - consumed) < 12)
				break;
			if (os_gettime_ns() - cache_wait_ns > 1000000000ULL)
				break;
			os_sleep_ms(2);
		}

		if (!write_frame_to_output(video_output, &vi, &scaler, &scaler_src_fmt, &scaler_src_w, &scaler_src_h,
					   frame, target_ts)) {
			blog(LOG_WARNING, "[replay_save_queue] Failed to lock/write frame %" PRIu64, idx);
		} else {
			locked_frames++;
		}
	}

	/* Wait until video_thread has consumed all N clip frames and time reaches last_target_ts + 0.5 * frame_interval */
	const uint64_t stop_target_ts = prev_target_ts + (frame_interval_ns / 2);
	sleep_until_interruptible(stop_target_ts);

	const uint64_t drain_wait_ns = os_gettime_ns();
	while (!is_shutting_down() && video_output_get_total_frames(video_output) < (uint32_t)locked_frames) {
		if (os_gettime_ns() - drain_wait_ns > 2000000000ULL)
			break;
		os_sleep_ms(2);
	}

	if (is_shutting_down()) {
		*aborted_out = true;
		obs_output_force_stop(file_output);
		goto cleanup;
	}

	obs_output_stop(file_output);

	/* Feed tail frames at frame_interval pacing while encoder lookahead/B-frames and muxer flush up to stop_ts */
	uint64_t tail_ts = prev_target_ts + frame_interval_ns;
	const struct obs_source_frame *last_frame = job->video_frames[job->video_frame_count - 1];
	const uint64_t stop_wait_start_ns = os_gettime_ns();

	while (obs_output_active(file_output) && !is_shutting_down()) {
		if (os_gettime_ns() - stop_wait_start_ns > 5000000000ULL) {
			blog(LOG_WARNING, "[replay_save_queue] Timeout waiting for output stop on '%s', forcing stop",
			     job->output_path);
			obs_output_force_stop(file_output);
			break;
		}

		sleep_until_interruptible(tail_ts);
		if (!obs_output_active(file_output) || is_shutting_down())
			break;

		uint32_t consumed = video_output_get_total_frames(video_output);
		if (locked_frames <= consumed || (locked_frames - consumed) < 12) {
			if (write_frame_to_output(video_output, &vi, &scaler, &scaler_src_fmt, &scaler_src_w,
						  &scaler_src_h, last_frame, tail_ts)) {
				locked_frames++;
			}
		}
		tail_ts += frame_interval_ns;
	}

	if (is_shutting_down() && obs_output_active(file_output)) {
		*aborted_out = true;
		obs_output_force_stop(file_output);
		goto cleanup;
	}

	success = os_file_exists(job->output_path) && os_get_file_size(job->output_path) > 0;

cleanup:
	if (file_output)
		obs_output_release(file_output);
	if (video_encoder)
		obs_encoder_release(video_encoder);
	if (audio_encoder)
		obs_encoder_release(audio_encoder);
	if (video_output)
		video_output_close(video_output);
	if (audio_output)
		audio_output_close(audio_output);
	if (scaler)
		video_scaler_destroy(scaler);

	os_atomic_dec_long(&g_queue.active_encoders);

	if (*aborted_out && job->output_path && os_file_exists(job->output_path)) {
		os_unlink(job->output_path);
	}

	return success;
}

static void *save_queue_worker(void *param)
{
	UNUSED_PARAMETER(param);
	os_set_thread_name("replay-save-queue");

	pthread_mutex_lock(&g_queue.mutex);
	while (!is_shutting_down()) {
		if (!g_queue.head) {
			pthread_cond_broadcast(&g_queue.idle_cond);
			pthread_cond_wait(&g_queue.cond, &g_queue.mutex);
			continue;
		}

		struct replay_save_job *job = g_queue.head;
		g_queue.head = job->next;
		if (!g_queue.head)
			g_queue.tail = NULL;
		job->next = NULL;
		g_queue.pending_count--;
		g_queue.active_job = job;
		const size_t remaining_before = g_queue.pending_count;
		const size_t pinned_before = g_queue.pinned_bytes;
		pthread_mutex_unlock(&g_queue.mutex);

		const uint64_t start_ns = os_gettime_ns();
		blog(LOG_INFO,
		     "[replay_save_queue] Job start: source='%s' path='%s' frames=%" PRIu64
		     " duration=%.2fs queue_pending=%zu pinned=%zu bytes (%.2f MB)",
		     job->source_name, job->output_path, job->video_frame_count, (double)job->duration_ns / 1e9,
		     remaining_before, pinned_before, (double)pinned_before / (1024.0 * 1024.0));

		replay_control_emit_save_started(job->source_name, job->output_path);
		bool aborted = false;
		const bool ok = run_save_job(job, &aborted);
		const double elapsed_s = (double)(os_gettime_ns() - start_ns) / 1e9;

		pthread_mutex_lock(&g_queue.mutex);
		g_queue.active_job = NULL;
		if (g_queue.pinned_bytes >= job->pinned_bytes)
			g_queue.pinned_bytes -= job->pinned_bytes;
		else
			g_queue.pinned_bytes = 0;
		if (ok)
			g_queue.completed_count++;
		const size_t remaining_after = g_queue.pending_count;
		const size_t pinned_after = g_queue.pinned_bytes;
		pthread_mutex_unlock(&g_queue.mutex);

		blog(LOG_INFO,
		     "[replay_save_queue] Job end: status=%s source='%s' path='%s' frames=%" PRIu64
		     " clip_duration=%.2fs encode_time=%.2fs queue_pending=%zu pinned=%zu bytes (%.2f MB)",
		     ok ? "OK" : (aborted ? "ABORTED" : "FAILED"), job->source_name, job->output_path,
		     job->video_frame_count, (double)job->duration_ns / 1e9, elapsed_s, remaining_after, pinned_after,
		     (double)pinned_after / (1024.0 * 1024.0));


		replay_control_emit_save_finished(job->source_name, job->output_path, ok);
		free_save_job(job);

		pthread_mutex_lock(&g_queue.mutex);
		if (!g_queue.head && !g_queue.active_job)
			pthread_cond_broadcast(&g_queue.idle_cond);
	}

	pthread_cond_broadcast(&g_queue.idle_cond);
	pthread_mutex_unlock(&g_queue.mutex);
	return NULL;
}

static bool collect_replay_sources_cb(void *param, obs_source_t *source)
{
	DARRAY(obs_source_t *) *list = param;
	const char *id = obs_source_get_unversioned_id(source);
	if (id && strcmp(id, REPLAY_SOURCE_ID) == 0) {
		obs_source_t *ref = obs_source_get_ref(source);
		if (ref)
			da_push_back((*list), &ref);
	}
	return true;
}

size_t replay_save_all_loaded_replays(void)
{
	DARRAY(obs_source_t *) sources;
	da_init(sources);

	obs_enum_sources(collect_replay_sources_cb, &sources);

	const size_t before_pending = replay_save_queue_pending_count();
	uint64_t before_enqueued = 0;
	if (g_queue.initialized) {
		pthread_mutex_lock(&g_queue.mutex);
		before_enqueued = g_queue.enqueued_count;
		pthread_mutex_unlock(&g_queue.mutex);
	}
	for (size_t i = 0; i < sources.num; i++) {
		obs_source_t *s = sources.array[i];
		struct replay_source *ctx = obs_obj_get_data(s);
		if (ctx)
			replay_save(ctx);
		obs_source_release(s);
	}
	const size_t total_sources = sources.num;
	da_free(sources);

	uint64_t after_enqueued = before_enqueued;
	if (g_queue.initialized) {
		pthread_mutex_lock(&g_queue.mutex);
		after_enqueued = g_queue.enqueued_count;
		pthread_mutex_unlock(&g_queue.mutex);
	}
	const size_t queued = (size_t)(after_enqueued - before_enqueued);
	const size_t after_pending = replay_save_queue_pending_count();
	blog(LOG_INFO,
	     "[replay_save_queue] Save all loaded replays triggered across %zu replay sources (queued=%zu, queue length %zu -> %zu)",
	     total_sources, queued, before_pending, after_pending);
	return queued;
}

static void save_all_replays_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	replay_save_all_loaded_replays();
}

void replay_save_queue_init(void)
{
	if (g_queue.initialized)
		return;

	pthread_mutex_init(&g_queue.mutex, NULL);
	pthread_cond_init(&g_queue.cond, NULL);
	pthread_cond_init(&g_queue.idle_cond, NULL);
	g_queue.head = NULL;
	g_queue.tail = NULL;
	g_queue.active_job = NULL;
	g_queue.pending_count = 0;
	g_queue.pinned_bytes = 0;
	g_queue.completed_count = 0;
	g_queue.enqueued_count = 0;
	os_atomic_set_long(&g_queue.active_encoders, 0);
	os_atomic_set_long(&g_queue.max_concurrent_encoders, 0);
	os_atomic_set_bool(&g_queue.shutting_down, false);

	if (pthread_create(&g_queue.worker_thread, NULL, save_queue_worker, NULL) == 0) {
		g_queue.worker_running = true;
	} else {
		blog(LOG_ERROR, "[replay_save_queue] Failed to start worker thread");
		g_queue.worker_running = false;
	}

	const char *hotkey_desc = obs_module_text("SaveAllReplays");
	if (!hotkey_desc || !*hotkey_desc || strcmp(hotkey_desc, "SaveAllReplays") == 0)
		hotkey_desc = "Replay: save all loaded replays";
	g_queue.save_all_hotkey =
		obs_hotkey_register_frontend("ReplaySource.SaveAll", hotkey_desc, save_all_replays_hotkey, NULL);

	g_queue.initialized = true;
}

void replay_save_queue_shutdown(void)
{
	if (!g_queue.initialized)
		return;

	if (g_queue.save_all_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_queue.save_all_hotkey);
		g_queue.save_all_hotkey = OBS_INVALID_HOTKEY_ID;
	}

	pthread_mutex_lock(&g_queue.mutex);
	os_atomic_set_bool(&g_queue.shutting_down, true);
	pthread_cond_broadcast(&g_queue.cond);
	pthread_mutex_unlock(&g_queue.mutex);

	if (g_queue.worker_running) {
		pthread_join(g_queue.worker_thread, NULL);
		g_queue.worker_running = false;
	}

	pthread_mutex_lock(&g_queue.mutex);
	struct replay_save_job *cur = g_queue.head;
	g_queue.head = NULL;
	g_queue.tail = NULL;
	g_queue.pending_count = 0;
	g_queue.pinned_bytes = 0;
	pthread_mutex_unlock(&g_queue.mutex);

	while (cur) {
		struct replay_save_job *next = cur->next;
		free_save_job(cur);
		cur = next;
	}

	pthread_cond_destroy(&g_queue.idle_cond);
	pthread_cond_destroy(&g_queue.cond);
	pthread_mutex_destroy(&g_queue.mutex);
	g_queue.initialized = false;
}

bool replay_save_queue_enqueue(const struct replay_save_job_params *params)
{
	if (!params || !params->video_frames || params->video_frame_count == 0)
		return false;

	if (!g_queue.initialized)
		replay_save_queue_init();

	if (is_shutting_down())
		return false;

	uint64_t clip_start_ts = params->first_frame_timestamp;
	if (params->trim_front > 0 && (uint64_t)params->trim_front < params->duration)
		clip_start_ts += (uint64_t)params->trim_front;

	uint64_t clip_end_ts = params->last_frame_timestamp;
	const bool has_trim_end = params->trim_end > 0 && (uint64_t)params->trim_end < params->duration &&
				  (clip_end_ts - (uint64_t)params->trim_end > clip_start_ts);
	if (has_trim_end)
		clip_end_ts -= (uint64_t)params->trim_end;

	uint64_t valid_v_count = 0;
	for (uint64_t i = 0; i < params->video_frame_count; i++) {
		struct obs_source_frame *f = params->video_frames[i];
		if (!f)
			continue;
		if (f->timestamp < clip_start_ts)
			continue;
		if (has_trim_end ? (f->timestamp >= clip_end_ts) : (f->timestamp > clip_end_ts))
			continue;
		valid_v_count++;
	}

	if (valid_v_count == 0) {
		blog(LOG_WARNING, "[replay_save_queue] No video frames in trimmed range for '%s'",
		     params->source_name ? params->source_name : "Replay");
		return false;
	}

	struct replay_save_job *job = bzalloc(sizeof(*job));
	job->source_name = bstrdup((params->source_name && *params->source_name) ? params->source_name : "Replay");
	job->save_encoder =
		bstrdup((params->save_encoder && *params->save_encoder) ? params->save_encoder : SAVE_ENCODER_AUTO);
	job->save_container = bstrdup(
		(params->save_container && *params->save_container) ? params->save_container : SAVE_CONTAINER_MP4);
	job->lossless = params->lossless;

	job->video_frames = bzalloc((size_t)valid_v_count * sizeof(struct obs_source_frame *));
	uint64_t v_idx = 0;
	for (uint64_t i = 0; i < params->video_frame_count; i++) {
		struct obs_source_frame *f = params->video_frames[i];
		if (!f)
			continue;
		if (f->timestamp < clip_start_ts)
			continue;
		if (has_trim_end ? (f->timestamp >= clip_end_ts) : (f->timestamp > clip_end_ts))
			continue;
		replay_frame_addref(f);
		job->video_frames[v_idx++] = f;
		job->pinned_bytes += estimate_frame_bytes(f);
	}
	job->video_frame_count = v_idx;
	job->first_frame_timestamp = job->video_frames[0]->timestamp;
	job->last_frame_timestamp = job->video_frames[v_idx - 1]->timestamp;
	job->duration_ns = (job->last_frame_timestamp >= job->first_frame_timestamp)
				   ? (job->last_frame_timestamp - job->first_frame_timestamp)
				   : 0;

	job->oai = params->oai;
	if (params->audio_frames && params->audio_frame_count > 0) {
		job->audio_frames = bzalloc((size_t)params->audio_frame_count * sizeof(struct obs_audio_data));
		uint64_t a_idx = 0;
		for (uint64_t i = 0; i < params->audio_frame_count; i++) {
			const struct obs_audio_data *src = &params->audio_frames[i];
			if (!src->frames || src->timestamp < job->first_frame_timestamp ||
			    src->timestamp > job->last_frame_timestamp)
				continue;

			struct obs_audio_data *dst = &job->audio_frames[a_idx++];
			*dst = *src;
			const size_t plane_bytes = (size_t)src->frames * sizeof(float);
			for (size_t p = 0; p < MAX_AV_PLANES; p++) {
				if (src->data[p]) {
					dst->data[p] = bmemdup(src->data[p], plane_bytes);
					job->pinned_bytes += plane_bytes;
				} else {
					dst->data[p] = NULL;
				}
			}
		}
		job->audio_frame_count = a_idx;
	}

	const char *ext = SAVE_CONTAINER_MP4;
	if (job->lossless) {
		ext = "avi";
	} else if (astrcmpi(job->save_container, SAVE_CONTAINER_MKV) == 0) {
		ext = SAVE_CONTAINER_MKV;
	}

	pthread_mutex_lock(&g_queue.mutex);
	if (is_shutting_down()) {
		pthread_mutex_unlock(&g_queue.mutex);
		free_save_job(job);
		return false;
	}

	job->output_path =
		build_unique_output_path_locked(params->directory, params->file_format, job->source_name, ext);

	if (g_queue.tail) {
		g_queue.tail->next = job;
		g_queue.tail = job;
	} else {
		g_queue.head = job;
		g_queue.tail = job;
	}
	g_queue.pending_count++;
	g_queue.pinned_bytes += job->pinned_bytes;
	g_queue.enqueued_count++;

	const size_t total_queue_len = g_queue.pending_count + (g_queue.active_job ? 1 : 0);
	const size_t total_pinned = g_queue.pinned_bytes;

	blog(LOG_INFO,
	     "[replay_save_queue] Enqueued save job: source='%s' path='%s' frames=%" PRIu64
	     " duration=%.2fs job_pinned=%zu bytes (%.2f MB) total_pinned=%zu bytes (%.2f MB) queue_len=%zu",
	     job->source_name, job->output_path, job->video_frame_count, (double)job->duration_ns / 1e9,
	     job->pinned_bytes, (double)job->pinned_bytes / (1024.0 * 1024.0), total_pinned,
	     (double)total_pinned / (1024.0 * 1024.0), total_queue_len);

	pthread_cond_signal(&g_queue.cond);
	pthread_mutex_unlock(&g_queue.mutex);
	return true;
}

size_t replay_save_queue_pending_count(void)
{
	if (!g_queue.initialized)
		return 0;
	pthread_mutex_lock(&g_queue.mutex);
	const size_t count = g_queue.pending_count + (g_queue.active_job ? 1 : 0);
	pthread_mutex_unlock(&g_queue.mutex);
	return count;
}

size_t replay_save_queue_pinned_bytes(void)
{
	if (!g_queue.initialized)
		return 0;
	pthread_mutex_lock(&g_queue.mutex);
	const size_t bytes = g_queue.pinned_bytes;
	pthread_mutex_unlock(&g_queue.mutex);
	return bytes;
}

uint64_t replay_save_queue_completed_count(void)
{
	if (!g_queue.initialized)
		return 0;
	pthread_mutex_lock(&g_queue.mutex);
	const uint64_t count = g_queue.completed_count;
	pthread_mutex_unlock(&g_queue.mutex);
	return count;
}

long replay_save_queue_max_concurrent_encoders(void)
{
	return os_atomic_load_long(&g_queue.max_concurrent_encoders);
}

bool replay_save_queue_wait_idle(uint32_t timeout_ms)
{
	if (!g_queue.initialized)
		return true;

	const uint64_t start_ns = os_gettime_ns();
	const uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;

	pthread_mutex_lock(&g_queue.mutex);
	while (g_queue.head || g_queue.active_job) {
		if (timeout_ms > 0 && (os_gettime_ns() - start_ns) >= timeout_ns) {
			pthread_mutex_unlock(&g_queue.mutex);
			return false;
		}
		pthread_mutex_unlock(&g_queue.mutex);
		os_sleep_ms(10);
		pthread_mutex_lock(&g_queue.mutex);
	}
	pthread_mutex_unlock(&g_queue.mutex);
	return true;
}
