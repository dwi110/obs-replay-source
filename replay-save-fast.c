#include "replay-save-fast.h"
#include "replay-save-queue.h"

#ifndef REPLAY_FAST_SAVE

bool replay_save_fast_supported(void)
{
	return false;
}

bool replay_save_fast_available(void)
{
	return false;
}

enum replay_save_fast_result replay_save_fast_run(const struct replay_save_fast_input *in,
						 struct replay_save_fast_stats *stats)
{
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(stats);
	return REPLAY_SAVE_FAST_UNAVAILABLE;
}

#else

#include <inttypes.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#define FAST_STR2(x) #x
#define FAST_STR(x) FAST_STR2(x)
#define LIVE_LAG_SLOWDOWN_NS 2000000000ULL
#define LIVE_LATE_PAUSE_NS 150000000ULL
#define LIVE_LATE_RESUME_NS 50000000ULL
#define LIVE_LATE_CHECK_NS 20000000ULL
#define LIVE_PAUSE_BUDGET_NS 60000000000ULL

bool replay_save_fast_supported(void)
{
	return true;
}

bool replay_save_fast_available(void)
{
#ifdef _WIN32
	/* The plugin delay-loads FFmpeg (OBS 29.1 build base, FFmpeg headers matching the target OBS). A missing or
	 * different FFmpeg major version must disable this path instead of crashing on the first call. */
	static volatile long checked = 0;
	static volatile bool available = false;
	if (os_atomic_load_long(&checked))
		return available;
	static const char *const libs[] = {
		"avutil-" FAST_STR(LIBAVUTIL_VERSION_MAJOR),
		"avcodec-" FAST_STR(LIBAVCODEC_VERSION_MAJOR),
		"avformat-" FAST_STR(LIBAVFORMAT_VERSION_MAJOR),
	};
	bool ok = true;
	for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
		/* Kept loaded: the delay-load helper resolves the same module. */
		if (!os_dlopen(libs[i])) {
			blog(LOG_WARNING, "[replay_save_fast] %s.dll not found; saving stays at real time", libs[i]);
			ok = false;
			break;
		}
	}
	available = ok;
	os_atomic_set_long(&checked, 1);
	return available;
#else
	return true;
#endif
}

struct fast_ctx {
	const struct replay_save_fast_input *in;
	AVFormatContext *fmt;
	AVCodecContext *venc;
	AVCodecContext *aenc;
	AVStream *vst;
	AVStream *ast;
	AVFrame *vframe;
	AVFrame *aframe;
	AVPacket *pkt;
	enum video_format obs_fmt;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	enum video_colorspace colorspace;
	enum video_range_type range;

	float *audio[MAX_AV_PLANES];
	size_t channels;
	uint32_t sample_rate;
	uint64_t audio_samples;
	uint64_t audio_sent;

	video_scaler_t *scaler;
	enum video_format scaler_src_fmt;
	uint32_t scaler_src_w;
	uint32_t scaler_src_h;
};

static void log_av_error(const char *what, int err)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(err, buf, sizeof(buf));
	blog(LOG_WARNING, "[replay_save_fast] %s: %s", what, buf);
}

static bool is_422(enum video_format f)
{
	return f == VIDEO_FORMAT_UYVY || f == VIDEO_FORMAT_YUY2 || f == VIDEO_FORMAT_YVYU || f == VIDEO_FORMAT_I422;
}

static void set_color(AVCodecContext *c, enum video_colorspace cs, enum video_range_type range)
{
	switch (cs) {
	case VIDEO_CS_601:
		c->color_primaries = AVCOL_PRI_SMPTE170M;
		c->color_trc = AVCOL_TRC_SMPTE170M;
		c->colorspace = AVCOL_SPC_SMPTE170M;
		break;
	case VIDEO_CS_SRGB:
		c->color_primaries = AVCOL_PRI_BT709;
		c->color_trc = AVCOL_TRC_IEC61966_2_1;
		c->colorspace = AVCOL_SPC_BT709;
		break;
	default:
		c->color_primaries = AVCOL_PRI_BT709;
		c->color_trc = AVCOL_TRC_BT709;
		c->colorspace = AVCOL_SPC_BT709;
		break;
	}
	c->color_range = range == VIDEO_RANGE_FULL ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

static AVCodecContext *open_video_encoder(struct fast_ctx *f, const char *name)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return NULL;
	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (!c)
		return NULL;
	c->width = (int)f->width;
	c->height = (int)f->height;
	c->time_base = (AVRational){(int)f->fps_den, (int)f->fps_num};
	c->framerate = (AVRational){(int)f->fps_num, (int)f->fps_den};
	c->sample_aspect_ratio = (AVRational){1, 1};
	set_color(c, f->colorspace, f->range);
	if (f->fmt->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	const int q = f->in->save_quality;
	char qs[16];
	snprintf(qs, sizeof(qs), "%d", q);
	if (f->in->lossless) {
		c->pix_fmt = f->obs_fmt == VIDEO_FORMAT_I422 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
	} else {
		c->pix_fmt = AV_PIX_FMT_NV12;
		c->gop_size = (int)(2 * f->fps_num / (f->fps_den ? f->fps_den : 1));
		/* Same picture settings as the real-time path (P5, CQP, no B-frames) so both produce comparable files. */
		c->max_b_frames = 0;
		if (strstr(name, "nvenc")) {
			av_opt_set(c->priv_data, "preset", "p5", 0);
			av_opt_set(c->priv_data, "tune", "hq", 0);
			av_opt_set(c->priv_data, "rc", "constqp", 0);
			av_opt_set_int(c->priv_data, "qp", q, 0);
			av_opt_set_int(c->priv_data, "rc-lookahead", 0, 0);
		} else {
			av_opt_set(c->priv_data, "preset", "veryfast", 0);
			av_opt_set(c->priv_data, "crf", qs, 0);
			av_opt_set(c->priv_data, "profile", "high", 0);
		}
	}
	const int err = avcodec_open2(c, codec, NULL);
	if (err < 0) {
		char what[96];
		snprintf(what, sizeof(what), "cannot open video encoder '%s'", name);
		log_av_error(what, err);
		avcodec_free_context(&c);
		return NULL;
	}
	return c;
}

static AVCodecContext *open_audio_encoder(struct fast_ctx *f)
{
	const char *name = f->in->lossless ? "pcm_s16le" : "aac";
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return NULL;
	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (!c)
		return NULL;
	c->sample_rate = (int)f->sample_rate;
	c->sample_fmt = f->in->lossless ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLTP;
	av_channel_layout_default(&c->ch_layout, (int)f->channels);
	c->time_base = (AVRational){1, (int)f->sample_rate};
	if (!f->in->lossless)
		c->bit_rate = 192000;
	if (f->fmt->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	const int err = avcodec_open2(c, codec, NULL);
	if (err < 0) {
		log_av_error("cannot open audio encoder", err);
		avcodec_free_context(&c);
		return NULL;
	}
	return c;
}

/* Encoder candidates in the same order as the real-time path: HEVC NVENC -> H.264 NVENC -> x264. */
static size_t video_encoder_names(const struct replay_save_fast_input *in, const char **names)
{
	size_t n = 0;
	if (in->lossless) {
		names[n++] = "utvideo";
		return n;
	}
	const char *enc = in->save_encoder ? in->save_encoder : SAVE_ENCODER_AUTO;
	if (astrcmpi(enc, SAVE_ENCODER_X264) != 0 && in->nvenc_present) {
		if (astrcmpi(enc, SAVE_ENCODER_H264_NVENC) != 0)
			names[n++] = "hevc_nvenc";
		names[n++] = "h264_nvenc";
	}
	names[n++] = "libx264";
	return n;
}

static bool write_packets(struct fast_ctx *f, AVCodecContext *c, AVStream *st)
{
	for (;;) {
		int err = avcodec_receive_packet(c, f->pkt);
		if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
			return true;
		if (err < 0) {
			log_av_error("encode", err);
			return false;
		}
		av_packet_rescale_ts(f->pkt, c->time_base, st->time_base);
		f->pkt->stream_index = st->index;
		err = av_interleaved_write_frame(f->fmt, f->pkt);
		if (err < 0) {
			log_av_error("write packet", err);
			return false;
		}
	}
}

static bool send_frame(struct fast_ctx *f, AVCodecContext *c, AVStream *st, AVFrame *frame)
{
	const int err = avcodec_send_frame(c, frame);
	if (err < 0) {
		log_av_error("send frame", err);
		return false;
	}
	return write_packets(f, c, st);
}

static bool fill_video_frame(struct fast_ctx *f, const struct obs_source_frame *src)
{
	if (av_frame_make_writable(f->vframe) < 0)
		return false;
	struct video_frame dst = {0};
	uint32_t dst_linesize[MAX_AV_PLANES] = {0};
	for (size_t p = 0; p < MAX_AV_PLANES && p < AV_NUM_DATA_POINTERS; p++) {
		dst.data[p] = f->vframe->data[p];
		dst.linesize[p] = (uint32_t)f->vframe->linesize[p];
		dst_linesize[p] = dst.linesize[p];
	}
	if (src->format == f->obs_fmt && src->width == f->width && src->height == f->height) {
		struct video_frame in = {0};
		for (size_t p = 0; p < MAX_AV_PLANES; p++) {
			in.data[p] = src->data[p];
			in.linesize[p] = src->linesize[p];
		}
		video_frame_copy(&dst, &in, f->obs_fmt, f->height);
		return true;
	}
	if (!f->scaler || f->scaler_src_fmt != src->format || f->scaler_src_w != src->width ||
	    f->scaler_src_h != src->height) {
		if (f->scaler)
			video_scaler_destroy(f->scaler);
		f->scaler = NULL;
		struct video_scale_info ssi = {
			.format = src->format,
			.width = src->width,
			.height = src->height,
			.colorspace = f->colorspace,
			.range = src->full_range ? VIDEO_RANGE_FULL : f->range,
		};
		struct video_scale_info dsi = {
			.format = f->obs_fmt,
			.width = f->width,
			.height = f->height,
			.colorspace = f->colorspace,
			.range = f->range,
		};
		if (video_scaler_create(&f->scaler, &dsi, &ssi, VIDEO_SCALE_DEFAULT) != VIDEO_SCALER_SUCCESS) {
			blog(LOG_WARNING, "[replay_save_fast] cannot convert video format %d", (int)src->format);
			f->scaler = NULL;
			return false;
		}
		f->scaler_src_fmt = src->format;
		f->scaler_src_w = src->width;
		f->scaler_src_h = src->height;
	}
	return video_scaler_scale(f->scaler, dst.data, dst_linesize, (const uint8_t *const *)src->data,
				  src->linesize);
}

static uint64_t ns_to_samples(uint32_t rate, uint64_t ns)
{
	return util_mul_div64(ns, rate, 1000000000ULL);
}

/* Lays the captured PCM onto the clip timeline (clip start = first video frame), silence for gaps. */
static void build_audio_timeline(struct fast_ctx *f)
{
	const struct replay_save_fast_input *in = f->in;
	for (size_t ch = 0; ch < f->channels; ch++)
		f->audio[ch] = bzalloc((size_t)f->audio_samples * sizeof(float));
	for (uint64_t i = 0; i < in->audio_frame_count; i++) {
		const struct obs_audio_data *pkt = &in->audio_frames[i];
		uint64_t src_off = 0;
		uint64_t dst_off = 0;
		if (pkt->timestamp >= in->first_frame_timestamp)
			dst_off = ns_to_samples(f->sample_rate, pkt->timestamp - in->first_frame_timestamp);
		else
			src_off = ns_to_samples(f->sample_rate, in->first_frame_timestamp - pkt->timestamp);
		if (src_off >= pkt->frames || dst_off >= f->audio_samples)
			continue;
		uint64_t n = pkt->frames - src_off;
		if (n > f->audio_samples - dst_off)
			n = f->audio_samples - dst_off;
		for (size_t ch = 0; ch < f->channels; ch++) {
			const float *s = (const float *)pkt->data[ch];
			if (!s)
				continue;
			s += src_off;
			float *d = f->audio[ch] + dst_off;
			for (uint64_t k = 0; k < n; k++)
				d[k] += s[k];
		}
	}
}

static bool send_audio_until(struct fast_ctx *f, uint64_t until)
{
	if (until > f->audio_samples)
		until = f->audio_samples;
	const int fixed = f->aenc->frame_size > 0 ? f->aenc->frame_size : 1024;
	while (f->audio_sent < until) {
		uint64_t n = until - f->audio_sent;
		if (n < (uint64_t)fixed && until < f->audio_samples)
			return true; /* wait for a full encoder frame */
		if (n > (uint64_t)fixed)
			n = (uint64_t)fixed;
		AVFrame *a = f->aframe;
		av_frame_unref(a);
		a->format = f->aenc->sample_fmt;
		a->sample_rate = f->aenc->sample_rate;
		a->nb_samples = (int)n;
		if (av_channel_layout_copy(&a->ch_layout, &f->aenc->ch_layout) < 0 || av_frame_get_buffer(a, 0) < 0)
			return false;
		if (f->in->lossless) {
			int16_t *out = (int16_t *)a->data[0];
			for (uint64_t k = 0; k < n; k++) {
				for (size_t ch = 0; ch < f->channels; ch++) {
					float v = f->audio[ch][f->audio_sent + k];
					v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
					*out++ = (int16_t)(v * 32767.0f);
				}
			}
		} else {
			for (size_t ch = 0; ch < f->channels; ch++)
				memcpy(a->data[ch], f->audio[ch] + f->audio_sent, (size_t)n * sizeof(float));
		}
		a->pts = (int64_t)f->audio_sent;
		f->audio_sent += n;
		if (!send_frame(f, f->aenc, f->ast, a))
			return false;
	}
	return true;
}

/* Sleeps until target or abort; returns false on abort. */
static bool pace_until(const struct replay_save_fast_input *in, uint64_t target_ns)
{
	for (;;) {
		if (in->should_abort && in->should_abort())
			return false;
		const uint64_t now = os_gettime_ns();
		if (now >= target_ns)
			return true;
		const uint64_t left = target_ns - now;
		if (left > 10000000ULL)
			os_sleep_ms(10);
		else
			os_sleepto_ns(target_ns);
	}
}

/* Samples the camera delivery lateness at most every LIVE_LATE_CHECK_NS. Above LIVE_LATE_PAUSE_NS the save waits
 * until the cameras are back below LIVE_LATE_RESUME_NS (live first), within the per-clip pause budget. Returns false
 * on abort; *paused_out tells the caller to restart pacing from now. */
static bool live_late_gate(const struct replay_save_fast_input *in, struct replay_save_fast_stats *stats,
			   uint64_t *next_check_ns, bool *paused_out)
{
	*paused_out = false;
	if (!in->live_late_ns)
		return true;
	uint64_t now = os_gettime_ns();
	if (now < *next_check_ns)
		return true;
	*next_check_ns = now + LIVE_LATE_CHECK_NS;
	uint64_t late = in->live_late_ns();
	if (late > stats->live_late_max_ns)
		stats->live_late_max_ns = late;
	if (late <= LIVE_LATE_PAUSE_NS || stats->live_paused_ns >= LIVE_PAUSE_BUDGET_NS)
		return true;

	stats->live_pauses++;
	*paused_out = true;
	const uint64_t pause_start = now;
	while (late > LIVE_LATE_RESUME_NS && stats->live_paused_ns + (now - pause_start) < LIVE_PAUSE_BUDGET_NS) {
		if (in->should_abort && in->should_abort())
			return false;
		os_sleep_ms(20);
		now = os_gettime_ns();
		late = in->live_late_ns();
		if (late > stats->live_late_max_ns)
			stats->live_late_max_ns = late;
	}
	stats->live_paused_ns += now - pause_start;
	if (stats->live_paused_ns >= LIVE_PAUSE_BUDGET_NS)
		blog(LOG_WARNING, "[replay_save_fast] cameras still late after %.0f s of pauses; finishing at real time",
		     (double)LIVE_PAUSE_BUDGET_NS / 1e9);
	return true;
}

/* Frees FFmpeg/libobs resources; keeps the clip parameters so the next encoder candidate can reuse them. */
static void fast_ctx_close(struct fast_ctx *f)
{
	avcodec_free_context(&f->venc);
	avcodec_free_context(&f->aenc);
	if (f->fmt) {
		if (f->fmt->pb)
			avio_closep(&f->fmt->pb);
		avformat_free_context(f->fmt);
		f->fmt = NULL;
	}
	f->vst = NULL;
	f->ast = NULL;
	av_frame_free(&f->vframe);
	av_frame_free(&f->aframe);
	av_packet_free(&f->pkt);
	if (f->scaler)
		video_scaler_destroy(f->scaler);
	f->scaler = NULL;
	for (size_t ch = 0; ch < MAX_AV_PLANES; ch++) {
		bfree(f->audio[ch]);
		f->audio[ch] = NULL;
	}
}

/* Opens muxer + encoders for one video encoder candidate. */
static bool open_outputs(struct fast_ctx *f, const char *vcodec, const char *format_name)
{
	if (avformat_alloc_output_context2(&f->fmt, NULL, format_name, f->in->output_path) < 0 || !f->fmt)
		return false;
	f->venc = open_video_encoder(f, vcodec);
	if (!f->venc)
		return false;
	f->aenc = open_audio_encoder(f);
	if (!f->aenc)
		return false;
	f->vst = avformat_new_stream(f->fmt, NULL);
	f->ast = avformat_new_stream(f->fmt, NULL);
	if (!f->vst || !f->ast)
		return false;
	if (avcodec_parameters_from_context(f->vst->codecpar, f->venc) < 0 ||
	    avcodec_parameters_from_context(f->ast->codecpar, f->aenc) < 0)
		return false;
	f->vst->time_base = f->venc->time_base;
	f->vst->avg_frame_rate = f->venc->framerate;
	f->ast->time_base = f->aenc->time_base;
	int err = avio_open(&f->fmt->pb, f->in->output_path, AVIO_FLAG_WRITE);
	if (err < 0) {
		log_av_error("cannot open output file", err);
		return false;
	}
	err = avformat_write_header(f->fmt, NULL);
	if (err < 0) {
		log_av_error("cannot write header", err);
		return false;
	}
	f->vframe = av_frame_alloc();
	f->aframe = av_frame_alloc();
	f->pkt = av_packet_alloc();
	if (!f->vframe || !f->aframe || !f->pkt)
		return false;
	f->vframe->format = f->venc->pix_fmt;
	f->vframe->width = f->venc->width;
	f->vframe->height = f->venc->height;
	f->vframe->color_range = f->venc->color_range;
	f->vframe->colorspace = f->venc->colorspace;
	f->vframe->color_primaries = f->venc->color_primaries;
	f->vframe->color_trc = f->venc->color_trc;
	return av_frame_get_buffer(f->vframe, 0) >= 0;
}

enum replay_save_fast_result replay_save_fast_run(const struct replay_save_fast_input *in,
						 struct replay_save_fast_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	if (!in || !in->video_frames || !in->video_frame_count || !in->video_frames[0])
		return REPLAY_SAVE_FAST_FAILED;
	if (!replay_save_fast_available())
		return REPLAY_SAVE_FAST_UNAVAILABLE;

	struct obs_video_info ovi = {0};
	const bool have_ovi = obs_get_video_info(&ovi);
	struct obs_audio_info oai_base = {0};
	obs_get_audio_info(&oai_base);

	struct fast_ctx f = {0};
	f.in = in;
	f.width = in->video_frames[0]->width;
	f.height = in->video_frames[0]->height;
	f.fps_num = have_ovi && ovi.fps_num ? ovi.fps_num : 60;
	f.fps_den = have_ovi && ovi.fps_den ? ovi.fps_den : 1;
	f.colorspace = have_ovi ? ovi.colorspace : VIDEO_CS_709;
	f.range = have_ovi ? ovi.range : VIDEO_RANGE_PARTIAL;
	if (in->lossless)
		f.obs_fmt = is_422(in->video_frames[0]->format) ? VIDEO_FORMAT_I422 : VIDEO_FORMAT_I420;
	else
		f.obs_fmt = VIDEO_FORMAT_NV12;
	f.sample_rate = in->oai.samples_per_sec ? in->oai.samples_per_sec
						: (oai_base.samples_per_sec ? oai_base.samples_per_sec : 48000);
	const enum speaker_layout speakers = in->oai.speakers != SPEAKERS_UNKNOWN
						     ? in->oai.speakers
						     : (oai_base.speakers != SPEAKERS_UNKNOWN ? oai_base.speakers
											      : SPEAKERS_STEREO);
	f.channels = get_audio_channels(speakers);
	if (!f.width || !f.height || !f.channels || f.channels > MAX_AV_PLANES)
		return REPLAY_SAVE_FAST_FAILED;

	const uint64_t interval_ns = util_mul_div64(1000000000ULL, f.fps_den, f.fps_num);
	/* Constant frame rate over source time; gaps repeat the preceding frame (same schedule as real time). */
	const uint64_t frame_count =
		util_mul_div64(in->duration_ns + interval_ns / 2, f.fps_num, 1000000000ULL * f.fps_den) + 1;
	f.audio_samples = util_mul_div64(frame_count * f.fps_den, f.sample_rate, f.fps_num);

	const char *format_name = "mp4";
	if (in->lossless)
		format_name = "avi";
	else if (in->save_container && astrcmpi(in->save_container, SAVE_CONTAINER_MKV) == 0)
		format_name = "matroska";

	const char *names[3];
	const size_t name_count = video_encoder_names(in, names);
	bool opened = false;
	for (size_t i = 0; i < name_count && !opened; i++) {
		opened = open_outputs(&f, names[i], format_name);
		if (opened) {
			stats->codec = names[i];
		} else {
			fast_ctx_close(&f);
			os_unlink(in->output_path);
		}
	}
	if (!opened)
		return REPLAY_SAVE_FAST_UNAVAILABLE;

	build_audio_timeline(&f);

	enum replay_save_fast_result result = REPLAY_SAVE_FAST_FAILED;
	const uint64_t start_ns = os_gettime_ns();
	const uint64_t step_fast = in->max_speed >= 1 ? interval_ns / (uint64_t)in->max_speed : 0;
	uint64_t lag = in->live_lag_counter ? in->live_lag_counter() : 0;
	uint64_t slow_until = 0;
	uint64_t wall = start_ns;
	uint64_t source_idx = 0;
	uint64_t next_live_check = 0;

	for (uint64_t idx = 0; idx < frame_count; idx++) {
		bool paused = false;
		if (!live_late_gate(in, stats, &next_live_check, &paused)) {
			result = REPLAY_SAVE_FAST_ABORTED;
			goto done;
		}
		const uint64_t now = os_gettime_ns();
		if (paused) {
			wall = now;
			slow_until = stats->live_paused_ns >= LIVE_PAUSE_BUDGET_NS ? UINT64_MAX
										 : now + LIVE_LAG_SLOWDOWN_NS;
		}
		if (in->live_lag_counter) {
			const uint64_t cur = in->live_lag_counter();
			if (cur != lag) {
				lag = cur;
				if (slow_until != UINT64_MAX)
					slow_until = now + LIVE_LAG_SLOWDOWN_NS;
				stats->slowdowns++;
			}
		}
		/* Never faster than the limit on average; no catch-up bursts after falling behind or a pause. */
		const uint64_t target = wall + (now < slow_until ? interval_ns : step_fast);
		if (!pace_until(in, target)) {
			result = REPLAY_SAVE_FAST_ABORTED;
			goto done;
		}
		wall = target > now ? target : now;

		const uint64_t source_ts = in->first_frame_timestamp + idx * interval_ns + interval_ns / 2;
		while (source_idx + 1 < in->video_frame_count &&
		       in->video_frames[source_idx + 1]->timestamp <= source_ts)
			source_idx++;
		if (!fill_video_frame(&f, in->video_frames[source_idx]))
			goto done;
		f.vframe->pts = (int64_t)idx;
		if (!send_frame(&f, f.venc, f.vst, f.vframe))
			goto done;
		if (!send_audio_until(&f, util_mul_div64((idx + 1) * f.fps_den, f.sample_rate, f.fps_num)))
			goto done;
	}
	if (!send_audio_until(&f, f.audio_samples) || !send_frame(&f, f.venc, f.vst, NULL) ||
	    !send_frame(&f, f.aenc, f.ast, NULL))
		goto done;
	const int err = av_write_trailer(f.fmt);
	if (err < 0) {
		log_av_error("cannot finish file", err);
		goto done;
	}
	result = REPLAY_SAVE_FAST_OK;

done:
	stats->encode_ns = os_gettime_ns() - start_ns;
	fast_ctx_close(&f);
	if (result != REPLAY_SAVE_FAST_OK)
		os_unlink(in->output_path);
	return result;
}

#endif
