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

	if (src->format == VIDEO_FORMAT_NV12 || src->format == VIDEO_FORMAT_I420) {
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

static int test_1x_conversions_and_widths(void)
{
	const enum video_format formats[] = {
		VIDEO_FORMAT_UYVY,
		VIDEO_FORMAT_YUY2,
		VIDEO_FORMAT_I420,
		VIDEO_FORMAT_NV12,
	};
	/* Includes widths not multiple of 16: 2, 6, 18, 34, 66, 100, and standard 1920 */
	const uint32_t widths[] = {2, 6, 18, 34, 66, 100, 1920};
	const uint32_t heights[] = {2, 6, 10, 18, 18, 24, 32};

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
		VIDEO_FORMAT_NV12,
	};
	/* Source widths where dst_w = src_w/2 includes non-multiples of 16 (6, 18, 34) and 4K->1080p */
	const uint32_t src_widths[] = {12, 36, 68, 3840};
	const uint32_t src_heights[] = {8, 20, 28, 16};

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
	replay_convert_target_format(VIDEO_FORMAT_UYVY, 3840, 2160, true, true, &tfmt, &tw, &th);
	CHECK(tfmt == VIDEO_FORMAT_NV12 && tw == 1920 && th == 1080, "4K downscale target policy mismatch");

	replay_convert_target_format(VIDEO_FORMAT_UYVY, 1920, 1080, true, true, &tfmt, &tw, &th);
	CHECK(tfmt == VIDEO_FORMAT_NV12 && tw == 1920 && th == 1080, "1080p should not downscale under max_1080p");

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
	replay_convert_target_format(VIDEO_FORMAT_BGRA, 64, 32, true, true, &tfmt, &tw, &th);
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

static int bench_1080p_uyvy_to_nv12(void)
{
	const uint32_t w = 1920;
	const uint32_t h = 1080;
	const int iterations = 500;

	struct replay_pool *src_pool = replay_pool_create(VIDEO_FORMAT_UYVY, w, h, 1);
	struct replay_pool *dst_pool = replay_pool_create(VIDEO_FORMAT_NV12, w, h, 4);
	CHECK(src_pool && dst_pool, "bench pool create failed");
	replay_pool_wait_warmup(src_pool);
	replay_pool_wait_warmup(dst_pool);

	struct obs_source_frame *src = replay_pool_acquire(src_pool);
	fill_deterministic(src, 0x13572468U);

	/* Warm up cache */
	for (int i = 0; i < 10; i++) {
		struct obs_source_frame *dst = replay_pool_acquire(dst_pool);
		replay_convert_frame(dst, src);
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

	printf("bench 1080p UYVY->NV12 (pool acquire + convert + release, %d frames): avg=%.3f ms min=%.3f ms max=%.3f ms\n",
	       iterations, (double)sum_ns / iterations / 1e6, (double)min_ns / 1e6, (double)max_ns / 1e6);

	replay_frame_release(src);
	replay_pool_close(src_pool);
	replay_pool_close(dst_pool);
	return 0;
}

int main(void)
{
	if (test_1x_conversions_and_widths() != 0)
		return 1;
	if (test_2x_downscale() != 0)
		return 1;
	if (test_metadata_and_unsupported_formats() != 0)
		return 1;
	if (bench_1080p_uyvy_to_nv12() != 0)
		return 1;

	printf("test_convert: ALL PASSED\n");
	return 0;
}
