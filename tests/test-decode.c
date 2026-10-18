#include "replay.h"
#include "replay-pool.h"
#include "replay-convert.h"
#include "replay-decode.h"
#include <util/bmem.h>
#include <util/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int test_h264_mp4(void)
{
	const char *fixture_path = "/tmp/replay_test_h264.mp4";
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -y -v error -f lavfi -i testsrc2=size=640x360:rate=60 "
		 "-f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 "
		 "-c:v libx264 -pix_fmt yuv420p -c:a aac %s",
		 fixture_path);

	int gen_res = system(cmd);
	CHECK(gen_res == 0, "failed to generate H.264 MP4 fixture: %s", cmd);

	CHECK(replay_decode_supported(), "replay_decode_supported() returned false");

	long allocs_before = bnum_allocs();
	struct replay_decoded_clip clip;
	char err[256] = {0};
	const uint64_t base_ts = 1000000000000ULL;

	uint64_t t0 = os_gettime_ns();
	bool ok = replay_decode_file(fixture_path, base_ts, &clip, NULL, NULL, err, sizeof(err));
	uint64_t t1 = os_gettime_ns();
	double decode_ms = (double)(t1 - t0) / 1000000.0;
	printf("H.264 MP4 (2 s @ 60 fps) decode time: %.2f ms\n", decode_ms);

	CHECK(ok, "replay_decode_file failed: %s", err);
	CHECK(clip.video_frame_count == 120, "expected 120 frames, got %llu",
	      (unsigned long long)clip.video_frame_count);
	CHECK(clip.video_frames != NULL, "video_frames array is NULL");
	CHECK(clip.first_frame_timestamp == base_ts, "first_frame_timestamp expected %llu, got %llu",
	      (unsigned long long)base_ts, (unsigned long long)clip.first_frame_timestamp);
	CHECK(clip.last_frame_timestamp == clip.video_frames[119]->timestamp, "last_frame_timestamp mismatch");
	CHECK(clip.duration == clip.last_frame_timestamp - clip.first_frame_timestamp, "duration mismatch");

	/* Verify each video frame format, geometry, monotonic timestamp, and steps */
	for (uint64_t i = 0; i < clip.video_frame_count; i++) {
		struct obs_source_frame *f = clip.video_frames[i];
		CHECK(f != NULL, "frame %llu is NULL", (unsigned long long)i);
		CHECK(f->format == VIDEO_FORMAT_NV12, "frame %llu format expected NV12, got %d",
		      (unsigned long long)i, f->format);
		CHECK(f->width == 640 && f->height == 360, "frame %llu dims %ux%u != 640x360",
		      (unsigned long long)i, f->width, f->height);
		CHECK(f->data[0] != NULL && f->data[1] != NULL, "frame %llu planes NULL", (unsigned long long)i);

		if (i > 0) {
			CHECK(f->timestamp > clip.video_frames[i - 1]->timestamp,
			      "frame %llu timestamp %llu not strictly greater than prev %llu",
			      (unsigned long long)i, (unsigned long long)f->timestamp,
			      (unsigned long long)clip.video_frames[i - 1]->timestamp);
			uint64_t step_ns = f->timestamp - clip.video_frames[i - 1]->timestamp;
			CHECK(step_ns >= 15000000ULL && step_ns <= 18500000ULL,
			      "frame %llu timestamp step %llu ns out of ~16.67ms bounds",
			      (unsigned long long)i, (unsigned long long)step_ns);
		}
	}

	/* Check pixel content not all-zero; luma mean plausible */
	struct obs_source_frame *f0 = clip.video_frames[0];
	uint64_t luma_sum = 0;
	const size_t pixel_count = 640 * 360;
	for (size_t p = 0; p < pixel_count; p++) {
		luma_sum += f0->data[0][p];
	}
	double luma_mean = (double)luma_sum / (double)pixel_count;
	CHECK(luma_mean >= 20.0 && luma_mean <= 235.0, "luma mean %f not plausible for testsrc2", luma_mean);

	/* Audio verification */
	CHECK(clip.audio_frame_count > 0, "audio_frame_count is 0");
	CHECK(clip.audio_frames != NULL, "audio_frames is NULL");
	CHECK(clip.oai.format == AUDIO_FORMAT_FLOAT_PLANAR, "oai format expected FLOAT_PLANAR");
	CHECK(clip.oai.samples_per_sec == 48000, "oai samples_per_sec expected 48000, got %u",
	      clip.oai.samples_per_sec);
	CHECK(clip.oai.speakers == SPEAKERS_MONO, "oai speakers expected MONO, got %d", clip.oai.speakers);

	uint64_t total_samples = 0;
	for (uint64_t a = 0; a < clip.audio_frame_count; a++) {
		struct obs_audio_data *pkt = &clip.audio_frames[a];
		CHECK(pkt->data[0] != NULL, "audio packet %llu plane 0 is NULL", (unsigned long long)a);
		CHECK(pkt->frames > 0, "audio packet %llu has 0 frames", (unsigned long long)a);
		total_samples += pkt->frames;
		if (a > 0) {
			CHECK(pkt->timestamp >= clip.audio_frames[a - 1].timestamp,
			      "audio packet %llu timestamp not monotonic", (unsigned long long)a);
		}
	}
	uint64_t expected_samples = 2ULL * 48000ULL;
	uint64_t sample_diff = (total_samples > expected_samples) ? (total_samples - expected_samples)
								  : (expected_samples - total_samples);
	CHECK(sample_diff <= 1024, "audio sample total %llu not within 1 packet (1024) of %llu",
	      (unsigned long long)total_samples, (unsigned long long)expected_samples);

	replay_decoded_clip_free(&clip);
	CHECK(clip.video_frames == NULL && clip.audio_frames == NULL, "clip pointers not zeroed after free");
	CHECK(clip.video_frame_count == 0 && clip.audio_frame_count == 0, "clip counts not zeroed after free");

	long allocs_after = bnum_allocs();
	CHECK(allocs_after == allocs_before, "memory leak in H.264 decode: before=%ld, after=%ld",
	      allocs_before, allocs_after);

	unlink(fixture_path);
	return 0;
}

static int test_hevc_mkv(void)
{
	const char *check_path = "/tmp/replay_test_check_x265.mkv";
	char check_cmd[512];
	snprintf(check_cmd, sizeof(check_cmd),
		 "ffmpeg -y -v error -f lavfi -i testsrc2=size=640x360:rate=60 -t 0.1 -c:v libx265 %s >/dev/null 2>&1",
		 check_path);
	if (system(check_cmd) != 0) {
		printf("HEVC MKV: libx265 not available in ffmpeg CLI, skipping\n");
		unlink(check_path);
		return 0;
	}
	unlink(check_path);

	const char *fixture_path = "/tmp/replay_test_hevc.mkv";
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -y -v error -f lavfi -i testsrc2=size=640x360:rate=60 "
		 "-f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 "
		 "-c:v libx265 -pix_fmt yuv420p -c:a aac %s",
		 fixture_path);

	int gen_res = system(cmd);
	CHECK(gen_res == 0, "failed to generate HEVC MKV fixture: %s", cmd);

	long allocs_before = bnum_allocs();
	struct replay_decoded_clip clip;
	char err[256] = {0};
	const uint64_t base_ts = 2000000000000ULL;

	uint64_t t0 = os_gettime_ns();
	bool ok = replay_decode_file(fixture_path, base_ts, &clip, NULL, NULL, err, sizeof(err));
	uint64_t t1 = os_gettime_ns();
	double decode_ms = (double)(t1 - t0) / 1000000.0;
	printf("HEVC MKV (2 s @ 60 fps) decode time: %.2f ms\n", decode_ms);

	CHECK(ok, "replay_decode_file failed for HEVC MKV: %s", err);
	CHECK(clip.video_frame_count == 120, "expected 120 frames, got %llu",
	      (unsigned long long)clip.video_frame_count);
	CHECK(clip.first_frame_timestamp == base_ts, "first_frame_timestamp mismatch");

	/* Verify frames */
	for (uint64_t i = 0; i < clip.video_frame_count; i++) {
		struct obs_source_frame *f = clip.video_frames[i];
		CHECK(f->format == VIDEO_FORMAT_NV12, "frame %llu not NV12", (unsigned long long)i);
		CHECK(f->width == 640 && f->height == 360, "dims mismatch");
		if (i > 0) {
			CHECK(f->timestamp > clip.video_frames[i - 1]->timestamp,
			      "HEVC timestamp not strictly monotonic");
		}
	}

	/* Luma check */
	struct obs_source_frame *f0 = clip.video_frames[0];
	uint64_t luma_sum = 0;
	const size_t pixel_count = 640 * 360;
	for (size_t p = 0; p < pixel_count; p++) {
		luma_sum += f0->data[0][p];
	}
	double luma_mean = (double)luma_sum / (double)pixel_count;
	CHECK(luma_mean >= 20.0 && luma_mean <= 235.0, "HEVC luma mean %f not plausible", luma_mean);

	/* Audio sample count */
	CHECK(clip.audio_frame_count > 0, "HEVC audio frames 0");
	uint64_t total_samples = 0;
	for (uint64_t a = 0; a < clip.audio_frame_count; a++) {
		total_samples += clip.audio_frames[a].frames;
	}
	uint64_t expected_samples = 2ULL * 48000ULL;
	uint64_t sample_diff = (total_samples > expected_samples) ? (total_samples - expected_samples)
								  : (expected_samples - total_samples);
	CHECK(sample_diff <= 1024, "HEVC audio sample total %llu not within 1024 of %llu",
	      (unsigned long long)total_samples, (unsigned long long)expected_samples);

	replay_decoded_clip_free(&clip);
	long allocs_after = bnum_allocs();
	CHECK(allocs_after == allocs_before, "memory leak in HEVC decode: before=%ld, after=%ld",
	      allocs_before, allocs_after);

	unlink(fixture_path);
	return 0;
}

static int test_offset_mp4(void)
{
	const char *fixture_path = "/tmp/replay_test_offset.mp4";
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -y -v error -f lavfi -i testsrc2=size=640x360:rate=60 "
		 "-itsoffset 0.1 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 "
		 "-c:v libx264 -pix_fmt yuv420p -c:a aac %s",
		 fixture_path);

	int gen_res = system(cmd);
	CHECK(gen_res == 0, "failed to generate offset MP4 fixture: %s", cmd);

	long allocs_before = bnum_allocs();
	struct replay_decoded_clip clip;
	char err[256] = {0};
	const uint64_t base_ts = 5000000000000ULL;

	uint64_t t0 = os_gettime_ns();
	bool ok = replay_decode_file(fixture_path, base_ts, &clip, NULL, NULL, err, sizeof(err));
	uint64_t t1 = os_gettime_ns();
	double decode_ms = (double)(t1 - t0) / 1000000.0;
	printf("Offset MP4 (2 s @ 60 fps, +100 ms audio) decode time: %.2f ms\n", decode_ms);

	CHECK(ok, "replay_decode_file failed for offset MP4: %s", err);
	CHECK(clip.video_frame_count == 120, "expected 120 video frames");
	CHECK(clip.audio_frame_count > 0, "expected audio frames");

	/* Audio first timestamp offset ~100 ms */
	uint64_t first_audio_ts = clip.audio_frames[0].timestamp;
	CHECK(first_audio_ts >= base_ts, "audio timestamp %llu before base_ts %llu",
	      (unsigned long long)first_audio_ts, (unsigned long long)base_ts);

	uint64_t offset_ns = first_audio_ts - base_ts;
	uint64_t target_offset_ns = 100ULL * 1000000ULL;
	uint64_t diff_ns = (offset_ns > target_offset_ns) ? (offset_ns - target_offset_ns)
							  : (target_offset_ns - offset_ns);
	printf("First audio timestamp offset: %.2f ms (target ~100 ms, diff %.2f ms)\n",
	       (double)offset_ns / 1000000.0, (double)diff_ns / 1000000.0);
	/* AAC packet duration is ~21.33 ms, so ~78 ms to ~122 ms is expected */
	CHECK(diff_ns <= 35000000ULL,
	      "audio first timestamp offset %llu ns not within 35 ms of 100 ms",
	      (unsigned long long)offset_ns);

	replay_decoded_clip_free(&clip);
	long allocs_after = bnum_allocs();
	CHECK(allocs_after == allocs_before, "memory leak in offset MP4 decode: before=%ld, after=%ld",
	      allocs_before, allocs_after);

	unlink(fixture_path);
	return 0;
}

static int test_corrupt_and_missing_file(void)
{
	/* Missing file */
	struct replay_decoded_clip clip;
	char err[256] = {0};
	long allocs_before = bnum_allocs();

	bool ok = replay_decode_file("/tmp/non_existent_file_xyz_12345.mp4", 1000, &clip, NULL, NULL,
				     err, sizeof(err));
	CHECK(!ok, "expected failure for non-existent file");
	CHECK(err[0] != '\0', "expected error message for non-existent file");
	CHECK(clip.video_frames == NULL && clip.audio_frames == NULL, "clip not zeroed on failure");
	CHECK(bnum_allocs() == allocs_before, "memory leak on missing file");

	/* Corrupt file with garbage bytes */
	const char *corrupt_path = "/tmp/replay_test_corrupt.bin";
	FILE *fp = fopen(corrupt_path, "wb");
	CHECK(fp != NULL, "failed to create corrupt test file");
	uint8_t garbage[512];
	memset(garbage, 0xA5, sizeof(garbage));
	fwrite(garbage, 1, sizeof(garbage), fp);
	fclose(fp);

	memset(err, 0, sizeof(err));
	ok = replay_decode_file(corrupt_path, 1000, &clip, NULL, NULL, err, sizeof(err));
	CHECK(!ok, "expected failure for corrupt file");
	CHECK(err[0] != '\0', "expected error message for corrupt file");
	CHECK(clip.video_frames == NULL && clip.audio_frames == NULL, "clip not zeroed on corrupt failure");
	CHECK(bnum_allocs() == allocs_before, "memory leak on corrupt file");

	unlink(corrupt_path);
	return 0;
}

static bool abort_immediately(void *param)
{
	UNUSED_PARAMETER(param);
	return true;
}

static int test_abort_callback(void)
{
	const char *fixture_path = "/tmp/replay_test_abort.mp4";
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -y -v error -f lavfi -i testsrc2=size=640x360:rate=60 -t 1 "
		 "-c:v libx264 -pix_fmt yuv420p %s",
		 fixture_path);

	int gen_res = system(cmd);
	CHECK(gen_res == 0, "failed to generate abort test fixture");

	struct replay_decoded_clip clip;
	char err[256] = {0};
	long allocs_before = bnum_allocs();

	bool ok = replay_decode_file(fixture_path, 1000, &clip, abort_immediately, NULL, err, sizeof(err));
	CHECK(!ok, "expected abort to return false");
	CHECK(strcmp(err, "aborted") == 0, "expected 'aborted' error message, got: %s", err);
	CHECK(clip.video_frames == NULL, "video frames not zeroed on abort");
	CHECK(bnum_allocs() == allocs_before, "memory leak on abort");

	unlink(fixture_path);
	return 0;
}

int main(void)
{
	if (test_h264_mp4() != 0)
		return 1;
	if (test_hevc_mkv() != 0)
		return 1;
	if (test_offset_mp4() != 0)
		return 1;
	if (test_corrupt_and_missing_file() != 0)
		return 1;
	if (test_abort_callback() != 0)
		return 1;

	printf("test-decode: ALL PASSED\n");
	return 0;
}
