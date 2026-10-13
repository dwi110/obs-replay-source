#include "replay.h"
#include "replay-pool.h"
#include "replay-convert.h"
#include <util/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal stubs for symbols referenced by replay.c */
struct obs_source_info replay_source_info = {.id = REPLAY_SOURCE_ID, .type = OBS_SOURCE_TYPE_INPUT};
struct obs_source_info replay_filter_info = {.id = REPLAY_FILTER_ID, .type = OBS_SOURCE_TYPE_FILTER};
struct obs_source_info replay_filter_audio_info = {.id = REPLAY_FILTER_AUDIO_ID, .type = OBS_SOURCE_TYPE_FILTER};
struct obs_source_info replay_filter_async_info = {.id = REPLAY_FILTER_ASYNC_ID, .type = OBS_SOURCE_TYPE_FILTER};
void replay_trigger_threshold(void *data)
{
	UNUSED_PARAMETER(data);
}

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			return 1;                                              \
		}                                                              \
	} while (0)

static uint32_t prng_next(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void fill_deterministic(struct obs_source_frame *f, uint32_t seed)
{
	size_t bytes = replay_frame_byte_size(f->format, f->width, f->height);
	uint32_t st = seed ? seed : 0x12345678U;
	for (size_t i = 0; i < bytes; i++) {
		f->data[0][i] = (uint8_t)(prng_next(&st) & 0xFFU);
	}
}

/* Straightforward independent reference implementation for 1x -> NV12 */
static void ref_convert_1x_to_nv12(struct obs_source_frame *ref, const struct obs_source_frame *src)
{
	const uint32_t w = src->width;
	const uint32_t h = src->height;

	if (src->format == VIDEO_FORMAT_NV12) {
		for (uint32_t y = 0; y < h; y++)
			memcpy(ref->data[0] + y * ref->linesize[0], src->data[0] + y * src->linesize[0], w);
		for (uint32_t cy = 0; cy < h / 2; cy++)
			memcpy(ref->data[1] + cy * ref->linesize[1], src->data[1] + cy * src->linesize[1], w);
	} else if (src->format == VIDEO_FORMAT_I420) {
		for (uint32_t y = 0; y < h; y++)
			memcpy(ref->data[0] + y * ref->linesize[0], src->data[0] + y * src->linesize[0], w);
		for (uint32_t cy = 0; cy < h / 2; cy++) {
			const uint8_t *u = src->data[1] + cy * src->linesize[1];
			const uint8_t *v = src->data[2] + cy * src->linesize[2];
			uint8_t *uv = ref->data[1] + cy * ref->linesize[1];
			for (uint32_t cx = 0; cx < w / 2; cx++) {
				uv[2 * cx + 0] = u[cx];
				uv[2 * cx + 1] = v[cx];
			}
		}
	} else if (src->format == VIDEO_FORMAT_I422) {
		for (uint32_t y = 0; y < h; y++)
			memcpy(ref->data[0] + y * ref->linesize[0], src->data[0] + y * src->linesize[0], w);
		for (uint32_t cy = 0; cy < h / 2; cy++) {
			const uint8_t *u0 = src->data[1] + (2 * cy) * src->linesize[1];
			const uint8_t *u1 = src->data[1] + (2 * cy + 1) * src->linesize[1];
			const uint8_t *v0 = src->data[2] + (2 * cy) * src->linesize[2];
			const uint8_t *v1 = src->data[2] + (2 * cy + 1) * src->linesize[2];
			uint8_t *uv = ref->data[1] + cy * ref->linesize[1];
			for (uint32_t cx = 0; cx < w / 2; cx++) {
				uv[2 * cx + 0] = (uint8_t)(((uint32_t)u0[cx] + u1[cx] + 1U) / 2U);
				uv[2 * cx + 1] = (uint8_t)(((uint32_t)v0[cx] + v1[cx] + 1U) / 2U);
			}
		}
	} else if (src->format == VIDEO_FORMAT_UYVY || src->format == VIDEO_FORMAT_YUY2) {
		const bool uyvy = (src->format == VIDEO_FORMAT_UYVY);
		const uint32_t y0_i = uyvy ? 1 : 0;
		const uint32_t y1_i = uyvy ? 3 : 2;
		const uint32_t u_i = uyvy ? 0 : 1;
		const uint32_t v_i = uyvy ? 2 : 3;

		for (uint32_t y = 0; y < h; y += 2) {
			const uint8_t *s0 = src->data[0] + y * src->linesize[0];
			const uint8_t *s1 = src->data[0] + (y + 1) * src->linesize[0];
			uint8_t *dy0 = ref->data[0] + y * ref->linesize[0];
			uint8_t *dy1 = ref->data[0] + (y + 1) * ref->linesize[0];
			uint8_t *duv = ref->data[1] + (y / 2) * ref->linesize[1];

			for (uint32_t x = 0; x < w; x += 2) {
				dy0[x + 0] = s0[2 * x + y0_i];
				dy0[x + 1] = s0[2 * x + y1_i];
				dy1[x + 0] = s1[2 * x + y0_i];
				dy1[x + 1] = s1[2 * x + y1_i];
				duv[x + 0] = (uint8_t)(((uint32_t)s0[2 * x + u_i] + s1[2 * x + u_i] + 1U) / 2U);
				duv[x + 1] = (uint8_t)(((uint32_t)s0[2 * x + v_i] + s1[2 * x + v_i] + 1U) / 2U);
			}
		}
	}
}

/* Straightforward independent reference implementation for 2x downscale -> NV12 */
static void ref_convert_2x_to_nv12(struct obs_source_frame *ref, const struct obs_source_frame *src)
{
	const uint32_t dst_w = ref->width;
	const uint32_t dst_h = ref->height;

	if (src->format == VIDEO_FORMAT_NV12 || src->format == VIDEO_FORMAT_I420 || src->format == VIDEO_FORMAT_I422) {
		for (uint32_t dy = 0; dy < dst_h; dy++) {
			const uint8_t *s0 = src->data[0] + (2 * dy) * src->linesize[0];
			const uint8_t *s1 = src->data[0] + (2 * dy + 1) * src->linesize[0];
			uint8_t *dy_row = ref->data[0] + dy * ref->linesize[0];
			for (uint32_t dx = 0; dx < dst_w; dx++) {
				uint32_t sum = (uint32_t)s0[2 * dx] + s0[2 * dx + 1] + s1[2 * dx] + s1[2 * dx + 1];
				dy_row[dx] = (uint8_t)((sum + 2U) / 4U);
			}
		}
		for (uint32_t cy = 0; cy < dst_h / 2; cy++) {
			uint8_t *duv = ref->data[1] + cy * ref->linesize[1];
			if (src->format == VIDEO_FORMAT_I420) {
				const uint8_t *u0 = src->data[1] + (2 * cy) * src->linesize[1];
				const uint8_t *u1 = src->data[1] + (2 * cy + 1) * src->linesize[1];
				const uint8_t *v0 = src->data[2] + (2 * cy) * src->linesize[2];
				const uint8_t *v1 = src->data[2] + (2 * cy + 1) * src->linesize[2];
				for (uint32_t cx = 0; cx < dst_w / 2; cx++) {
					uint32_t us = (uint32_t)u0[2 * cx] + u0[2 * cx + 1] + u1[2 * cx] + u1[2 * cx + 1];
					uint32_t vs = (uint32_t)v0[2 * cx] + v0[2 * cx + 1] + v1[2 * cx] + v1[2 * cx + 1];
					duv[2 * cx + 0] = (uint8_t)((us + 2U) / 4U);
					duv[2 * cx + 1] = (uint8_t)((vs + 2U) / 4U);
				}
			} else if (src->format == VIDEO_FORMAT_I422) {
				/* I422 -> NV12 also collapses 4 rows of 4:2:2 chroma into one NV12 row. */
				const uint8_t *u0 = src->data[1] + (4 * cy + 0) * src->linesize[1];
				const uint8_t *u1 = src->data[1] + (4 * cy + 1) * src->linesize[1];
				const uint8_t *u2 = src->data[1] + (4 * cy + 2) * src->linesize[1];
				const uint8_t *u3 = src->data[1] + (4 * cy + 3) * src->linesize[1];
				const uint8_t *v0 = src->data[2] + (4 * cy + 0) * src->linesize[2];
				const uint8_t *v1 = src->data[2] + (4 * cy + 1) * src->linesize[2];
				const uint8_t *v2 = src->data[2] + (4 * cy + 2) * src->linesize[2];
				const uint8_t *v3 = src->data[2] + (4 * cy + 3) * src->linesize[2];
				for (uint32_t cx = 0; cx < dst_w / 2; cx++) {
					uint32_t us = (uint32_t)u0[2 * cx] + u0[2 * cx + 1] + u1[2 * cx] + u1[2 * cx + 1] +
						      u2[2 * cx] + u2[2 * cx + 1] + u3[2 * cx] + u3[2 * cx + 1];
					uint32_t vs = (uint32_t)v0[2 * cx] + v0[2 * cx + 1] + v1[2 * cx] + v1[2 * cx + 1] +
						      v2[2 * cx] + v2[2 * cx + 1] + v3[2 * cx] + v3[2 * cx + 1];
					duv[2 * cx + 0] = (uint8_t)((us + 4U) / 8U);
					duv[2 * cx + 1] = (uint8_t)((vs + 4U) / 8U);
				}
			} else {
				const uint8_t *suv0 = src->data[1] + (2 * cy) * src->linesize[1];
				const uint8_t *suv1 = src->data[1] + (2 * cy + 1) * src->linesize[1];
				for (uint32_t cx = 0; cx < dst_w / 2; cx++) {
					uint32_t us = (uint32_t)suv0[4 * cx + 0] + suv0[4 * cx + 2] + suv1[4 * cx + 0] +
						      suv1[4 * cx + 2];
					uint32_t vs = (uint32_t)suv0[4 * cx + 1] + suv0[4 * cx + 3] + suv1[4 * cx + 1] +
						      suv1[4 * cx + 3];
					duv[2 * cx + 0] = (uint8_t)((us + 2U) / 4U);
					duv[2 * cx + 1] = (uint8_t)((vs + 2U) / 4U);
				}
			}
		}
	} else if (src->format == VIDEO_FORMAT_UYVY || src->format == VIDEO_FORMAT_YUY2) {
		const bool uyvy = (src->format == VIDEO_FORMAT_UYVY);
		const uint32_t y0_i = uyvy ? 1 : 0;
		const uint32_t y1_i = uyvy ? 3 : 2;
		const uint32_t u_i = uyvy ? 0 : 1;
		const uint32_t v_i = uyvy ? 2 : 3;

		for (uint32_t dy = 0; dy < dst_h; dy++) {
			const uint8_t *s0 = src->data[0] + (2 * dy) * src->linesize[0];
			const uint8_t *s1 = src->data[0] + (2 * dy + 1) * src->linesize[0];
			uint8_t *dy_row = ref->data[0] + dy * ref->linesize[0];
			for (uint32_t dx = 0; dx < dst_w; dx++) {
				uint32_t sum = (uint32_t)s0[4 * dx + y0_i] + s0[4 * dx + y1_i] + s1[4 * dx + y0_i] +
					       s1[4 * dx + y1_i];
				dy_row[dx] = (uint8_t)((sum + 2U) / 4U);
			}
		}
		for (uint32_t cy = 0; cy < dst_h / 2; cy++) {
			const uint8_t *s0 = src->data[0] + (4 * cy + 0) * src->linesize[0];
			const uint8_t *s1 = src->data[0] + (4 * cy + 1) * src->linesize[0];
			const uint8_t *s2 = src->data[0] + (4 * cy + 2) * src->linesize[0];
			const uint8_t *s3 = src->data[0] + (4 * cy + 3) * src->linesize[0];
			uint8_t *duv = ref->data[1] + cy * ref->linesize[1];
			for (uint32_t cx = 0; cx < dst_w / 2; cx++) {
				const uint32_t base = 8 * cx;
				uint32_t us = (uint32_t)s0[base + u_i] + s0[base + 4 + u_i] + s1[base + u_i] +
					      s1[base + 4 + u_i] + s2[base + u_i] + s2[base + 4 + u_i] +
					      s3[base + u_i] + s3[base + 4 + u_i];
				uint32_t vs = (uint32_t)s0[base + v_i] + s0[base + 4 + v_i] + s1[base + v_i] +
					      s1[base + 4 + v_i] + s2[base + v_i] + s2[base + 4 + v_i] +
					      s3[base + v_i] + s3[base + 4 + v_i];
				duv[2 * cx + 0] = (uint8_t)((us + 4U) / 8U);
				duv[2 * cx + 1] = (uint8_t)((vs + 4U) / 8U);
			}
		}
	}
}

/* Independent reference implementation for 1x -> UYVY (max 4:2:2, never upsample) */
static void ref_convert_1x_to_uyvy(struct obs_source_frame *ref, const struct obs_source_frame *src)
{
	const uint32_t w = src->width;
	const uint32_t h = src->height;
	const uint32_t row_bytes = w * 2;

	if (src->format == VIDEO_FORMAT_UYVY) {
		for (uint32_t y = 0; y < h; y++)
			memcpy(ref->data[0] + y * ref->linesize[0], src->data[0] + y * src->linesize[0], row_bytes);
	} else if (src->format == VIDEO_FORMAT_YUY2) {
		/* YUY2 (Y0 U Y1 V) -> UYVY (U Y0 V Y1): swap the two bytes of every 16-bit word. */
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *s = src->data[0] + y * src->linesize[0];
			uint8_t *d = ref->data[0] + y * ref->linesize[0];
			for (uint32_t i = 0; i < row_bytes; i += 2) {
				d[i + 0] = s[i + 1];
				d[i + 1] = s[i + 0];
			}
		}
	} else if (src->format == VIDEO_FORMAT_I422) {
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *sy = src->data[0] + y * src->linesize[0];
			const uint8_t *su = src->data[1] + y * src->linesize[1];
			const uint8_t *sv = src->data[2] + y * src->linesize[2];
			uint8_t *d = ref->data[0] + y * ref->linesize[0];
			for (uint32_t cx = 0; cx < w / 2; cx++) {
				d[4 * cx + 0] = su[cx];
				d[4 * cx + 1] = sy[2 * cx + 0];
				d[4 * cx + 2] = sv[cx];
				d[4 * cx + 3] = sy[2 * cx + 1];
			}
		}
	}
}

/* Independent reference implementation for 2x downscale -> UYVY (4:2:2 preserved) */
static void ref_convert_2x_to_uyvy(struct obs_source_frame *ref, const struct obs_source_frame *src)
{
	const uint32_t dst_w = ref->width;
	const uint32_t dst_h = ref->height;

	for (uint32_t dy = 0; dy < dst_h; dy++) {
		uint8_t *d = ref->data[0] + dy * ref->linesize[0];
		if (src->format == VIDEO_FORMAT_I422) {
			const uint8_t *sy0 = src->data[0] + (2 * dy + 0) * src->linesize[0];
			const uint8_t *sy1 = src->data[0] + (2 * dy + 1) * src->linesize[0];
			const uint8_t *su0 = src->data[1] + (2 * dy + 0) * src->linesize[1];
			const uint8_t *su1 = src->data[1] + (2 * dy + 1) * src->linesize[1];
			const uint8_t *sv0 = src->data[2] + (2 * dy + 0) * src->linesize[2];
			const uint8_t *sv1 = src->data[2] + (2 * dy + 1) * src->linesize[2];
			for (uint32_t dx = 0; dx < dst_w; dx += 2) {
				const uint32_t y0 = ((uint32_t)sy0[2 * dx + 0] + sy0[2 * dx + 1] + sy1[2 * dx + 0] +
						     sy1[2 * dx + 1] + 2U) >>
						    2;
				const uint32_t y1 = ((uint32_t)sy0[2 * dx + 2] + sy0[2 * dx + 3] + sy1[2 * dx + 2] +
						     sy1[2 * dx + 3] + 2U) >>
						    2;
				const uint32_t u =
					((uint32_t)su0[dx] + su0[dx + 1] + su1[dx] + su1[dx + 1] + 2U) >> 2;
				const uint32_t v =
					((uint32_t)sv0[dx] + sv0[dx + 1] + sv1[dx] + sv1[dx + 1] + 2U) >> 2;
				d[2 * dx + 0] = (uint8_t)u;
				d[2 * dx + 1] = (uint8_t)y0;
				d[2 * dx + 2] = (uint8_t)v;
				d[2 * dx + 3] = (uint8_t)y1;
			}
		} else {
			const bool uyvy = (src->format == VIDEO_FORMAT_UYVY);
			const uint32_t y0_i = uyvy ? 1 : 0;
			const uint32_t y1_i = uyvy ? 3 : 2;
			const uint32_t u_i = uyvy ? 0 : 1;
			const uint32_t v_i = uyvy ? 2 : 3;
			const uint8_t *s0 = src->data[0] + (2 * dy + 0) * src->linesize[0];
			const uint8_t *s1 = src->data[0] + (2 * dy + 1) * src->linesize[0];
			for (uint32_t dx = 0; dx < dst_w; dx += 2) {
				const uint8_t *p0 = s0 + 4 * dx;
				const uint8_t *p1 = s1 + 4 * dx;
				const uint32_t y0 =
					((uint32_t)p0[y0_i] + p0[y1_i] + p1[y0_i] + p1[y1_i] + 2U) >> 2;
				const uint32_t y1 =
					((uint32_t)p0[4 + y0_i] + p0[4 + y1_i] + p1[4 + y0_i] + p1[4 + y1_i] + 2U) >> 2;
				const uint32_t u =
					((uint32_t)p0[u_i] + p0[4 + u_i] + p1[u_i] + p1[4 + u_i] + 2U) >> 2;
				const uint32_t v =
					((uint32_t)p0[v_i] + p0[4 + v_i] + p1[v_i] + p1[4 + v_i] + 2U) >> 2;
				d[2 * dx + 0] = (uint8_t)u;
				d[2 * dx + 1] = (uint8_t)y0;
				d[2 * dx + 2] = (uint8_t)v;
				d[2 * dx + 3] = (uint8_t)y1;
			}
		}
	}
}

static int compare_nv12_frames(const struct obs_source_frame *a, const struct obs_source_frame *b, const char *label)
{
	CHECK(a->width == b->width && a->height == b->height, "%s: dimension mismatch", label);
	const uint32_t w = a->width;
	const uint32_t h = a->height;
	for (uint32_t y = 0; y < h; y++) {
		if (memcmp(a->data[0] + y * a->linesize[0], b->data[0] + y * b->linesize[0], w) != 0) {
			fprintf(stderr, "FAIL: %s Y plane mismatch at row %u\n", label, y);
			return 1;
		}
	}
	for (uint32_t cy = 0; cy < h / 2; cy++) {
		if (memcmp(a->data[1] + cy * a->linesize[1], b->data[1] + cy * b->linesize[1], w) != 0) {
			fprintf(stderr, "FAIL: %s UV plane mismatch at row %u\n", label, cy);
			return 1;
		}
	}
	return 0;
}

static int compare_uyvy_frames(const struct obs_source_frame *a, const struct obs_source_frame *b, const char *label)
{
	CHECK(a->width == b->width && a->height == b->height, "%s: dimension mismatch", label);
	const uint32_t row_bytes = a->width * 2;
	const uint32_t h = a->height;
	for (uint32_t y = 0; y < h; y++) {
		if (memcmp(a->data[0] + y * a->linesize[0], b->data[0] + y * b->linesize[0], row_bytes) != 0) {
			fprintf(stderr, "FAIL: %s UYVY row mismatch at row %u\n", label, y);
			return 1;
		}
	}
	return 0;
}

static int test_1x_conversions_and_widths(void)
{
	const enum video_format formats[] = {
		VIDEO_FORMAT_UYVY,
		VIDEO_FORMAT_YUY2,
		VIDEO_FORMAT_I420,
		VIDEO_FORMAT_I422,
		VIDEO_FORMAT_NV12,
	};
	/* Includes widths not multiple of 16 (2, 6, 18, 34, 66, 100, 1366) and 1080p/1920 */
	const uint32_t widths[] = {2, 6, 18, 34, 66, 100, 1366, 1920};
	const uint32_t heights[] = {2, 6, 10, 18, 18, 24, 768, 1080};

	for (size_t fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
		for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
			const enum video_format fmt = formats[fi];
			const uint32_t w = widths[wi];
			const uint32_t h = heights[wi];

			struct replay_pool *src_pool = replay_pool_create(fmt, w, h, 1);
			struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_NV12, w, h, 3);
			CHECK(src_pool && dst_pool, "pool create failed");

			struct obs_source_frame *src = replay_pool_acquire(src_pool);
			struct obs_source_frame *dst_simd = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_scalar = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_ref = replay_pool_acquire(dst_pool);

			fill_deterministic(src, (uint32_t)(0xA50000U + fi * 100 + wi));

			CHECK(replay_convert_frame_simd(dst_simd, src), "simd 1x convert failed");
			CHECK(replay_convert_frame_scalar(dst_scalar, src), "scalar 1x convert failed");
			ref_convert_1x_to_nv12(dst_ref, src);

			char label[128];
			snprintf(label, sizeof(label), "1x fmt=%d %ux%u scalar-vs-ref", (int)fmt, w, h);
			if (compare_nv12_frames(dst_scalar, dst_ref, label) != 0)
				return 1;

			snprintf(label, sizeof(label), "1x fmt=%d %ux%u simd-vs-scalar", (int)fmt, w, h);
			if (compare_nv12_frames(dst_simd, dst_scalar, label) != 0)
				return 1;

			replay_frame_release(src);
			replay_frame_release(dst_simd);
			replay_frame_release(dst_scalar);
			replay_frame_release(dst_ref);
			replay_pool_close(src_pool);
			replay_pool_close(dst_pool);
		}
	}
	return 0;
}

static int test_2x_downscale(void)
{
	const enum video_format formats[] = {
		VIDEO_FORMAT_UYVY,
		VIDEO_FORMAT_YUY2,
		VIDEO_FORMAT_I420,
		VIDEO_FORMAT_I422,
		VIDEO_FORMAT_NV12,
	};
	/* Source sizes: tiny, non-multiple-of-16 dst widths (6, 18, 34), 1366x768 and 4K */
	const uint32_t src_widths[] = {12, 36, 68, 2732, 3840};
	const uint32_t src_heights[] = {8, 20, 28, 1536, 2160};

	for (size_t fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
		for (size_t wi = 0; wi < sizeof(src_widths) / sizeof(src_widths[0]); wi++) {
			const enum video_format fmt = formats[fi];
			const uint32_t sw = src_widths[wi];
			const uint32_t sh = src_heights[wi];
			const uint32_t dw = sw / 2;
			const uint32_t dh = sh / 2;

			struct replay_pool *src_pool = replay_pool_create(fmt, sw, sh, 1);
			struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_NV12, dw, dh, 3);
			CHECK(src_pool && dst_pool, "pool create failed");

			struct obs_source_frame *src = replay_pool_acquire(src_pool);
			struct obs_source_frame *dst_simd = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_scalar = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_ref = replay_pool_acquire(dst_pool);

			fill_deterministic(src, (uint32_t)(0x5A0000U + fi * 100 + wi));

			CHECK(replay_convert_frame_simd(dst_simd, src), "simd 2x convert failed");
			CHECK(replay_convert_frame_scalar(dst_scalar, src), "scalar 2x convert failed");
			ref_convert_2x_to_nv12(dst_ref, src);

			char label[128];
			snprintf(label, sizeof(label), "2x fmt=%d %ux%u->%ux%u scalar-vs-ref", (int)fmt, sw, sh, dw, dh);
			if (compare_nv12_frames(dst_scalar, dst_ref, label) != 0)
				return 1;

			snprintf(label, sizeof(label), "2x fmt=%d %ux%u->%ux%u simd-vs-scalar", (int)fmt, sw, sh, dw,
				 dh);
			if (compare_nv12_frames(dst_simd, dst_scalar, label) != 0)
				return 1;

			replay_frame_release(src);
			replay_frame_release(dst_simd);
			replay_frame_release(dst_scalar);
			replay_frame_release(dst_ref);
			replay_pool_close(src_pool);
			replay_pool_close(dst_pool);
		}
	}

	/* Verify replay_convert_target_format policy for 3840x2160 -> 1920x1080 */
	enum video_format tfmt;
	uint32_t tw, th;
	replay_convert_target_format(VIDEO_FORMAT_UYVY, 3840, 2160, STORAGE_FORMAT_NV12, true, &tfmt, &tw, &th);
	CHECK(tfmt == VIDEO_FORMAT_NV12 && tw == 1920 && th == 1080, "4K downscale target policy mismatch");

	replay_convert_target_format(VIDEO_FORMAT_UYVY, 1920, 1080, STORAGE_FORMAT_NV12, true, &tfmt, &tw, &th);
	CHECK(tfmt == VIDEO_FORMAT_NV12 && tw == 1920 && th == 1080, "1080p should not downscale under max_1080p");

	return 0;
}

/* UYVY storage target: 4:2:2 sources must stay 4:2:2, SIMD == scalar byte-for-byte. */
static int test_uyvy_target_conversions(void)
{
	const enum video_format src_formats[] = {
		VIDEO_FORMAT_UYVY,
		VIDEO_FORMAT_YUY2,
		VIDEO_FORMAT_I422,
	};
	const uint32_t widths[] = {2, 6, 18, 34, 1366, 1920};
	const uint32_t heights[] = {2, 6, 10, 18, 768, 1080};

	for (size_t fi = 0; fi < sizeof(src_formats) / sizeof(src_formats[0]); fi++) {
		for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
			const enum video_format fmt = src_formats[fi];
			const uint32_t w = widths[wi];
			const uint32_t h = heights[wi];

			struct replay_pool *src_pool = replay_pool_create(fmt, w, h, 1);
			struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_UYVY, w, h, 3);
			CHECK(src_pool && dst_pool, "UYVY 1x pool create failed");

			struct obs_source_frame *src = replay_pool_acquire(src_pool);
			struct obs_source_frame *dst_simd = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_scalar = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_ref = replay_pool_acquire(dst_pool);

			fill_deterministic(src, (uint32_t)(0xC30000U + fi * 100 + wi));

			CHECK(replay_convert_frame_simd(dst_simd, src), "simd UYVY 1x convert failed");
			CHECK(replay_convert_frame_scalar(dst_scalar, src), "scalar UYVY 1x convert failed");
			ref_convert_1x_to_uyvy(dst_ref, src);

			char label[128];
			snprintf(label, sizeof(label), "1x->UYVY fmt=%d %ux%u scalar-vs-ref", (int)fmt, w, h);
			if (compare_uyvy_frames(dst_scalar, dst_ref, label) != 0)
				return 1;

			snprintf(label, sizeof(label), "1x->UYVY fmt=%d %ux%u simd-vs-scalar", (int)fmt, w, h);
			if (compare_uyvy_frames(dst_simd, dst_scalar, label) != 0)
				return 1;

			replay_frame_release(src);
			replay_frame_release(dst_simd);
			replay_frame_release(dst_scalar);
			replay_frame_release(dst_ref);
			replay_pool_close(src_pool);
			replay_pool_close(dst_pool);
		}
	}

	const uint32_t src_widths[] = {12, 36, 68, 2732, 3840};
	const uint32_t src_heights[] = {8, 20, 28, 1536, 2160};

	for (size_t fi = 0; fi < sizeof(src_formats) / sizeof(src_formats[0]); fi++) {
		for (size_t wi = 0; wi < sizeof(src_widths) / sizeof(src_widths[0]); wi++) {
			const enum video_format fmt = src_formats[fi];
			const uint32_t sw = src_widths[wi];
			const uint32_t sh = src_heights[wi];
			const uint32_t dw = sw / 2;
			const uint32_t dh = sh / 2;

			struct replay_pool *src_pool = replay_pool_create(fmt, sw, sh, 1);
			struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_UYVY, dw, dh, 3);
			CHECK(src_pool && dst_pool, "UYVY 2x pool create failed");

			struct obs_source_frame *src = replay_pool_acquire(src_pool);
			struct obs_source_frame *dst_simd = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_scalar = replay_pool_acquire(dst_pool);
			struct obs_source_frame *dst_ref = replay_pool_acquire(dst_pool);

			fill_deterministic(src, (uint32_t)(0xD40000U + fi * 100 + wi));

			CHECK(replay_convert_frame_simd(dst_simd, src), "simd UYVY 2x convert failed");
			CHECK(replay_convert_frame_scalar(dst_scalar, src), "scalar UYVY 2x convert failed");
			ref_convert_2x_to_uyvy(dst_ref, src);

			char label[128];
			snprintf(label, sizeof(label), "2x->UYVY fmt=%d %ux%u->%ux%u scalar-vs-ref", (int)fmt, sw, sh, dw,
				 dh);
			if (compare_uyvy_frames(dst_scalar, dst_ref, label) != 0)
				return 1;

			snprintf(label, sizeof(label), "2x->UYVY fmt=%d %ux%u->%ux%u simd-vs-scalar", (int)fmt, sw, sh, dw,
				 dh);
			if (compare_uyvy_frames(dst_simd, dst_scalar, label) != 0)
				return 1;

			replay_frame_release(src);
			replay_frame_release(dst_simd);
			replay_frame_release(dst_scalar);
			replay_frame_release(dst_ref);
			replay_pool_close(src_pool);
			replay_pool_close(dst_pool);
		}
	}
	return 0;
}

/* Hand-computed pixel values against the documented formulas. */
static int test_hand_computed_values(void)
{
	/* --- I422 -> UYVY 1x, 4x2 --- */
	{
		struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_I422, 4, 2, 1);
		struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_UYVY, 4, 2, 1);
		struct obs_source_frame *src = replay_pool_acquire(src_pool);
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);

		const uint8_t yrows[2][4] = {{10, 20, 30, 40}, {50, 60, 70, 80}};
		const uint8_t urows[2][2] = {{100, 110}, {120, 130}};
		const uint8_t vrows[2][2] = {{200, 210}, {220, 230}};
		for (uint32_t y = 0; y < 2; y++) {
			memcpy(src->data[0] + y * src->linesize[0], yrows[y], 4);
			memcpy(src->data[1] + y * src->linesize[1], urows[y], 2);
			memcpy(src->data[2] + y * src->linesize[2], vrows[y], 2);
		}

		CHECK(replay_convert_frame(dst, src), "I422->UYVY 1x failed");
		const uint8_t exp0[8] = {100, 10, 200, 20, 110, 30, 210, 40};
		const uint8_t exp1[8] = {120, 50, 220, 60, 130, 70, 230, 80};
		CHECK(memcmp(dst->data[0], exp0, 8) == 0, "I422->UYVY 1x row0 hand-computed mismatch");
		CHECK(memcmp(dst->data[0] + dst->linesize[0], exp1, 8) == 0, "I422->UYVY 1x row1 hand-computed mismatch");

		replay_frame_release(src);
		replay_frame_release(dst);
		replay_pool_close(src_pool);
		replay_pool_close(dst_pool);
	}

	/* --- I422 -> NV12 1x, 4x2 (chroma = average of the two 4:2:2 chroma rows) --- */
	{
		struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_I422, 4, 2, 1);
		struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_NV12, 4, 2, 1);
		struct obs_source_frame *src = replay_pool_acquire(src_pool);
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);

		const uint8_t yrows[2][4] = {{10, 20, 30, 40}, {50, 60, 70, 80}};
		const uint8_t urows[2][2] = {{100, 110}, {120, 130}};
		const uint8_t vrows[2][2] = {{200, 210}, {220, 230}};
		for (uint32_t y = 0; y < 2; y++) {
			memcpy(src->data[0] + y * src->linesize[0], yrows[y], 4);
			memcpy(src->data[1] + y * src->linesize[1], urows[y], 2);
			memcpy(src->data[2] + y * src->linesize[2], vrows[y], 2);
		}

		CHECK(replay_convert_frame(dst, src), "I422->NV12 1x failed");
		CHECK(memcmp(dst->data[0], yrows[0], 4) == 0, "I422->NV12 1x Y row0 mismatch");
		CHECK(memcmp(dst->data[0] + dst->linesize[0], yrows[1], 4) == 0, "I422->NV12 1x Y row1 mismatch");
		/* avgU = ((100+120+1)/2, (110+130+1)/2) = (110,120); avgV = (210,220) */
		const uint8_t exp_uv[4] = {110, 210, 120, 220};
		CHECK(memcmp(dst->data[1], exp_uv, 4) == 0, "I422->NV12 1x chroma hand-computed mismatch");

		replay_frame_release(src);
		replay_frame_release(dst);
		replay_pool_close(src_pool);
		replay_pool_close(dst_pool);
	}

	/* --- UYVY -> UYVY 2x, 4x4 -> 2x2 (documented 2x2 box filter) --- */
	{
		struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_UYVY, 4, 4, 1);
		struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_UYVY, 2, 2, 1);
		struct obs_source_frame *src = replay_pool_acquire(src_pool);
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);

		const uint8_t rows[4][8] = {
			{10, 20, 30, 40, 50, 60, 70, 80},
			{12, 22, 32, 42, 52, 62, 72, 82},
			{14, 24, 34, 44, 54, 64, 74, 84},
			{16, 26, 36, 46, 56, 66, 76, 86},
		};
		for (uint32_t y = 0; y < 4; y++)
			memcpy(src->data[0] + y * src->linesize[0], rows[y], 8);

		CHECK(replay_convert_frame(dst, src), "UYVY->UYVY 2x failed");
		/* dy=0: U=avg(10,50,12,52)=31, Y0=avg(20,40,22,42)=31, V=avg(30,70,32,72)=51, Y1=avg(60,80,62,82)=71 */
		const uint8_t exp0[4] = {31, 31, 51, 71};
		/* dy=1: U=avg(14,54,16,56)=35, Y0=avg(24,44,26,46)=35, V=avg(34,74,36,76)=55, Y1=avg(64,84,66,86)=75 */
		const uint8_t exp1[4] = {35, 35, 55, 75};
		CHECK(memcmp(dst->data[0], exp0, 4) == 0, "UYVY->UYVY 2x row0 hand-computed mismatch");
		CHECK(memcmp(dst->data[0] + dst->linesize[0], exp1, 4) == 0, "UYVY->UYVY 2x row1 hand-computed mismatch");

		replay_frame_release(src);
		replay_frame_release(dst);
		replay_pool_close(src_pool);
		replay_pool_close(dst_pool);
	}

	/* --- I422 -> UYVY 2x, 4x4 -> 2x2 --- */
	{
		struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_I422, 4, 4, 1);
		struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_UYVY, 2, 2, 1);
		struct obs_source_frame *src = replay_pool_acquire(src_pool);
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);

		const uint8_t yrows[4][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}};
		const uint8_t urows[4][2] = {{100, 110}, {120, 130}, {140, 150}, {160, 170}};
		const uint8_t vrows[4][2] = {{200, 210}, {220, 230}, {240, 250}, {230, 240}};
		for (uint32_t y = 0; y < 4; y++) {
			memcpy(src->data[0] + y * src->linesize[0], yrows[y], 4);
			memcpy(src->data[1] + y * src->linesize[1], urows[y], 2);
			memcpy(src->data[2] + y * src->linesize[2], vrows[y], 2);
		}

		CHECK(replay_convert_frame(dst, src), "I422->UYVY 2x failed");
		/* dy=0: U=avg(100,110,120,130)=115, Y0=avg(1,2,5,6)=4, V=avg(200,210,220,230)=215, Y1=avg(3,4,7,8)=6 */
		const uint8_t exp0[4] = {115, 4, 215, 6};
		/* dy=1: U=avg(140,150,160,170)=155, Y0=avg(9,10,13,14)=12, V=avg(240,250,230,240)=240, Y1=avg(11,12,15,16)=14 */
		const uint8_t exp1[4] = {155, 12, 240, 14};
		CHECK(memcmp(dst->data[0], exp0, 4) == 0, "I422->UYVY 2x row0 hand-computed mismatch");
		CHECK(memcmp(dst->data[0] + dst->linesize[0], exp1, 4) == 0, "I422->UYVY 2x row1 hand-computed mismatch");

		replay_frame_release(src);
		replay_frame_release(dst);
		replay_pool_close(src_pool);
		replay_pool_close(dst_pool);
	}

	/* --- I422 -> NV12 2x, 4x4 -> 2x2 --- */
	{
		struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_I422, 4, 4, 1);
		struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_NV12, 2, 2, 1);
		struct obs_source_frame *src = replay_pool_acquire(src_pool);
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);

		const uint8_t yrows[4][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}};
		const uint8_t urows[4][2] = {{100, 110}, {120, 130}, {140, 150}, {160, 170}};
		const uint8_t vrows[4][2] = {{200, 210}, {220, 230}, {240, 250}, {230, 240}};
		for (uint32_t y = 0; y < 4; y++) {
			memcpy(src->data[0] + y * src->linesize[0], yrows[y], 4);
			memcpy(src->data[1] + y * src->linesize[1], urows[y], 2);
			memcpy(src->data[2] + y * src->linesize[2], vrows[y], 2);
		}

		CHECK(replay_convert_frame(dst, src), "I422->NV12 2x failed");
		/* luma 2x2: row0 = {avg(1,2,5,6)=4, avg(3,4,7,8)=6}, row1 = {avg(9,10,13,14)=12, avg(11,12,15,16)=14} */
		const uint8_t exp_y0[2] = {4, 6};
		const uint8_t exp_y1[2] = {12, 14};
		CHECK(memcmp(dst->data[0], exp_y0, 2) == 0, "I422->NV12 2x Y row0 mismatch");
		CHECK(memcmp(dst->data[0] + dst->linesize[0], exp_y1, 2) == 0, "I422->NV12 2x Y row1 mismatch");
		/* chroma 2x2 box over 8 samples: U=avg(100..170)=135, V=avg(200,210,220,230,240,250,230,240)=228 */
		const uint8_t exp_uv[2] = {135, 228};
		CHECK(memcmp(dst->data[1], exp_uv, 2) == 0, "I422->NV12 2x chroma hand-computed mismatch");

		replay_frame_release(src);
		replay_frame_release(dst);
		replay_pool_close(src_pool);
		replay_pool_close(dst_pool);
	}

	/* --- YUY2 -> UYVY 1x byte swap --- */
	{
		struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_YUY2, 4, 2, 1);
		struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_UYVY, 4, 2, 1);
		struct obs_source_frame *src = replay_pool_acquire(src_pool);
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);

		const uint8_t rows[2][8] = {{10, 100, 20, 200, 30, 110, 40, 210},
					    {50, 120, 60, 220, 70, 130, 80, 230}};
		for (uint32_t y = 0; y < 2; y++)
			memcpy(src->data[0] + y * src->linesize[0], rows[y], 8);

		CHECK(replay_convert_frame(dst, src), "YUY2->UYVY 1x failed");
		const uint8_t exp0[8] = {100, 10, 200, 20, 110, 30, 210, 40};
		const uint8_t exp1[8] = {120, 50, 220, 60, 130, 70, 230, 80};
		CHECK(memcmp(dst->data[0], exp0, 8) == 0, "YUY2->UYVY 1x row0 hand-computed mismatch");
		CHECK(memcmp(dst->data[0] + dst->linesize[0], exp1, 8) == 0, "YUY2->UYVY 1x row1 hand-computed mismatch");

		replay_frame_release(src);
		replay_frame_release(dst);
		replay_pool_close(src_pool);
		replay_pool_close(dst_pool);
	}

	return 0;
}

/* Target-format decision table. */
static int test_target_format_decision_table(void)
{
	enum video_format f;
	uint32_t w, h;

	/* UYVY mode: 4:2:2 stays UYVY, 4:2:0 stays NV12, others untouched. */
	replay_convert_target_format(VIDEO_FORMAT_NV12, 1920, 1080, STORAGE_FORMAT_UYVY, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_NV12 && w == 1920 && h == 1080, "UYVY mode: NV12 input should stay NV12");
	replay_convert_target_format(VIDEO_FORMAT_I420, 1920, 1080, STORAGE_FORMAT_UYVY, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_NV12 && w == 1920 && h == 1080, "UYVY mode: I420 input should become NV12");
	replay_convert_target_format(VIDEO_FORMAT_UYVY, 1920, 1080, STORAGE_FORMAT_UYVY, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_UYVY && w == 1920 && h == 1080, "UYVY mode: UYVY input should stay UYVY");
	replay_convert_target_format(VIDEO_FORMAT_YUY2, 1920, 1080, STORAGE_FORMAT_UYVY, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_UYVY && w == 1920 && h == 1080, "UYVY mode: YUY2 input should become UYVY");
	replay_convert_target_format(VIDEO_FORMAT_I422, 1920, 1080, STORAGE_FORMAT_UYVY, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_UYVY && w == 1920 && h == 1080, "UYVY mode: I422 input should become UYVY");
	replay_convert_target_format(VIDEO_FORMAT_BGRA, 1920, 1080, STORAGE_FORMAT_UYVY, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_BGRA && w == 1920 && h == 1080, "UYVY mode: BGRA input should stay BGRA");
	/* 4K + downscale in UYVY mode */
	replay_convert_target_format(VIDEO_FORMAT_UYVY, 3840, 2160, STORAGE_FORMAT_UYVY, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_UYVY && w == 1920 && h == 1080, "UYVY mode: 4K UYVY should downscale to UYVY");
	replay_convert_target_format(VIDEO_FORMAT_I422, 3840, 2160, STORAGE_FORMAT_UYVY, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_UYVY && w == 1920 && h == 1080, "UYVY mode: 4K I422 should downscale to UYVY");
	replay_convert_target_format(VIDEO_FORMAT_NV12, 3840, 2160, STORAGE_FORMAT_UYVY, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_NV12 && w == 1920 && h == 1080, "UYVY mode: 4K NV12 should downscale to NV12");
	replay_convert_target_format(VIDEO_FORMAT_I420, 3840, 2160, STORAGE_FORMAT_UYVY, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_NV12 && w == 1920 && h == 1080, "UYVY mode: 4K I420 should downscale to NV12");
	/* 1080p UYVY must not be downscaled */
	replay_convert_target_format(VIDEO_FORMAT_UYVY, 1920, 1080, STORAGE_FORMAT_UYVY, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_UYVY && w == 1920 && h == 1080, "UYVY mode: 1080p should not downscale");

	/* NV12 mode: I422 is now convertible too. */
	replay_convert_target_format(VIDEO_FORMAT_I422, 1920, 1080, STORAGE_FORMAT_NV12, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_NV12 && w == 1920 && h == 1080, "NV12 mode: I422 input should become NV12");
	replay_convert_target_format(VIDEO_FORMAT_I422, 3840, 2160, STORAGE_FORMAT_NV12, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_NV12 && w == 1920 && h == 1080, "NV12 mode: 4K I422 should downscale to NV12");
	replay_convert_target_format(VIDEO_FORMAT_BGRA, 1920, 1080, STORAGE_FORMAT_NV12, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_BGRA && w == 1920 && h == 1080, "NV12 mode: BGRA input should stay BGRA");

	/* ORIGINAL mode: unchanged format unless a 2x downscale kicks in (today's behaviour). */
	replay_convert_target_format(VIDEO_FORMAT_UYVY, 1920, 1080, STORAGE_FORMAT_ORIGINAL, false, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_UYVY && w == 1920 && h == 1080, "ORIGINAL mode: UYVY should stay UYVY");
	replay_convert_target_format(VIDEO_FORMAT_UYVY, 3840, 2160, STORAGE_FORMAT_ORIGINAL, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_NV12 && w == 1920 && h == 1080, "ORIGINAL mode: 4K downscale should land in NV12");
	replay_convert_target_format(VIDEO_FORMAT_BGRA, 3840, 2160, STORAGE_FORMAT_ORIGINAL, true, &f, &w, &h);
	CHECK(f == VIDEO_FORMAT_BGRA && w == 3840 && h == 2160, "ORIGINAL mode: BGRA is never converted");

	return 0;
}

static int test_metadata_and_unsupported_formats(void)
{
	struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_UYVY, 64, 32, 1);
	struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_NV12, 64, 32, 1);
	struct obs_source_frame *src = replay_pool_acquire(src_pool);
	struct obs_source_frame *dst = replay_pool_acquire(dst_pool);

	src->timestamp = 9876543210ULL;
	for (int i = 0; i < 16; i++)
		src->color_matrix[i] = (float)(i + 1) * 0.25f;
	src->full_range = true;
	src->max_luminance = 1000;
	src->color_range_min[0] = 0.05f;
	src->color_range_min[1] = 0.06f;
	src->color_range_min[2] = 0.07f;
	src->color_range_max[0] = 0.95f;
	src->color_range_max[1] = 0.96f;
	src->color_range_max[2] = 0.97f;
	src->flip = true;
	src->flags = 0x5AU;
	src->trc = VIDEO_TRC_PQ;

	CHECK(replay_convert_frame(dst, src), "convert failed");
	CHECK(dst->timestamp == src->timestamp, "timestamp mismatch");
	CHECK(memcmp(dst->color_matrix, src->color_matrix, sizeof(src->color_matrix)) == 0, "color_matrix mismatch");
	CHECK(dst->full_range == true, "full_range mismatch");
	CHECK(dst->max_luminance == 1000, "max_luminance mismatch");
	CHECK(memcmp(dst->color_range_min, src->color_range_min, sizeof(src->color_range_min)) == 0,
	      "color_range_min mismatch");
	CHECK(memcmp(dst->color_range_max, src->color_range_max, sizeof(src->color_range_max)) == 0,
	      "color_range_max mismatch");
	CHECK(dst->flip == true, "flip mismatch");
	CHECK(dst->flags == 0x5AU, "flags mismatch");
	CHECK(dst->trc == VIDEO_TRC_PQ, "trc mismatch");

	replay_frame_release(src);
	replay_frame_release(dst);
	replay_pool_close(src_pool);
	replay_pool_close(dst_pool);

	/* Unsupported format (BGRA) copied unchanged into BGRA pool */
	enum video_format tfmt;
	uint32_t tw, th;
	replay_convert_target_format(VIDEO_FORMAT_BGRA, 64, 32, STORAGE_FORMAT_NV12, true, &tfmt, &tw, &th);
	CHECK(tfmt == VIDEO_FORMAT_BGRA && tw == 64 && th == 32, "BGRA should remain BGRA");

	struct replay_pool *bgra_pool = replay_pool_create(VIDEO_FORMAT_BGRA, 64, 32, 2);
	struct obs_source_frame *bgra_src = replay_pool_acquire(bgra_pool);
	struct obs_source_frame *bgra_dst = replay_pool_acquire(bgra_pool);
	fill_deterministic(bgra_src, 0xCAFEBABEU);
	CHECK(replay_convert_frame(bgra_dst, bgra_src), "BGRA copy failed");
	CHECK(memcmp(bgra_dst->data[0], bgra_src->data[0], 64 * 32 * 4) == 0, "BGRA pixel copy mismatch");

	replay_frame_release(bgra_src);
	replay_frame_release(bgra_dst);
	replay_pool_close(bgra_pool);
	return 0;
}

static int run_bench(const char *label, enum video_format src_fmt, uint32_t sw, uint32_t sh, enum video_format dst_fmt,
		     uint32_t dw, uint32_t dh, int iterations)
{
	struct replay_pool *src_pool = replay_pool_create(src_fmt, sw, sh, 1);
	struct replay_pool *dst_pool = replay_pool_create(dst_fmt, dw, dh, 4);
	CHECK(src_pool && dst_pool, "%s: bench pool create failed", label);
	replay_pool_wait_warmup(src_pool);
	replay_pool_wait_warmup(dst_pool);

	struct obs_source_frame *src = replay_pool_acquire(src_pool);
	fill_deterministic(src, 0x13572468U);

	for (int i = 0; i < 10; i++) {
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);
		CHECK(replay_convert_frame(dst, src), "%s: bench warm-up convert failed", label);
		replay_frame_release(dst);
	}

	uint64_t sum_ns = 0;
	uint64_t min_ns = UINT64_MAX;
	uint64_t max_ns = 0;
	for (int i = 0; i < iterations; i++) {
		const uint64_t t0 = os_gettime_ns();
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);
		replay_convert_frame(dst, src);
		replay_frame_release(dst);
		const uint64_t dt = os_gettime_ns() - t0;
		sum_ns += dt;
		if (dt < min_ns)
			min_ns = dt;
		if (dt > max_ns)
			max_ns = dt;
	}

	printf("bench %s (pool acquire + convert + release, %d frames): avg=%.3f ms min=%.3f ms max=%.3f ms\n", label,
	       iterations, (double)sum_ns / iterations / 1e6, (double)min_ns / 1e6, (double)max_ns / 1e6);

	replay_frame_release(src);
	replay_pool_close(src_pool);
	replay_pool_close(dst_pool);
	return 0;
}

static int bench_conversions(void)
{
	if (run_bench("1080p UYVY->NV12", VIDEO_FORMAT_UYVY, 1920, 1080, VIDEO_FORMAT_NV12, 1920, 1080, 500) != 0)
		return 1;
	if (run_bench("1080p UYVY->UYVY (copy into pool)", VIDEO_FORMAT_UYVY, 1920, 1080, VIDEO_FORMAT_UYVY, 1920,
		      1080, 500) != 0)
		return 1;
	if (run_bench("4K UYVY->UYVY 2x", VIDEO_FORMAT_UYVY, 3840, 2160, VIDEO_FORMAT_UYVY, 1920, 1080, 200) != 0)
		return 1;
	return 0;
}

int main(void)
{
	if (test_1x_conversions_and_widths() != 0)
		return 1;
	if (test_2x_downscale() != 0)
		return 1;
	if (test_uyvy_target_conversions() != 0)
		return 1;
	if (test_hand_computed_values() != 0)
		return 1;
	if (test_target_format_decision_table() != 0)
		return 1;
	if (test_metadata_and_unsupported_formats() != 0)
		return 1;
	if (bench_conversions() != 0)
		return 1;

	printf("test_convert: ALL PASSED\n");
	return 0;
}
