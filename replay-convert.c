#include "replay-convert.h"
#include "replay.h"
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define REPLAY_HAVE_SSE2 1
#else
#define REPLAY_HAVE_SSE2 0
#endif

static bool is_422_8bit(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_I422:
		return true;
	default:
		return false;
	}
}

static bool is_420_8bit(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
		return true;
	default:
		return false;
	}
}

static bool is_nv12_convertible(enum video_format format)
{
	return is_422_8bit(format) || is_420_8bit(format);
}

void replay_convert_target_format(enum video_format src_format, uint32_t src_width, uint32_t src_height,
				  int storage_format, bool downscale_max_1080p, enum video_format *dst_format,
				  uint32_t *dst_width, uint32_t *dst_height)
{
	const bool even_dims = src_width >= 2 && src_height >= 2 && (src_width & 1U) == 0 && (src_height & 1U) == 0;
	const bool can_nv12 = is_nv12_convertible(src_format) && even_dims;
	const bool can_downscale = downscale_max_1080p && can_nv12 && (src_width > 1920 || src_height > 1080) &&
				   (src_width & 3U) == 0 && (src_height & 3U) == 0;
	const uint32_t out_w = can_downscale ? (src_width / 2) : src_width;
	const uint32_t out_h = can_downscale ? (src_height / 2) : src_height;

	if (storage_format == STORAGE_FORMAT_NV12 && can_nv12) {
		*dst_format = VIDEO_FORMAT_NV12;
		*dst_width = out_w;
		*dst_height = out_h;
	} else if (storage_format == STORAGE_FORMAT_UYVY && even_dims && is_422_8bit(src_format)) {
		*dst_format = VIDEO_FORMAT_UYVY;
		*dst_width = out_w;
		*dst_height = out_h;
	} else if (storage_format == STORAGE_FORMAT_UYVY && even_dims && is_420_8bit(src_format)) {
		*dst_format = VIDEO_FORMAT_NV12;
		*dst_width = out_w;
		*dst_height = out_h;
	} else if (can_downscale) {
		/* ORIGINAL mode keeps today's behaviour: a 2x downscale always lands in NV12. */
		*dst_format = VIDEO_FORMAT_NV12;
		*dst_width = out_w;
		*dst_height = out_h;
	} else {
		*dst_format = src_format;
		*dst_width = src_width;
		*dst_height = src_height;
	}
}

void replay_copy_frame_metadata(struct obs_source_frame *dst, const struct obs_source_frame *src)
{
	dst->timestamp = src->timestamp;
	memcpy(dst->color_matrix, src->color_matrix, sizeof(dst->color_matrix));
	dst->full_range = src->full_range;
	dst->max_luminance = src->max_luminance;
	memcpy(dst->color_range_min, src->color_range_min, sizeof(dst->color_range_min));
	memcpy(dst->color_range_max, src->color_range_max, sizeof(dst->color_range_max));
	dst->flip = src->flip;
	dst->flags = src->flags;
	dst->trc = src->trc;
}

static void copy_plane_2d(uint8_t *dst, uint32_t dst_linesize, const uint8_t *src, uint32_t src_linesize,
			  uint32_t row_bytes, uint32_t rows)
{
	if (!dst || !src || row_bytes == 0 || rows == 0)
		return;
	if (dst_linesize == src_linesize && dst_linesize == row_bytes) {
		memcpy(dst, src, (size_t)row_bytes * rows);
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy(dst + (size_t)y * dst_linesize, src + (size_t)y * src_linesize, row_bytes);
	}
}

/* ------------------------------------------------------------------------- */
/* 1x conversions to NV12                                                    */
/* ------------------------------------------------------------------------- */

#if REPLAY_HAVE_SSE2
/* Ring frames are written once and read again only when a replay is played or saved, so the
 * 1x paths stream them past the caches when the destination is 16-byte aligned. Measured on
 * i5-10500T: 0.44 ms -> 0.19 ms per 1080p UYVY->NV12 frame; it also stops multi-GB ring writes
 * from evicting the render thread's working set. */
static inline bool nv12_dst_can_stream(const struct obs_source_frame *dst)
{
	const uintptr_t bits = (uintptr_t)dst->data[0] | (uintptr_t)dst->data[1] | (uintptr_t)dst->linesize[0] |
			       (uintptr_t)dst->linesize[1];
	return (bits & 15) == 0;
}

static inline bool uyvy_dst_can_stream(const struct obs_source_frame *dst)
{
	const uintptr_t bits = (uintptr_t)dst->data[0] | (uintptr_t)dst->linesize[0];
	return (bits & 15) == 0;
}
static inline void store_si128(uint8_t *p, __m128i v, bool stream)
{
	if (stream)
		_mm_stream_si128((__m128i *)p, v);
	else
		_mm_storeu_si128((__m128i *)p, v);
}
#endif

static void convert_nv12_to_nv12_1x(struct obs_source_frame *dst, const struct obs_source_frame *src)
{
	const uint32_t w = dst->width;
	const uint32_t h = dst->height;
	const uint32_t ch = (h + 1) / 2;

	copy_plane_2d(dst->data[0], dst->linesize[0], src->data[0], src->linesize[0], w, h);
	copy_plane_2d(dst->data[1], dst->linesize[1], src->data[1], src->linesize[1], w, ch);
}

static void convert_i420_to_nv12_1x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t w = dst->width;
	const uint32_t h = dst->height;
	const uint32_t cw = w / 2;
	const uint32_t ch = h / 2;

	copy_plane_2d(dst->data[0], dst->linesize[0], src->data[0], src->linesize[0], w, h);

	for (uint32_t cy = 0; cy < ch; cy++) {
		const uint8_t *su = src->data[1] + (size_t)cy * src->linesize[1];
		const uint8_t *sv = src->data[2] + (size_t)cy * src->linesize[2];
		uint8_t *duv = dst->data[1] + (size_t)cy * dst->linesize[1];
		uint32_t cx = 0;

#if REPLAY_HAVE_SSE2
		const bool stream = use_simd && nv12_dst_can_stream(dst);
		if (use_simd) {
			for (; cx + 16 <= cw; cx += 16) {
				__m128i u = _mm_loadu_si128((const __m128i *)(su + cx));
				__m128i v = _mm_loadu_si128((const __m128i *)(sv + cx));
				__m128i uv_lo = _mm_unpacklo_epi8(u, v);
				__m128i uv_hi = _mm_unpackhi_epi8(u, v);
				store_si128(duv + 2 * cx, uv_lo, stream);
				store_si128(duv + 2 * cx + 16, uv_hi, stream);
			}
		}
#else
		(void)use_simd;
#endif
		for (; cx < cw; cx++) {
			duv[2 * cx + 0] = su[cx];
			duv[2 * cx + 1] = sv[cx];
		}
	}
}

static void convert_i422_to_nv12_1x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t w = dst->width;
	const uint32_t h = dst->height;
	const uint32_t cw = w / 2;
	const uint32_t ch = h / 2;

	copy_plane_2d(dst->data[0], dst->linesize[0], src->data[0], src->linesize[0], w, h);

	for (uint32_t cy = 0; cy < ch; cy++) {
		const uint8_t *su0 = src->data[1] + (size_t)(2 * cy) * src->linesize[1];
		const uint8_t *su1 = src->data[1] + (size_t)(2 * cy + 1) * src->linesize[1];
		const uint8_t *sv0 = src->data[2] + (size_t)(2 * cy) * src->linesize[2];
		const uint8_t *sv1 = src->data[2] + (size_t)(2 * cy + 1) * src->linesize[2];
		uint8_t *duv = dst->data[1] + (size_t)cy * dst->linesize[1];
		uint32_t cx = 0;

#if REPLAY_HAVE_SSE2
		const bool stream = use_simd && nv12_dst_can_stream(dst);
		if (use_simd) {
			for (; cx + 16 <= cw; cx += 16) {
				__m128i u0 = _mm_loadu_si128((const __m128i *)(su0 + cx));
				__m128i u1 = _mm_loadu_si128((const __m128i *)(su1 + cx));
				__m128i v0 = _mm_loadu_si128((const __m128i *)(sv0 + cx));
				__m128i v1 = _mm_loadu_si128((const __m128i *)(sv1 + cx));
				__m128i u = _mm_avg_epu8(u0, u1);
				__m128i v = _mm_avg_epu8(v0, v1);
				store_si128(duv + 2 * cx, _mm_unpacklo_epi8(u, v), stream);
				store_si128(duv + 2 * cx + 16, _mm_unpackhi_epi8(u, v), stream);
			}
		}
#else
		(void)use_simd;
#endif
		for (; cx < cw; cx++) {
			duv[2 * cx + 0] = (uint8_t)(((uint32_t)su0[cx] + (uint32_t)su1[cx] + 1U) >> 1);
			duv[2 * cx + 1] = (uint8_t)(((uint32_t)sv0[cx] + (uint32_t)sv1[cx] + 1U) >> 1);
		}
	}
}

static void convert_uyvy_to_nv12_1x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t w = dst->width;
	const uint32_t h = dst->height;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const bool stream = use_simd && nv12_dst_can_stream(dst);
#endif

	for (uint32_t y = 0; y < h; y += 2) {
		const uint8_t *s0 = src->data[0] + (size_t)y * src->linesize[0];
		const uint8_t *s1 = src->data[0] + (size_t)(y + 1) * src->linesize[0];
		uint8_t *dy0 = dst->data[0] + (size_t)y * dst->linesize[0];
		uint8_t *dy1 = dst->data[0] + (size_t)(y + 1) * dst->linesize[0];
		uint8_t *duv = dst->data[1] + (size_t)(y / 2) * dst->linesize[1];
		uint32_t x = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; x + 16 <= w; x += 16) {
				__m128i r0_a = _mm_loadu_si128((const __m128i *)(s0 + 2 * x));
				__m128i r0_b = _mm_loadu_si128((const __m128i *)(s0 + 2 * x + 16));
				__m128i r1_a = _mm_loadu_si128((const __m128i *)(s1 + 2 * x));
				__m128i r1_b = _mm_loadu_si128((const __m128i *)(s1 + 2 * x + 16));

				__m128i y0 = _mm_packus_epi16(_mm_srli_epi16(r0_a, 8), _mm_srli_epi16(r0_b, 8));
				__m128i y1 = _mm_packus_epi16(_mm_srli_epi16(r1_a, 8), _mm_srli_epi16(r1_b, 8));
				store_si128(dy0 + x, y0, stream);
				store_si128(dy1 + x, y1, stream);

				__m128i avg_a = _mm_avg_epu8(r0_a, r1_a);
				__m128i avg_b = _mm_avg_epu8(r0_b, r1_b);
				__m128i uv =
					_mm_packus_epi16(_mm_and_si128(avg_a, mask_00ff), _mm_and_si128(avg_b, mask_00ff));
				store_si128(duv + x, uv, stream);
			}
		}
#else
		(void)use_simd;
#endif
		for (; x < w; x += 2) {
			const uint8_t *p0 = s0 + 2 * x;
			const uint8_t *p1 = s1 + 2 * x;
			dy0[x + 0] = p0[1];
			dy0[x + 1] = p0[3];
			dy1[x + 0] = p1[1];
			dy1[x + 1] = p1[3];
			duv[x + 0] = (uint8_t)(((uint32_t)p0[0] + (uint32_t)p1[0] + 1U) >> 1);
			duv[x + 1] = (uint8_t)(((uint32_t)p0[2] + (uint32_t)p1[2] + 1U) >> 1);
		}
	}
}

static void convert_yuy2_to_nv12_1x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t w = dst->width;
	const uint32_t h = dst->height;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const bool stream = use_simd && nv12_dst_can_stream(dst);
#endif

	for (uint32_t y = 0; y < h; y += 2) {
		const uint8_t *s0 = src->data[0] + (size_t)y * src->linesize[0];
		const uint8_t *s1 = src->data[0] + (size_t)(y + 1) * src->linesize[0];
		uint8_t *dy0 = dst->data[0] + (size_t)y * dst->linesize[0];
		uint8_t *dy1 = dst->data[0] + (size_t)(y + 1) * dst->linesize[0];
		uint8_t *duv = dst->data[1] + (size_t)(y / 2) * dst->linesize[1];
		uint32_t x = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; x + 16 <= w; x += 16) {
				__m128i r0_a = _mm_loadu_si128((const __m128i *)(s0 + 2 * x));
				__m128i r0_b = _mm_loadu_si128((const __m128i *)(s0 + 2 * x + 16));
				__m128i r1_a = _mm_loadu_si128((const __m128i *)(s1 + 2 * x));
				__m128i r1_b = _mm_loadu_si128((const __m128i *)(s1 + 2 * x + 16));

				__m128i y0 = _mm_packus_epi16(_mm_and_si128(r0_a, mask_00ff),
							      _mm_and_si128(r0_b, mask_00ff));
				__m128i y1 = _mm_packus_epi16(_mm_and_si128(r1_a, mask_00ff),
							      _mm_and_si128(r1_b, mask_00ff));
				store_si128(dy0 + x, y0, stream);
				store_si128(dy1 + x, y1, stream);

				__m128i avg_a = _mm_avg_epu8(r0_a, r1_a);
				__m128i avg_b = _mm_avg_epu8(r0_b, r1_b);
				__m128i uv = _mm_packus_epi16(_mm_srli_epi16(avg_a, 8), _mm_srli_epi16(avg_b, 8));
				store_si128(duv + x, uv, stream);
			}
		}
#else
		(void)use_simd;
#endif
		for (; x < w; x += 2) {
			const uint8_t *p0 = s0 + 2 * x;
			const uint8_t *p1 = s1 + 2 * x;
			dy0[x + 0] = p0[0];
			dy0[x + 1] = p0[2];
			dy1[x + 0] = p1[0];
			dy1[x + 1] = p1[2];
			duv[x + 0] = (uint8_t)(((uint32_t)p0[1] + (uint32_t)p1[1] + 1U) >> 1);
			duv[x + 1] = (uint8_t)(((uint32_t)p0[3] + (uint32_t)p1[3] + 1U) >> 1);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* 1x conversions to UYVY                                                    */
/* ------------------------------------------------------------------------- */

static void convert_uyvy_to_uyvy_1x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t row_bytes = dst->width * 2;
	const uint32_t h = dst->height;

#if REPLAY_HAVE_SSE2
	if (use_simd) {
		const bool stream = uyvy_dst_can_stream(dst);
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *s = src->data[0] + (size_t)y * src->linesize[0];
			uint8_t *d = dst->data[0] + (size_t)y * dst->linesize[0];
			uint32_t bx = 0;
			for (; bx + 16 <= row_bytes; bx += 16) {
				__m128i v = _mm_loadu_si128((const __m128i *)(s + bx));
				store_si128(d + bx, v, stream);
			}
			for (; bx < row_bytes; bx++)
				d[bx] = s[bx];
		}
		return;
	}
#else
	(void)use_simd;
#endif
	copy_plane_2d(dst->data[0], dst->linesize[0], src->data[0], src->linesize[0], row_bytes, h);
}

static void convert_yuy2_to_uyvy_1x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t w = dst->width;
	const uint32_t h = dst->height;
#if REPLAY_HAVE_SSE2
	const bool stream = use_simd && uyvy_dst_can_stream(dst);
#endif

	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *s = src->data[0] + (size_t)y * src->linesize[0];
		uint8_t *d = dst->data[0] + (size_t)y * dst->linesize[0];
		uint32_t x = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; x + 8 <= w; x += 8) {
				__m128i v = _mm_loadu_si128((const __m128i *)(s + 2 * x));
				__m128i swapped = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
				store_si128(d + 2 * x, swapped, stream);
			}
		}
#else
		(void)use_simd;
#endif
		for (; x < w; x += 2) {
			const uint8_t *p = s + 2 * x;
			uint8_t *q = d + 2 * x;
			q[0] = p[1];
			q[1] = p[0];
			q[2] = p[3];
			q[3] = p[2];
		}
	}
}

static void convert_i422_to_uyvy_1x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t w = dst->width;
	const uint32_t h = dst->height;
	const uint32_t cw = w / 2;
#if REPLAY_HAVE_SSE2
	const bool stream = use_simd && uyvy_dst_can_stream(dst);
#endif

	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *sy = src->data[0] + (size_t)y * src->linesize[0];
		const uint8_t *su = src->data[1] + (size_t)y * src->linesize[1];
		const uint8_t *sv = src->data[2] + (size_t)y * src->linesize[2];
		uint8_t *d = dst->data[0] + (size_t)y * dst->linesize[0];
		uint32_t cx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; cx + 8 <= cw; cx += 8) {
				__m128i u = _mm_loadl_epi64((const __m128i *)(su + cx));
				__m128i v = _mm_loadl_epi64((const __m128i *)(sv + cx));
				__m128i uv = _mm_unpacklo_epi8(u, v);
				__m128i yv = _mm_loadu_si128((const __m128i *)(sy + 2 * cx));
				store_si128(d + 4 * cx, _mm_unpacklo_epi8(uv, yv), stream);
				store_si128(d + 4 * cx + 16, _mm_unpackhi_epi8(uv, yv), stream);
			}
		}
#else
		(void)use_simd;
#endif
		for (; cx < cw; cx++) {
			d[4 * cx + 0] = su[cx];
			d[4 * cx + 1] = sy[2 * cx + 0];
			d[4 * cx + 2] = sv[cx];
			d[4 * cx + 3] = sy[2 * cx + 1];
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Exact 2x downscale conversions to NV12                                    */
/* ------------------------------------------------------------------------- */

#if REPLAY_HAVE_SSE2
static inline __m128i hpair_sum_epu8(__m128i v, __m128i mask_00ff)
{
	return _mm_add_epi16(_mm_and_si128(v, mask_00ff), _mm_srli_epi16(v, 8));
}

static inline __m128i avg_2x2_epu8_to_epi16(__m128i row0, __m128i row1, __m128i mask_00ff, __m128i two)
{
	__m128i sum0 = hpair_sum_epu8(row0, mask_00ff);
	__m128i sum1 = hpair_sum_epu8(row1, mask_00ff);
	__m128i total = _mm_add_epi16(_mm_add_epi16(sum0, sum1), two);
	return _mm_srli_epi16(total, 2);
}
#endif

static void downscale_luma_2x(struct obs_source_frame *dst, const struct obs_source_frame *src, bool use_simd)
{
	const uint32_t dst_w = dst->width;
	const uint32_t dst_h = dst->height;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const __m128i two = _mm_set1_epi16(2);
#endif

	for (uint32_t dy = 0; dy < dst_h; dy++) {
		const uint8_t *s0 = src->data[0] + (size_t)(2 * dy) * src->linesize[0];
		const uint8_t *s1 = src->data[0] + (size_t)(2 * dy + 1) * src->linesize[0];
		uint8_t *dy_row = dst->data[0] + (size_t)dy * dst->linesize[0];
		uint32_t dx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; dx + 16 <= dst_w; dx += 16) {
				__m128i r0_a = _mm_loadu_si128((const __m128i *)(s0 + 2 * dx));
				__m128i r0_b = _mm_loadu_si128((const __m128i *)(s0 + 2 * dx + 16));
				__m128i r1_a = _mm_loadu_si128((const __m128i *)(s1 + 2 * dx));
				__m128i r1_b = _mm_loadu_si128((const __m128i *)(s1 + 2 * dx + 16));

				__m128i y_lo = avg_2x2_epu8_to_epi16(r0_a, r1_a, mask_00ff, two);
				__m128i y_hi = avg_2x2_epu8_to_epi16(r0_b, r1_b, mask_00ff, two);
				_mm_storeu_si128((__m128i *)(dy_row + dx), _mm_packus_epi16(y_lo, y_hi));
			}
		}
#else
		(void)use_simd;
#endif
		for (; dx < dst_w; dx++) {
			const uint32_t sum = (uint32_t)s0[2 * dx] + (uint32_t)s0[2 * dx + 1] + (uint32_t)s1[2 * dx] +
					     (uint32_t)s1[2 * dx + 1];
			dy_row[dx] = (uint8_t)((sum + 2U) >> 2);
		}
	}
}

static void convert_i420_to_nv12_2x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	downscale_luma_2x(dst, src, use_simd);

	const uint32_t dst_cw = dst->width / 2;
	const uint32_t dst_ch = dst->height / 2;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const __m128i two = _mm_set1_epi16(2);
#endif

	for (uint32_t cy = 0; cy < dst_ch; cy++) {
		const uint8_t *u0 = src->data[1] + (size_t)(2 * cy) * src->linesize[1];
		const uint8_t *u1 = src->data[1] + (size_t)(2 * cy + 1) * src->linesize[1];
		const uint8_t *v0 = src->data[2] + (size_t)(2 * cy) * src->linesize[2];
		const uint8_t *v1 = src->data[2] + (size_t)(2 * cy + 1) * src->linesize[2];
		uint8_t *duv = dst->data[1] + (size_t)cy * dst->linesize[1];
		uint32_t cx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; cx + 8 <= dst_cw; cx += 8) {
				__m128i u_avg = avg_2x2_epu8_to_epi16(_mm_loadu_si128((const __m128i *)(u0 + 2 * cx)),
								      _mm_loadu_si128((const __m128i *)(u1 + 2 * cx)),
								      mask_00ff, two);
				__m128i v_avg = avg_2x2_epu8_to_epi16(_mm_loadu_si128((const __m128i *)(v0 + 2 * cx)),
								      _mm_loadu_si128((const __m128i *)(v1 + 2 * cx)),
								      mask_00ff, two);
				__m128i uv = _mm_or_si128(u_avg, _mm_slli_epi16(v_avg, 8));
				_mm_storeu_si128((__m128i *)(duv + 2 * cx), uv);
			}
		}
#endif
		for (; cx < dst_cw; cx++) {
			const uint32_t usum = (uint32_t)u0[2 * cx] + (uint32_t)u0[2 * cx + 1] + (uint32_t)u1[2 * cx] +
					      (uint32_t)u1[2 * cx + 1];
			const uint32_t vsum = (uint32_t)v0[2 * cx] + (uint32_t)v0[2 * cx + 1] + (uint32_t)v1[2 * cx] +
					      (uint32_t)v1[2 * cx + 1];
			duv[2 * cx + 0] = (uint8_t)((usum + 2U) >> 2);
			duv[2 * cx + 1] = (uint8_t)((vsum + 2U) >> 2);
		}
	}
}

static void convert_nv12_to_nv12_2x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	downscale_luma_2x(dst, src, use_simd);

	const uint32_t dst_cw = dst->width / 2;
	const uint32_t dst_ch = dst->height / 2;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const __m128i two = _mm_set1_epi16(2);
#endif

	for (uint32_t cy = 0; cy < dst_ch; cy++) {
		const uint8_t *suv0 = src->data[1] + (size_t)(2 * cy) * src->linesize[1];
		const uint8_t *suv1 = src->data[1] + (size_t)(2 * cy + 1) * src->linesize[1];
		uint8_t *duv = dst->data[1] + (size_t)cy * dst->linesize[1];
		uint32_t cx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; cx + 8 <= dst_cw; cx += 8) {
				__m128i r0_a = _mm_loadu_si128((const __m128i *)(suv0 + 4 * cx));
				__m128i r0_b = _mm_loadu_si128((const __m128i *)(suv0 + 4 * cx + 16));
				__m128i r1_a = _mm_loadu_si128((const __m128i *)(suv1 + 4 * cx));
				__m128i r1_b = _mm_loadu_si128((const __m128i *)(suv1 + 4 * cx + 16));

				__m128i u0 = _mm_packus_epi16(_mm_and_si128(r0_a, mask_00ff),
							      _mm_and_si128(r0_b, mask_00ff));
				__m128i v0 = _mm_packus_epi16(_mm_srli_epi16(r0_a, 8), _mm_srli_epi16(r0_b, 8));
				__m128i u1 = _mm_packus_epi16(_mm_and_si128(r1_a, mask_00ff),
							      _mm_and_si128(r1_b, mask_00ff));
				__m128i v1 = _mm_packus_epi16(_mm_srli_epi16(r1_a, 8), _mm_srli_epi16(r1_b, 8));

				__m128i u_avg = avg_2x2_epu8_to_epi16(u0, u1, mask_00ff, two);
				__m128i v_avg = avg_2x2_epu8_to_epi16(v0, v1, mask_00ff, two);
				_mm_storeu_si128((__m128i *)(duv + 2 * cx), _mm_or_si128(u_avg, _mm_slli_epi16(v_avg, 8)));
			}
		}
#endif
		for (; cx < dst_cw; cx++) {
			const uint8_t *p0 = suv0 + 4 * cx;
			const uint8_t *p1 = suv1 + 4 * cx;
			duv[2 * cx + 0] = (uint8_t)(((uint32_t)p0[0] + p0[2] + p1[0] + p1[2] + 2U) >> 2);
			duv[2 * cx + 1] = (uint8_t)(((uint32_t)p0[1] + p0[3] + p1[1] + p1[3] + 2U) >> 2);
		}
	}
}

static void convert_packed422_to_nv12_2x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					      bool is_uyvy, bool use_simd)
{
	const uint32_t dst_w = dst->width;
	const uint32_t dst_ch = dst->height / 2;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const __m128i mask_ffff = _mm_set1_epi32(0x0000FFFF);
	const __m128i two = _mm_set1_epi16(2);
	const __m128i four32 = _mm_set1_epi32(4);
#endif

	for (uint32_t cy = 0; cy < dst_ch; cy++) {
		const uint8_t *s0 = src->data[0] + (size_t)(4 * cy + 0) * src->linesize[0];
		const uint8_t *s1 = src->data[0] + (size_t)(4 * cy + 1) * src->linesize[0];
		const uint8_t *s2 = src->data[0] + (size_t)(4 * cy + 2) * src->linesize[0];
		const uint8_t *s3 = src->data[0] + (size_t)(4 * cy + 3) * src->linesize[0];
		uint8_t *dy0 = dst->data[0] + (size_t)(2 * cy + 0) * dst->linesize[0];
		uint8_t *dy1 = dst->data[0] + (size_t)(2 * cy + 1) * dst->linesize[0];
		uint8_t *duv = dst->data[1] + (size_t)cy * dst->linesize[1];
		uint32_t dx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; dx + 8 <= dst_w; dx += 8) {
				__m128i r0_a = _mm_loadu_si128((const __m128i *)(s0 + 4 * dx));
				__m128i r0_b = _mm_loadu_si128((const __m128i *)(s0 + 4 * dx + 16));
				__m128i r1_a = _mm_loadu_si128((const __m128i *)(s1 + 4 * dx));
				__m128i r1_b = _mm_loadu_si128((const __m128i *)(s1 + 4 * dx + 16));
				__m128i r2_a = _mm_loadu_si128((const __m128i *)(s2 + 4 * dx));
				__m128i r2_b = _mm_loadu_si128((const __m128i *)(s2 + 4 * dx + 16));
				__m128i r3_a = _mm_loadu_si128((const __m128i *)(s3 + 4 * dx));
				__m128i r3_b = _mm_loadu_si128((const __m128i *)(s3 + 4 * dx + 16));

				__m128i y0, y1, y2, y3;
				__m128i uv0, uv1, uv2, uv3;
				if (is_uyvy) {
					y0 = _mm_packus_epi16(_mm_srli_epi16(r0_a, 8), _mm_srli_epi16(r0_b, 8));
					y1 = _mm_packus_epi16(_mm_srli_epi16(r1_a, 8), _mm_srli_epi16(r1_b, 8));
					y2 = _mm_packus_epi16(_mm_srli_epi16(r2_a, 8), _mm_srli_epi16(r2_b, 8));
					y3 = _mm_packus_epi16(_mm_srli_epi16(r3_a, 8), _mm_srli_epi16(r3_b, 8));

					uv0 = _mm_packus_epi16(_mm_and_si128(r0_a, mask_00ff),
							       _mm_and_si128(r0_b, mask_00ff));
					uv1 = _mm_packus_epi16(_mm_and_si128(r1_a, mask_00ff),
							       _mm_and_si128(r1_b, mask_00ff));
					uv2 = _mm_packus_epi16(_mm_and_si128(r2_a, mask_00ff),
							       _mm_and_si128(r2_b, mask_00ff));
					uv3 = _mm_packus_epi16(_mm_and_si128(r3_a, mask_00ff),
							       _mm_and_si128(r3_b, mask_00ff));
				} else {
					y0 = _mm_packus_epi16(_mm_and_si128(r0_a, mask_00ff),
							      _mm_and_si128(r0_b, mask_00ff));
					y1 = _mm_packus_epi16(_mm_and_si128(r1_a, mask_00ff),
							      _mm_and_si128(r1_b, mask_00ff));
					y2 = _mm_packus_epi16(_mm_and_si128(r2_a, mask_00ff),
							      _mm_and_si128(r2_b, mask_00ff));
					y3 = _mm_packus_epi16(_mm_and_si128(r3_a, mask_00ff),
							      _mm_and_si128(r3_b, mask_00ff));

					uv0 = _mm_packus_epi16(_mm_srli_epi16(r0_a, 8), _mm_srli_epi16(r0_b, 8));
					uv1 = _mm_packus_epi16(_mm_srli_epi16(r1_a, 8), _mm_srli_epi16(r1_b, 8));
					uv2 = _mm_packus_epi16(_mm_srli_epi16(r2_a, 8), _mm_srli_epi16(r2_b, 8));
					uv3 = _mm_packus_epi16(_mm_srli_epi16(r3_a, 8), _mm_srli_epi16(r3_b, 8));
				}

				__m128i dy0_16 = avg_2x2_epu8_to_epi16(y0, y1, mask_00ff, two);
				__m128i dy1_16 = avg_2x2_epu8_to_epi16(y2, y3, mask_00ff, two);
				__m128i dy01 = _mm_packus_epi16(dy0_16, dy1_16);
				_mm_storel_epi64((__m128i *)(dy0 + dx), dy01);
				_mm_storel_epi64((__m128i *)(dy1 + dx), _mm_srli_si128(dy01, 8));

				__m128i u_vsum = _mm_add_epi16(
					_mm_add_epi16(_mm_and_si128(uv0, mask_00ff), _mm_and_si128(uv1, mask_00ff)),
					_mm_add_epi16(_mm_and_si128(uv2, mask_00ff), _mm_and_si128(uv3, mask_00ff)));
				__m128i v_vsum =
					_mm_add_epi16(_mm_add_epi16(_mm_srli_epi16(uv0, 8), _mm_srli_epi16(uv1, 8)),
						      _mm_add_epi16(_mm_srli_epi16(uv2, 8), _mm_srli_epi16(uv3, 8)));

				__m128i u_out32 = _mm_srli_epi32(
					_mm_add_epi32(_mm_add_epi32(_mm_and_si128(u_vsum, mask_ffff),
								    _mm_srli_epi32(u_vsum, 16)),
						      four32),
					3);
				__m128i v_out32 = _mm_srli_epi32(
					_mm_add_epi32(_mm_add_epi32(_mm_and_si128(v_vsum, mask_ffff),
								    _mm_srli_epi32(v_vsum, 16)),
						      four32),
					3);

				__m128i uv_16 = _mm_packs_epi32(u_out32, v_out32);
				__m128i uv_8 = _mm_packus_epi16(uv_16, uv_16);
				__m128i uv_interleaved = _mm_unpacklo_epi8(uv_8, _mm_srli_si128(uv_8, 4));
				_mm_storel_epi64((__m128i *)(duv + dx), uv_interleaved);
			}
		}
#else
		(void)use_simd;
#endif
		const uint32_t y0_off = is_uyvy ? 1U : 0U;
		const uint32_t y1_off = is_uyvy ? 3U : 2U;
		const uint32_t u_off = is_uyvy ? 0U : 1U;
		const uint32_t v_off = is_uyvy ? 2U : 3U;

		for (; dx < dst_w; dx += 2) {
			const uint8_t *p0 = s0 + 4 * dx;
			const uint8_t *p1 = s1 + 4 * dx;
			const uint8_t *p2 = s2 + 4 * dx;
			const uint8_t *p3 = s3 + 4 * dx;

			dy0[dx + 0] = (uint8_t)(((uint32_t)p0[y0_off] + p0[y1_off] + p1[y0_off] + p1[y1_off] + 2U) >> 2);
			dy0[dx + 1] =
				(uint8_t)(((uint32_t)p0[4 + y0_off] + p0[4 + y1_off] + p1[4 + y0_off] + p1[4 + y1_off] + 2U) >>
					  2);
			dy1[dx + 0] = (uint8_t)(((uint32_t)p2[y0_off] + p2[y1_off] + p3[y0_off] + p3[y1_off] + 2U) >> 2);
			dy1[dx + 1] =
				(uint8_t)(((uint32_t)p2[4 + y0_off] + p2[4 + y1_off] + p3[4 + y0_off] + p3[4 + y1_off] + 2U) >>
					  2);

			const uint32_t usum = (uint32_t)p0[u_off] + p0[4 + u_off] + p1[u_off] + p1[4 + u_off] + p2[u_off] +
					      p2[4 + u_off] + p3[u_off] + p3[4 + u_off];
			const uint32_t vsum = (uint32_t)p0[v_off] + p0[4 + v_off] + p1[v_off] + p1[4 + v_off] + p2[v_off] +
					      p2[4 + v_off] + p3[v_off] + p3[4 + v_off];

			duv[dx + 0] = (uint8_t)((usum + 4U) >> 3);
			duv[dx + 1] = (uint8_t)((vsum + 4U) >> 3);
		}
	}
}
static void convert_i422_to_nv12_2x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	downscale_luma_2x(dst, src, use_simd);

	const uint32_t dst_cw = dst->width / 2;
	const uint32_t dst_ch = dst->height / 2;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const __m128i four16 = _mm_set1_epi16(4);
#endif

	for (uint32_t cy = 0; cy < dst_ch; cy++) {
		const uint8_t *u0 = src->data[1] + (size_t)(4 * cy + 0) * src->linesize[1];
		const uint8_t *u1 = src->data[1] + (size_t)(4 * cy + 1) * src->linesize[1];
		const uint8_t *u2 = src->data[1] + (size_t)(4 * cy + 2) * src->linesize[1];
		const uint8_t *u3 = src->data[1] + (size_t)(4 * cy + 3) * src->linesize[1];
		const uint8_t *v0 = src->data[2] + (size_t)(4 * cy + 0) * src->linesize[2];
		const uint8_t *v1 = src->data[2] + (size_t)(4 * cy + 1) * src->linesize[2];
		const uint8_t *v2 = src->data[2] + (size_t)(4 * cy + 2) * src->linesize[2];
		const uint8_t *v3 = src->data[2] + (size_t)(4 * cy + 3) * src->linesize[2];
		uint8_t *duv = dst->data[1] + (size_t)cy * dst->linesize[1];
		uint32_t cx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; cx + 8 <= dst_cw; cx += 8) {
				__m128i u_s01 = _mm_add_epi16(
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(u0 + 2 * cx)), mask_00ff),
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(u1 + 2 * cx)), mask_00ff));
				__m128i u_s23 = _mm_add_epi16(
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(u2 + 2 * cx)), mask_00ff),
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(u3 + 2 * cx)), mask_00ff));
				__m128i u_avg = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(u_s01, u_s23), four16), 3);

				__m128i v_s01 = _mm_add_epi16(
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(v0 + 2 * cx)), mask_00ff),
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(v1 + 2 * cx)), mask_00ff));
				__m128i v_s23 = _mm_add_epi16(
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(v2 + 2 * cx)), mask_00ff),
					hpair_sum_epu8(_mm_loadu_si128((const __m128i *)(v3 + 2 * cx)), mask_00ff));
				__m128i v_avg = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(v_s01, v_s23), four16), 3);

				_mm_storeu_si128((__m128i *)(duv + 2 * cx), _mm_or_si128(u_avg, _mm_slli_epi16(v_avg, 8)));
			}
		}
#endif
		for (; cx < dst_cw; cx++) {
			const uint32_t usum = (uint32_t)u0[2 * cx] + u0[2 * cx + 1] + u1[2 * cx] + u1[2 * cx + 1] +
					      u2[2 * cx] + u2[2 * cx + 1] + u3[2 * cx] + u3[2 * cx + 1];
			const uint32_t vsum = (uint32_t)v0[2 * cx] + v0[2 * cx + 1] + v1[2 * cx] + v1[2 * cx + 1] +
					      v2[2 * cx] + v2[2 * cx + 1] + v3[2 * cx] + v3[2 * cx + 1];
			duv[2 * cx + 0] = (uint8_t)((usum + 4U) >> 3);
			duv[2 * cx + 1] = (uint8_t)((vsum + 4U) >> 3);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Exact 2x downscale conversions to UYVY                                    */
/*                                                                           */
/* 2x2 box filter preserving 4:2:2 chroma subsampling:                       */
/* For each destination pixel pair (dx, dx+1) at row dy (dx even):           */
/*   Y0(dy, dx)   = (Y(2*dy, 2*dx)   + Y(2*dy, 2*dx+1)   +                   */
/*                   Y(2*dy+1, 2*dx) + Y(2*dy+1, 2*dx+1) + 2) >> 2           */
/*   Y1(dy, dx+1) = (Y(2*dy, 2*dx+2)   + Y(2*dy, 2*dx+3)   +                 */
/*                   Y(2*dy+1, 2*dx+2) + Y(2*dy+1, 2*dx+3) + 2) >> 2         */
/*   U(dy, dx/2)  = (U(2*dy, dx) + U(2*dy, dx+1) +                           */
/*                   U(2*dy+1, dx) + U(2*dy+1, dx+1) + 2) >> 2               */
/*   V(dy, dx/2)  = (V(2*dy, dx) + V(2*dy, dx+1) +                           */
/*                   V(2*dy+1, dx) + V(2*dy+1, dx+1) + 2) >> 2               */
/* ------------------------------------------------------------------------- */

static void convert_packed422_to_uyvy_2x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					      bool is_uyvy, bool use_simd)
{
	const uint32_t dst_w = dst->width;
	const uint32_t dst_h = dst->height;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const __m128i mask_ffff = _mm_set1_epi32(0x0000FFFF);
	const __m128i two = _mm_set1_epi16(2);
	const __m128i two32 = _mm_set1_epi32(2);
#endif

	for (uint32_t dy = 0; dy < dst_h; dy++) {
		const uint8_t *s0 = src->data[0] + (size_t)(2 * dy + 0) * src->linesize[0];
		const uint8_t *s1 = src->data[0] + (size_t)(2 * dy + 1) * src->linesize[0];
		uint8_t *d = dst->data[0] + (size_t)dy * dst->linesize[0];
		uint32_t dx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; dx + 8 <= dst_w; dx += 8) {
				__m128i r0_a = _mm_loadu_si128((const __m128i *)(s0 + 4 * dx));
				__m128i r0_b = _mm_loadu_si128((const __m128i *)(s0 + 4 * dx + 16));
				__m128i r1_a = _mm_loadu_si128((const __m128i *)(s1 + 4 * dx));
				__m128i r1_b = _mm_loadu_si128((const __m128i *)(s1 + 4 * dx + 16));

				__m128i y0, y1, uv0, uv1;
				if (is_uyvy) {
					y0 = _mm_packus_epi16(_mm_srli_epi16(r0_a, 8), _mm_srli_epi16(r0_b, 8));
					y1 = _mm_packus_epi16(_mm_srli_epi16(r1_a, 8), _mm_srli_epi16(r1_b, 8));
					uv0 = _mm_packus_epi16(_mm_and_si128(r0_a, mask_00ff),
							       _mm_and_si128(r0_b, mask_00ff));
					uv1 = _mm_packus_epi16(_mm_and_si128(r1_a, mask_00ff),
							       _mm_and_si128(r1_b, mask_00ff));
				} else {
					y0 = _mm_packus_epi16(_mm_and_si128(r0_a, mask_00ff),
							      _mm_and_si128(r0_b, mask_00ff));
					y1 = _mm_packus_epi16(_mm_and_si128(r1_a, mask_00ff),
							      _mm_and_si128(r1_b, mask_00ff));
					uv0 = _mm_packus_epi16(_mm_srli_epi16(r0_a, 8), _mm_srli_epi16(r0_b, 8));
					uv1 = _mm_packus_epi16(_mm_srli_epi16(r1_a, 8), _mm_srli_epi16(r1_b, 8));
				}

				__m128i dy_16 = avg_2x2_epu8_to_epi16(y0, y1, mask_00ff, two);
				__m128i y_out8 = _mm_packus_epi16(dy_16, dy_16);

				__m128i u_vsum = _mm_add_epi16(_mm_and_si128(uv0, mask_00ff),
							       _mm_and_si128(uv1, mask_00ff));
				__m128i v_vsum = _mm_add_epi16(_mm_srli_epi16(uv0, 8), _mm_srli_epi16(uv1, 8));

				__m128i u_out32 = _mm_srli_epi32(
					_mm_add_epi32(_mm_add_epi32(_mm_and_si128(u_vsum, mask_ffff),
								    _mm_srli_epi32(u_vsum, 16)),
						      two32),
					2);
				__m128i v_out32 = _mm_srli_epi32(
					_mm_add_epi32(_mm_add_epi32(_mm_and_si128(v_vsum, mask_ffff),
								    _mm_srli_epi32(v_vsum, 16)),
						      two32),
					2);

				__m128i uv_16 = _mm_packs_epi32(u_out32, v_out32);
				__m128i uv_8 = _mm_packus_epi16(uv_16, uv_16);
				__m128i uv_interleaved = _mm_unpacklo_epi8(uv_8, _mm_srli_si128(uv_8, 4));
				__m128i uyvy_out = _mm_unpacklo_epi8(uv_interleaved, y_out8);
				_mm_storeu_si128((__m128i *)(d + 2 * dx), uyvy_out);
			}
		}
#else
		(void)use_simd;
#endif
		const uint32_t y0_off = is_uyvy ? 1U : 0U;
		const uint32_t y1_off = is_uyvy ? 3U : 2U;
		const uint32_t u_off = is_uyvy ? 0U : 1U;
		const uint32_t v_off = is_uyvy ? 2U : 3U;

		for (; dx < dst_w; dx += 2) {
			const uint8_t *p0 = s0 + 4 * dx;
			const uint8_t *p1 = s1 + 4 * dx;

			const uint32_t dy0 =
				((uint32_t)p0[y0_off] + p0[y1_off] + p1[y0_off] + p1[y1_off] + 2U) >> 2;
			const uint32_t dy1 = ((uint32_t)p0[4 + y0_off] + p0[4 + y1_off] + p1[4 + y0_off] +
					      p1[4 + y1_off] + 2U) >>
					     2;
			const uint32_t du = ((uint32_t)p0[u_off] + p0[4 + u_off] + p1[u_off] + p1[4 + u_off] + 2U) >> 2;
			const uint32_t dv = ((uint32_t)p0[v_off] + p0[4 + v_off] + p1[v_off] + p1[4 + v_off] + 2U) >> 2;

			d[2 * dx + 0] = (uint8_t)du;
			d[2 * dx + 1] = (uint8_t)dy0;
			d[2 * dx + 2] = (uint8_t)dv;
			d[2 * dx + 3] = (uint8_t)dy1;
		}
	}
}

static void convert_i422_to_uyvy_2x_impl(struct obs_source_frame *dst, const struct obs_source_frame *src,
					 bool use_simd)
{
	const uint32_t dst_w = dst->width;
	const uint32_t dst_h = dst->height;
#if REPLAY_HAVE_SSE2
	const __m128i mask_00ff = _mm_set1_epi16(0x00FF);
	const __m128i two = _mm_set1_epi16(2);
#endif

	for (uint32_t dy = 0; dy < dst_h; dy++) {
		const uint8_t *sy0 = src->data[0] + (size_t)(2 * dy + 0) * src->linesize[0];
		const uint8_t *sy1 = src->data[0] + (size_t)(2 * dy + 1) * src->linesize[0];
		const uint8_t *su0 = src->data[1] + (size_t)(2 * dy + 0) * src->linesize[1];
		const uint8_t *su1 = src->data[1] + (size_t)(2 * dy + 1) * src->linesize[1];
		const uint8_t *sv0 = src->data[2] + (size_t)(2 * dy + 0) * src->linesize[2];
		const uint8_t *sv1 = src->data[2] + (size_t)(2 * dy + 1) * src->linesize[2];
		uint8_t *d = dst->data[0] + (size_t)dy * dst->linesize[0];
		uint32_t dx = 0;

#if REPLAY_HAVE_SSE2
		if (use_simd) {
			for (; dx + 16 <= dst_w; dx += 16) {
				__m128i y0_a = _mm_loadu_si128((const __m128i *)(sy0 + 2 * dx));
				__m128i y0_b = _mm_loadu_si128((const __m128i *)(sy0 + 2 * dx + 16));
				__m128i y1_a = _mm_loadu_si128((const __m128i *)(sy1 + 2 * dx));
				__m128i y1_b = _mm_loadu_si128((const __m128i *)(sy1 + 2 * dx + 16));

				__m128i y_lo16 = avg_2x2_epu8_to_epi16(y0_a, y1_a, mask_00ff, two);
				__m128i y_hi16 = avg_2x2_epu8_to_epi16(y0_b, y1_b, mask_00ff, two);
				__m128i y_out = _mm_packus_epi16(y_lo16, y_hi16);

				__m128i u_16 = avg_2x2_epu8_to_epi16(_mm_loadu_si128((const __m128i *)(su0 + dx)),
								     _mm_loadu_si128((const __m128i *)(su1 + dx)),
								     mask_00ff, two);
				__m128i v_16 = avg_2x2_epu8_to_epi16(_mm_loadu_si128((const __m128i *)(sv0 + dx)),
								     _mm_loadu_si128((const __m128i *)(sv1 + dx)),
								     mask_00ff, two);
				__m128i uv_out = _mm_or_si128(u_16, _mm_slli_epi16(v_16, 8));

				_mm_storeu_si128((__m128i *)(d + 2 * dx), _mm_unpacklo_epi8(uv_out, y_out));
				_mm_storeu_si128((__m128i *)(d + 2 * dx + 16), _mm_unpackhi_epi8(uv_out, y_out));
			}
		}
#else
		(void)use_simd;
#endif
		for (; dx < dst_w; dx += 2) {
			const uint32_t y0 = ((uint32_t)sy0[2 * dx + 0] + sy0[2 * dx + 1] + sy1[2 * dx + 0] +
					     sy1[2 * dx + 1] + 2U) >>
					    2;
			const uint32_t y1 = ((uint32_t)sy0[2 * dx + 2] + sy0[2 * dx + 3] + sy1[2 * dx + 2] +
					     sy1[2 * dx + 3] + 2U) >>
					    2;
			const uint32_t u = ((uint32_t)su0[dx] + su0[dx + 1] + su1[dx] + su1[dx + 1] + 2U) >> 2;
			const uint32_t v = ((uint32_t)sv0[dx] + sv0[dx + 1] + sv1[dx] + sv1[dx + 1] + 2U) >> 2;

			d[2 * dx + 0] = (uint8_t)u;
			d[2 * dx + 1] = (uint8_t)y0;
			d[2 * dx + 2] = (uint8_t)v;
			d[2 * dx + 3] = (uint8_t)y1;
		}
	}
}

static bool convert_frame_impl(struct obs_source_frame *dst, const struct obs_source_frame *src, bool use_simd)
{
	if (!dst || !src || !dst->data[0] || !src->data[0])
		return false;

	replay_copy_frame_metadata(dst, src);

	const bool same_size = (dst->width == src->width && dst->height == src->height);
	const bool half_size = (dst->width * 2 == src->width && dst->height * 2 == src->height &&
				(dst->width & 1U) == 0 && (dst->height & 1U) == 0);

	if (dst->format == VIDEO_FORMAT_NV12 && same_size) {
		bool converted = true;
		switch (src->format) {
		case VIDEO_FORMAT_NV12:
			convert_nv12_to_nv12_1x(dst, src);
			return true;
		case VIDEO_FORMAT_I420:
			convert_i420_to_nv12_1x_impl(dst, src, use_simd);
			break;
		case VIDEO_FORMAT_I422:
			convert_i422_to_nv12_1x_impl(dst, src, use_simd);
			break;
		case VIDEO_FORMAT_UYVY:
			convert_uyvy_to_nv12_1x_impl(dst, src, use_simd);
			break;
		case VIDEO_FORMAT_YUY2:
			convert_yuy2_to_nv12_1x_impl(dst, src, use_simd);
			break;
		default:
			converted = false;
			break;
		}
		if (converted) {
#if REPLAY_HAVE_SSE2
			/* order the streaming stores before the frame is published to other threads */
			if (use_simd)
				_mm_sfence();
#endif
			return true;
		}
	} else if (dst->format == VIDEO_FORMAT_NV12 && half_size) {
		switch (src->format) {
		case VIDEO_FORMAT_NV12:
			convert_nv12_to_nv12_2x_impl(dst, src, use_simd);
			return true;
		case VIDEO_FORMAT_I420:
			convert_i420_to_nv12_2x_impl(dst, src, use_simd);
			return true;
		case VIDEO_FORMAT_I422:
			convert_i422_to_nv12_2x_impl(dst, src, use_simd);
			return true;
		case VIDEO_FORMAT_UYVY:
			convert_packed422_to_nv12_2x_impl(dst, src, true, use_simd);
			return true;
		case VIDEO_FORMAT_YUY2:
			convert_packed422_to_nv12_2x_impl(dst, src, false, use_simd);
			return true;
		default:
			break;
		}
	} else if (dst->format == VIDEO_FORMAT_UYVY && same_size && (dst->width & 1U) == 0) {
		bool converted = true;
		switch (src->format) {
		case VIDEO_FORMAT_UYVY:
			convert_uyvy_to_uyvy_1x_impl(dst, src, use_simd);
			break;
		case VIDEO_FORMAT_YUY2:
			convert_yuy2_to_uyvy_1x_impl(dst, src, use_simd);
			break;
		case VIDEO_FORMAT_I422:
			convert_i422_to_uyvy_1x_impl(dst, src, use_simd);
			break;
		default:
			converted = false;
			break;
		}
		if (converted) {
#if REPLAY_HAVE_SSE2
			if (use_simd)
				_mm_sfence();
#endif
			return true;
		}
	} else if (dst->format == VIDEO_FORMAT_UYVY && half_size) {
		switch (src->format) {
		case VIDEO_FORMAT_UYVY:
			convert_packed422_to_uyvy_2x_impl(dst, src, true, use_simd);
			return true;
		case VIDEO_FORMAT_YUY2:
			convert_packed422_to_uyvy_2x_impl(dst, src, false, use_simd);
			return true;
		case VIDEO_FORMAT_I422:
			convert_i422_to_uyvy_2x_impl(dst, src, use_simd);
			return true;
		default:
			break;
		}
	}

	if (dst->format == src->format && same_size) {
		obs_source_frame_copy(dst, src);
		replay_copy_frame_metadata(dst, src);
		return true;
	}

	return false;
}

bool replay_convert_frame_scalar(struct obs_source_frame *dst, const struct obs_source_frame *src)
{
	return convert_frame_impl(dst, src, false);
}

bool replay_convert_frame_simd(struct obs_source_frame *dst, const struct obs_source_frame *src)
{
	return convert_frame_impl(dst, src, true);
}

bool replay_convert_frame(struct obs_source_frame *dst, const struct obs_source_frame *src)
{
	return convert_frame_impl(dst, src, true);
}

bool replay_half_preview_supported(const struct obs_source_frame *src)
{
	return src && src->width >= REPLAY_HALF_PREVIEW_MIN_WIDTH && src->height >= 4 &&
	       (src->width & 3U) == 0 && (src->height & 3U) == 0 && is_nv12_convertible(src->format);
}

bool replay_convert_half_preview(struct obs_source_frame **scratch, const struct obs_source_frame *src)
{
	if (!scratch || !replay_half_preview_supported(src))
		return false;
	const uint32_t w = src->width / 2;
	const uint32_t h = src->height / 2;
	struct obs_source_frame *dst = *scratch;
	if (!dst || dst->format != VIDEO_FORMAT_NV12 || dst->width != w || dst->height != h) {
		if (dst)
			obs_source_frame_destroy(dst);
		dst = obs_source_frame_create(VIDEO_FORMAT_NV12, w, h);
		*scratch = dst;
	}
	return dst && convert_frame_impl(dst, src, true);
}

void replay_half_preview_free(struct obs_source_frame **scratch)
{
	if (scratch && *scratch) {
		obs_source_frame_destroy(*scratch);
		*scratch = NULL;
	}
}
