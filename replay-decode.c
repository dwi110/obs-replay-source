#include "replay-decode.h"
#include "replay.h"
#include "replay-pool.h"
#include "replay-convert.h"
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <string.h>
#include <stdio.h>

#ifndef REPLAY_FAST_SAVE

bool replay_decode_supported(void)
{
	return false;
}

bool replay_decode_file(const char *path, uint64_t base_ts, struct replay_decoded_clip *out,
			bool (*should_abort)(void *param), void *param, char *err, size_t err_size)
{
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(base_ts);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(should_abort);
	UNUSED_PARAMETER(param);
	if (err && err_size > 0)
		snprintf(err, err_size, "FFmpeg unavailable");
	return false;
}

void replay_decoded_clip_free(struct replay_decoded_clip *clip)
{
	if (!clip)
		return;

	if (clip->video_frames) {
		for (uint64_t i = 0; i < clip->video_frame_count; i++) {
			replay_frame_release(clip->video_frames[i]);
		}
		bfree(clip->video_frames);
	}
	if (clip->audio_frames) {
		for (uint64_t i = 0; i < clip->audio_frame_count; i++) {
			free_audio_packet(&clip->audio_frames[i]);
		}
		bfree(clip->audio_frames);
	}
	memset(clip, 0, sizeof(*clip));
}

#else

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>

#define FAST_STR2(x) #x
#define FAST_STR(x) FAST_STR2(x)

bool replay_decode_supported(void)
{
#ifdef _WIN32
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
		if (!os_dlopen(libs[i])) {
			blog(LOG_WARNING, "[replay_decode] %s.dll not found", libs[i]);
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

void replay_decoded_clip_free(struct replay_decoded_clip *clip)
{
	if (!clip)
		return;

	if (clip->video_frames) {
		for (uint64_t i = 0; i < clip->video_frame_count; i++) {
			replay_frame_release(clip->video_frames[i]);
		}
		bfree(clip->video_frames);
	}
	if (clip->audio_frames) {
		for (uint64_t i = 0; i < clip->audio_frame_count; i++) {
			free_audio_packet(&clip->audio_frames[i]);
		}
		bfree(clip->audio_frames);
	}
	memset(clip, 0, sizeof(*clip));
}

static inline enum speaker_layout channels_to_speaker_layout(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

struct decode_audio_scratch {
	float *buf;
	size_t capacity;
};

static bool convert_audio_to_planar_float(const AVFrame *aframe, int channels,
					  struct decode_audio_scratch *scratch, float **channel_ptrs)
{
	const int nb_samples = aframe->nb_samples;
	const size_t needed = (size_t)channels * (size_t)nb_samples;
	if (needed > scratch->capacity) {
		float *new_buf = brealloc(scratch->buf, needed * sizeof(float));
		if (!new_buf)
			return false;
		scratch->buf = new_buf;
		scratch->capacity = needed;
	}

	for (int ch = 0; ch < channels; ch++) {
		channel_ptrs[ch] = scratch->buf + (size_t)ch * (size_t)nb_samples;
	}

	switch (aframe->format) {
	case AV_SAMPLE_FMT_FLT: {
		const float *in = (const float *)aframe->extended_data[0];
		for (int s = 0; s < nb_samples; s++) {
			for (int ch = 0; ch < channels; ch++) {
				channel_ptrs[ch][s] = in[s * channels + ch];
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_S16: {
		const int16_t *in = (const int16_t *)aframe->extended_data[0];
		for (int s = 0; s < nb_samples; s++) {
			for (int ch = 0; ch < channels; ch++) {
				channel_ptrs[ch][s] = (float)in[s * channels + ch] / 32768.0f;
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_S16P: {
		for (int ch = 0; ch < channels; ch++) {
			const int16_t *in = (const int16_t *)aframe->extended_data[ch];
			for (int s = 0; s < nb_samples; s++) {
				channel_ptrs[ch][s] = (float)in[s] / 32768.0f;
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_S32: {
		const int32_t *in = (const int32_t *)aframe->extended_data[0];
		for (int s = 0; s < nb_samples; s++) {
			for (int ch = 0; ch < channels; ch++) {
				channel_ptrs[ch][s] = (float)in[s * channels + ch] / 2147483648.0f;
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_S32P: {
		for (int ch = 0; ch < channels; ch++) {
			const int32_t *in = (const int32_t *)aframe->extended_data[ch];
			for (int s = 0; s < nb_samples; s++) {
				channel_ptrs[ch][s] = (float)in[s] / 2147483648.0f;
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_U8: {
		const uint8_t *in = (const uint8_t *)aframe->extended_data[0];
		for (int s = 0; s < nb_samples; s++) {
			for (int ch = 0; ch < channels; ch++) {
				channel_ptrs[ch][s] = ((float)in[s * channels + ch] - 128.0f) / 128.0f;
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_U8P: {
		for (int ch = 0; ch < channels; ch++) {
			const uint8_t *in = (const uint8_t *)aframe->extended_data[ch];
			for (int s = 0; s < nb_samples; s++) {
				channel_ptrs[ch][s] = ((float)in[s] - 128.0f) / 128.0f;
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_DBL: {
		const double *in = (const double *)aframe->extended_data[0];
		for (int s = 0; s < nb_samples; s++) {
			for (int ch = 0; ch < channels; ch++) {
				channel_ptrs[ch][s] = (float)in[s * channels + ch];
			}
		}
		break;
	}
	case AV_SAMPLE_FMT_DBLP: {
		for (int ch = 0; ch < channels; ch++) {
			const double *in = (const double *)aframe->extended_data[ch];
			for (int s = 0; s < nb_samples; s++) {
				channel_ptrs[ch][s] = (float)in[s];
			}
		}
		break;
	}
	default:
		return false;
	}
	return true;
}

struct pending_audio_entry {
	struct obs_audio_data packet;
	int64_t pts_ns;
};

static bool append_audio_frame(struct obs_audio_data **frames, uint64_t *count, size_t *cap,
			       const struct obs_audio_data *pkt)
{
	if (*count >= *cap) {
		size_t new_cap = *cap ? *cap * 2 : 128;
		struct obs_audio_data *new_arr = brealloc(*frames, new_cap * sizeof(*new_arr));
		if (!new_arr)
			return false;
		*frames = new_arr;
		*cap = new_cap;
	}
	(*frames)[(*count)++] = *pkt;
	return true;
}

bool replay_decode_file(const char *path, uint64_t base_ts, struct replay_decoded_clip *out,
			bool (*should_abort)(void *param), void *param, char *err, size_t err_size)
{
	if (!path || !out) {
		if (err && err_size > 0)
			snprintf(err, err_size, "invalid arguments");
		return false;
	}
	memset(out, 0, sizeof(*out));

	if (!replay_decode_supported()) {
		if (err && err_size > 0)
			snprintf(err, err_size, "FFmpeg unavailable");
		return false;
	}

	AVFormatContext *fmt_ctx = NULL;
	int ret = avformat_open_input(&fmt_ctx, path, NULL, NULL);
	if (ret < 0) {
		if (err && err_size > 0) {
			char av_err[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_strerror(ret, av_err, sizeof(av_err));
			snprintf(err, err_size, "could not open input: %s", av_err);
		}
		return false;
	}

	ret = avformat_find_stream_info(fmt_ctx, NULL);
	if (ret < 0) {
		if (err && err_size > 0) {
			char av_err[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_strerror(ret, av_err, sizeof(av_err));
			snprintf(err, err_size, "could not find stream info: %s", av_err);
		}
		avformat_close_input(&fmt_ctx);
		return false;
	}

	const AVCodec *vcodec = NULL;
	int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
	if (video_stream_idx < 0 || !vcodec) {
		if (err && err_size > 0)
			snprintf(err, err_size, "no video stream found");
		avformat_close_input(&fmt_ctx);
		return false;
	}

	AVStream *vst = fmt_ctx->streams[video_stream_idx];
	AVCodecContext *vdec = avcodec_alloc_context3(vcodec);
	if (!vdec) {
		if (err && err_size > 0)
			snprintf(err, err_size, "could not allocate video decoder context");
		avformat_close_input(&fmt_ctx);
		return false;
	}

	ret = avcodec_parameters_to_context(vdec, vst->codecpar);
	if (ret < 0) {
		if (err && err_size > 0)
			snprintf(err, err_size, "could not copy video codec params");
		avcodec_free_context(&vdec);
		avformat_close_input(&fmt_ctx);
		return false;
	}
	vdec->thread_count = 0;

	ret = avcodec_open2(vdec, vcodec, NULL);
	if (ret < 0) {
		if (err && err_size > 0)
			snprintf(err, err_size, "could not open video decoder");
		avcodec_free_context(&vdec);
		avformat_close_input(&fmt_ctx);
		return false;
	}

	const AVCodec *acodec = NULL;
	int audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
	AVStream *ast = NULL;
	AVCodecContext *adec = NULL;
	int audio_channels = 0;
	enum speaker_layout audio_speakers = SPEAKERS_UNKNOWN;

	if (audio_stream_idx >= 0 && acodec) {
		ast = fmt_ctx->streams[audio_stream_idx];
		adec = avcodec_alloc_context3(acodec);
		if (adec) {
			ret = avcodec_parameters_to_context(adec, ast->codecpar);
			if (ret >= 0) {
				adec->thread_count = 0;
				ret = avcodec_open2(adec, acodec, NULL);
			}
			if (ret < 0) {
				avcodec_free_context(&adec);
				adec = NULL;
				audio_stream_idx = -1;
			}
		}
	}

	if (adec) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
		audio_channels = adec->ch_layout.nb_channels;
#else
		audio_channels = adec->channels;
#endif
		audio_speakers = channels_to_speaker_layout(audio_channels);
		if (audio_channels <= 0 || audio_channels > 8 || audio_speakers == SPEAKERS_UNKNOWN) {
			if (err && err_size > 0)
				snprintf(err, err_size, "unsupported audio channel count (%d)", audio_channels);
			avcodec_free_context(&vdec);
			avcodec_free_context(&adec);
			avformat_close_input(&fmt_ctx);
			return false;
		}
	}

	uint64_t v_frame_interval_ns = 16666666ULL;
	if (vst->avg_frame_rate.num > 0 && vst->avg_frame_rate.den > 0) {
		v_frame_interval_ns = (uint64_t)vst->avg_frame_rate.den * 1000000000ULL /
				      (uint64_t)vst->avg_frame_rate.num;
	} else if (vst->r_frame_rate.num > 0 && vst->r_frame_rate.den > 0) {
		v_frame_interval_ns = (uint64_t)vst->r_frame_rate.den * 1000000000ULL /
				      (uint64_t)vst->r_frame_rate.num;
	}

	struct replay_pool *pool = NULL;
	struct obs_source_frame **video_frames = NULL;
	size_t video_cap = 0;
	uint64_t video_count = 0;

	struct obs_audio_data *audio_frames = NULL;
	size_t audio_cap = 0;
	uint64_t audio_count = 0;

	struct pending_audio_entry *pending = NULL;
	size_t pending_cap = 0;
	size_t pending_count = 0;

	struct decode_audio_scratch scratch = {0};

	bool has_video_first_pts = false;
	int64_t video_first_pts_ns = 0;
	int64_t next_expected_vpts_ns = 0;
	int64_t next_expected_apts_ns = 0;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *vframe = av_frame_alloc();
	AVFrame *aframe = av_frame_alloc();
	if (!pkt || !vframe || !aframe) {
		if (err && err_size > 0)
			snprintf(err, err_size, "could not allocate av frames/packet");
		goto fail;
	}

	bool draining = false;
	int adec_sample_rate = adec ? adec->sample_rate : 0;

	for (;;) {
		if (should_abort && should_abort(param)) {
			if (err && err_size > 0)
				snprintf(err, err_size, "aborted");
			goto fail;
		}

		if (!draining) {
			ret = av_read_frame(fmt_ctx, pkt);
			if (ret == AVERROR_EOF) {
				draining = true;
				avcodec_send_packet(vdec, NULL);
				if (adec)
					avcodec_send_packet(adec, NULL);
			} else if (ret < 0) {
				if (err && err_size > 0) {
					char av_err[AV_ERROR_MAX_STRING_SIZE] = {0};
					av_strerror(ret, av_err, sizeof(av_err));
					snprintf(err, err_size, "read frame error: %s", av_err);
				}
				goto fail;
			}
		}

		if (!draining) {
			if (pkt->stream_index == video_stream_idx) {
				ret = avcodec_send_packet(vdec, pkt);
				if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
					if (err && err_size > 0)
						snprintf(err, err_size, "video decode send packet failed");
					av_packet_unref(pkt);
					goto fail;
				}
			} else if (audio_stream_idx >= 0 && pkt->stream_index == audio_stream_idx && adec) {
				ret = avcodec_send_packet(adec, pkt);
				if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
					if (err && err_size > 0)
						snprintf(err, err_size, "audio decode send packet failed");
					av_packet_unref(pkt);
					goto fail;
				}
			}
			av_packet_unref(pkt);
		}

		/* Receive all available video frames */
		for (;;) {
			ret = avcodec_receive_frame(vdec, vframe);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0) {
				if (err && err_size > 0)
					snprintf(err, err_size, "video decode receive frame failed");
				goto fail;
			}

			const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(vframe->format);
			if (desc && desc->comp[0].depth > 8) {
				if (err && err_size > 0)
					snprintf(err, err_size, "unsupported pixel format");
				goto fail;
			}

			enum video_format src_obs_fmt = VIDEO_FORMAT_NONE;
			switch (vframe->format) {
			case AV_PIX_FMT_NV12:
				src_obs_fmt = VIDEO_FORMAT_NV12;
				break;
			case AV_PIX_FMT_YUV420P:
			case AV_PIX_FMT_YUVJ420P:
				src_obs_fmt = VIDEO_FORMAT_I420;
				break;
			case AV_PIX_FMT_YUV422P:
			case AV_PIX_FMT_YUVJ422P:
				src_obs_fmt = VIDEO_FORMAT_I422;
				break;
			case AV_PIX_FMT_UYVY422:
				src_obs_fmt = VIDEO_FORMAT_UYVY;
				break;
			case AV_PIX_FMT_YUYV422:
				src_obs_fmt = VIDEO_FORMAT_YUY2;
				break;
			default:
				if (err && err_size > 0)
					snprintf(err, err_size, "unsupported pixel format");
				goto fail;
			}

			if ((vframe->width & 1) != 0 || (vframe->height & 1) != 0) {
				if (err && err_size > 0)
					snprintf(err, err_size, "odd frame dimensions not supported");
				goto fail;
			}

			if (!pool) {
				pool = replay_pool_create(VIDEO_FORMAT_NV12, (uint32_t)vframe->width,
							  (uint32_t)vframe->height, 0);
				if (!pool) {
					if (err && err_size > 0)
						snprintf(err, err_size, "could not create frame pool");
					goto fail;
				}
			}

			struct obs_source_frame *dst = replay_pool_acquire(pool);
			if (!dst) {
				if (err && err_size > 0)
					snprintf(err, err_size, "could not acquire pool frame");
				goto fail;
			}

			struct obs_source_frame src_view;
			memset(&src_view, 0, sizeof(src_view));
			src_view.format = src_obs_fmt;
			src_view.width = (uint32_t)vframe->width;
			src_view.height = (uint32_t)vframe->height;
			for (int p = 0; p < 4; p++) {
				src_view.data[p] = vframe->data[p];
				src_view.linesize[p] = (uint32_t)vframe->linesize[p];
			}

			if (!replay_convert_frame(dst, &src_view)) {
				replay_frame_release(dst);
				if (err && err_size > 0)
					snprintf(err, err_size, "frame conversion failed");
				goto fail;
			}

			enum video_colorspace cs = VIDEO_CS_709;
			if (vframe->colorspace == AVCOL_SPC_BT470BG ||
			    vframe->colorspace == AVCOL_SPC_SMPTE170M ||
			    vframe->colorspace == AVCOL_SPC_SMPTE240M) {
				cs = VIDEO_CS_601;
			} else if (vframe->colorspace == AVCOL_SPC_BT709) {
				cs = VIDEO_CS_709;
			} else if (vframe->height < 720) {
				cs = VIDEO_CS_601;
			} else {
				cs = VIDEO_CS_709;
			}

			enum video_range_type range = VIDEO_RANGE_PARTIAL;
			if (vframe->color_range == AVCOL_RANGE_JPEG ||
			    vframe->format == AV_PIX_FMT_YUVJ420P ||
			    vframe->format == AV_PIX_FMT_YUVJ422P) {
				range = VIDEO_RANGE_FULL;
			}

			dst->full_range = (range == VIDEO_RANGE_FULL);
			video_format_get_parameters_for_format(cs, range, VIDEO_FORMAT_NV12, dst->color_matrix,
							       dst->color_range_min, dst->color_range_max);

			int64_t pts = vframe->best_effort_timestamp;
			if (pts == AV_NOPTS_VALUE)
				pts = vframe->pts;

			int64_t pts_ns;
			if (pts != AV_NOPTS_VALUE) {
				pts_ns = av_rescale_q(pts, vst->time_base, (AVRational){1, 1000000000});
				next_expected_vpts_ns = pts_ns + (int64_t)v_frame_interval_ns;
			} else {
				pts_ns = next_expected_vpts_ns;
				next_expected_vpts_ns += (int64_t)v_frame_interval_ns;
			}

			if (!has_video_first_pts) {
				has_video_first_pts = true;
				video_first_pts_ns = pts_ns;

				/* Process any audio packets received prior to first video frame */
				for (size_t i = 0; i < pending_count; i++) {
					if (pending[i].pts_ns < video_first_pts_ns) {
						free_audio_packet(&pending[i].packet);
					} else {
						pending[i].packet.timestamp =
							base_ts + (uint64_t)(pending[i].pts_ns - video_first_pts_ns);
						if (audio_count > 0 &&
						    pending[i].packet.timestamp <
							    audio_frames[audio_count - 1].timestamp) {
							pending[i].packet.timestamp =
								audio_frames[audio_count - 1].timestamp;
						}
						if (!append_audio_frame(&audio_frames, &audio_count, &audio_cap,
									&pending[i].packet)) {
							free_audio_packet(&pending[i].packet);
						}
					}
				}
				bfree(pending);
				pending = NULL;
				pending_count = 0;
				pending_cap = 0;
			}

			uint64_t frame_ts =
				base_ts + (uint64_t)(pts_ns >= video_first_pts_ns ? (pts_ns - video_first_pts_ns) : 0);
			if (video_count > 0 && frame_ts < video_frames[video_count - 1]->timestamp)
				frame_ts = video_frames[video_count - 1]->timestamp;
			dst->timestamp = frame_ts;

			if (video_count >= video_cap) {
				size_t new_cap = video_cap ? video_cap * 2 : 128;
				struct obs_source_frame **new_arr =
					brealloc(video_frames, new_cap * sizeof(*video_frames));
				if (!new_arr) {
					replay_frame_release(dst);
					if (err && err_size > 0)
						snprintf(err, err_size, "out of memory for video frames");
					goto fail;
				}
				video_frames = new_arr;
				video_cap = new_cap;
			}
			video_frames[video_count++] = dst;
		}

		/* Receive all available audio frames */
		if (adec) {
			for (;;) {
				ret = avcodec_receive_frame(adec, aframe);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				if (ret < 0) {
					if (err && err_size > 0)
						snprintf(err, err_size, "audio decode receive frame failed");
					goto fail;
				}

				const int nb_samples = aframe->nb_samples;
				struct obs_audio_data tmp;
				memset(&tmp, 0, sizeof(tmp));
				tmp.frames = (uint32_t)nb_samples;

				if (aframe->format == AV_SAMPLE_FMT_FLTP) {
					for (int ch = 0; ch < audio_channels; ch++) {
						tmp.data[ch] = aframe->extended_data[ch];
					}
				} else {
					float *channel_ptrs[8] = {0};
					if (!convert_audio_to_planar_float(aframe, audio_channels, &scratch,
									   channel_ptrs)) {
						if (err && err_size > 0)
							snprintf(err, err_size, "unsupported audio sample format");
						goto fail;
					}
					for (int ch = 0; ch < audio_channels; ch++) {
						tmp.data[ch] = (uint8_t *)channel_ptrs[ch];
					}
				}

				struct obs_audio_data packet;
				replay_audio_packet_copy(&packet, &tmp);

				int64_t a_pts = aframe->best_effort_timestamp;
				if (a_pts == AV_NOPTS_VALUE)
					a_pts = aframe->pts;

				int64_t a_pts_ns;
				if (a_pts != AV_NOPTS_VALUE) {
					a_pts_ns = av_rescale_q(a_pts, ast->time_base, (AVRational){1, 1000000000});
					next_expected_apts_ns =
						a_pts_ns +
						(int64_t)((uint64_t)nb_samples * 1000000000ULL / (uint64_t)adec->sample_rate);
				} else {
					a_pts_ns = next_expected_apts_ns;
					next_expected_apts_ns +=
						(int64_t)((uint64_t)nb_samples * 1000000000ULL / (uint64_t)adec->sample_rate);
				}

				if (!has_video_first_pts) {
					if (pending_count >= pending_cap) {
						size_t new_cap = pending_cap ? pending_cap * 2 : 32;
						struct pending_audio_entry *new_pending =
							brealloc(pending, new_cap * sizeof(*new_pending));
						if (!new_pending) {
							free_audio_packet(&packet);
							if (err && err_size > 0)
								snprintf(err, err_size, "out of memory for pending audio");
							goto fail;
						}
						pending = new_pending;
						pending_cap = new_cap;
					}
					pending[pending_count++] = (struct pending_audio_entry){packet, a_pts_ns};
				} else {
					if (a_pts_ns < video_first_pts_ns) {
						free_audio_packet(&packet);
					} else {
						packet.timestamp =
							base_ts + (uint64_t)(a_pts_ns - video_first_pts_ns);
						if (audio_count > 0 &&
						    packet.timestamp < audio_frames[audio_count - 1].timestamp) {
							packet.timestamp = audio_frames[audio_count - 1].timestamp;
						}
						if (!append_audio_frame(&audio_frames, &audio_count, &audio_cap,
									&packet)) {
							free_audio_packet(&packet);
							if (err && err_size > 0)
								snprintf(err, err_size,
									 "out of memory for audio frames");
							goto fail;
						}
					}
				}
			}
		}

		if (draining) {
			/* Check if both decoders are fully drained */
			int vret = avcodec_receive_frame(vdec, vframe);
			int aret = adec ? avcodec_receive_frame(adec, aframe) : AVERROR_EOF;
			if ((vret == AVERROR_EOF || vret == AVERROR(EAGAIN)) &&
			    (aret == AVERROR_EOF || aret == AVERROR(EAGAIN))) {
				break;
			}
		}
	}

	if (video_count == 0) {
		if (err && err_size > 0)
			snprintf(err, err_size, "no video frames decoded");
		goto fail;
	}

	if (pending) {
		for (size_t i = 0; i < pending_count; i++) {
			free_audio_packet(&pending[i].packet);
		}
		bfree(pending);
		pending = NULL;
	}
	bfree(scratch.buf);
	scratch.buf = NULL;

	if (vframe)
		av_frame_free(&vframe);
	if (aframe)
		av_frame_free(&aframe);
	if (pkt)
		av_packet_free(&pkt);
	if (vdec)
		avcodec_free_context(&vdec);
	if (adec)
		avcodec_free_context(&adec);
	if (fmt_ctx)
		avformat_close_input(&fmt_ctx);

	if (pool)
		replay_pool_close(pool);

	out->video_frames = video_frames;
	out->video_frame_count = video_count;
	out->audio_frames = audio_frames;
	out->audio_frame_count = audio_count;
	out->first_frame_timestamp = video_frames[0]->timestamp;
	out->last_frame_timestamp = video_frames[video_count - 1]->timestamp;
	out->duration = out->last_frame_timestamp - out->first_frame_timestamp;
	if (audio_count > 0 && adec_sample_rate > 0) {
		out->oai.format = AUDIO_FORMAT_FLOAT_PLANAR;
		out->oai.samples_per_sec = (uint32_t)adec_sample_rate;
		out->oai.speakers = audio_speakers;
	} else {
		memset(&out->oai, 0, sizeof(out->oai));
	}
	return true;

fail:
	if (pending) {
		for (size_t i = 0; i < pending_count; i++) {
			free_audio_packet(&pending[i].packet);
		}
		bfree(pending);
	}
	if (video_frames) {
		for (uint64_t i = 0; i < video_count; i++) {
			replay_frame_release(video_frames[i]);
		}
		bfree(video_frames);
	}
	if (audio_frames) {
		for (uint64_t i = 0; i < audio_count; i++) {
			free_audio_packet(&audio_frames[i]);
		}
		bfree(audio_frames);
	}
	if (pool)
		replay_pool_close(pool);

	bfree(scratch.buf);

	if (vframe)
		av_frame_free(&vframe);
	if (aframe)
		av_frame_free(&aframe);
	if (pkt)
		av_packet_free(&pkt);
	if (vdec)
		avcodec_free_context(&vdec);
	if (adec)
		avcodec_free_context(&adec);
	if (fmt_ctx)
		avformat_close_input(&fmt_ctx);

	memset(out, 0, sizeof(*out));
	return false;
}

#endif
