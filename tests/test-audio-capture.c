#include "replay-audio-capture.h"
#include "replay.h"
#include "replay-pool.h"
#include <util/platform.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#include <obs-nix-platform.h>
#endif

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

static const char *dummy_audio_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Dummy Audio Source";
}

static void *dummy_audio_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(source);
	return (void *)1;
}

static void dummy_audio_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static struct obs_source_info dummy_audio_info = {
	.id = "dummy_audio_test",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = dummy_audio_get_name,
	.create = dummy_audio_create,
	.destroy = dummy_audio_destroy,
};

static void output_test_audio(obs_source_t *src, uint64_t ts, uint32_t frames, float amp)
{
	float left[1024];
	float right[1024];
	if (frames > 1024)
		frames = 1024;
	for (uint32_t i = 0; i < frames; i++) {
		left[i] = amp;
		right[i] = amp;
	}
	const uint8_t *data[MAX_AV_PLANES] = {(const uint8_t *)left, (const uint8_t *)right};
	struct obs_source_audio audio = {
		.data = {data[0], data[1]},
		.frames = frames,
		.speakers = SPEAKERS_STEREO,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.samples_per_sec = 48000,
		.timestamp = ts,
	};
	obs_source_output_audio(src, &audio);
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("[test-audio-capture] No DISPLAY set, re-executing under xvfb-run -a...\n");
		char **new_argv = calloc((size_t)argc + 3, sizeof(char *));
		new_argv[0] = "xvfb-run";
		new_argv[1] = "-a";
		for (int i = 0; i < argc; i++)
			new_argv[i + 2] = argv[i];
		new_argv[argc + 2] = NULL;
		execvp("xvfb-run", new_argv);
		perror("execvp xvfb-run");
		return 1;
	}

	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
#endif

	printf("[test-audio-capture] Initializing headless libobs...\n");
	CHECK(obs_startup("en-US", NULL, NULL), "obs_startup failed");
	obs_add_data_path("/usr/share/obs/libobs/");

	struct obs_audio_info oai = {
		.samples_per_sec = 48000,
		.speakers = SPEAKERS_STEREO,
	};
	CHECK(obs_reset_audio(&oai), "obs_reset_audio failed");

	obs_register_source(&dummy_audio_info);

	obs_source_t *src_a = obs_source_create("dummy_audio_test", "Audio Source A", NULL, NULL);
	obs_source_t *src_b = obs_source_create("dummy_audio_test", "Audio Source B", NULL, NULL);
	CHECK(src_a && src_b, "obs_source_create failed for dummy sources");

	/* =====================================================================
	 * 1. Smoke test: configure fails cleanly for a missing source
	 * ===================================================================== */
	printf("[test-audio-capture] 1. Configure missing source...\n");
	char err[128] = "";
	bool ret = replay_audio_capture_configure("NonExistentSource", 0, 0, err, sizeof(err));
	CHECK(!ret, "configure should fail for missing source");
	CHECK(strcmp(err, "source not found") == 0, "expected 'source not found', got '%s'", err);
	CHECK(strcmp(replay_audio_capture_source_name(), "") == 0, "source name must be empty when unconfigured");

	/* =====================================================================
	 * 2. Empty source disables capture
	 * ===================================================================== */
	printf("[test-audio-capture] 2. Configure empty source disables...\n");
	ret = replay_audio_capture_configure("", 0, 0, err, sizeof(err));
	CHECK(ret, "configure with empty name should succeed (disables)");
	CHECK(strcmp(replay_audio_capture_source_name(), "") == 0, "source name must be empty");

	uint64_t oldest = 0, newest = 0;
	bool gap = false;
	CHECK(!replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage should be false when disabled");

	struct obs_audio_data *pkts = NULL;
	uint64_t pkt_count = 0;
	struct audio_convert_info out_oai;
	CHECK(!replay_audio_capture_window(0, 1000000, &pkts, &pkt_count, &out_oai),
	      "window should be false when disabled");

	/* =====================================================================
	 * 3. Packet arrival and window() with refcounted sharing
	 * ===================================================================== */
	printf("[test-audio-capture] 3. Packet arrival and window() with refcounted sharing...\n");
	ret = replay_audio_capture_configure("Audio Source A", 0, 30000000000ULL, err, sizeof(err));
	CHECK(ret, "configure Audio Source A failed: %s", err);
	CHECK(strcmp(replay_audio_capture_source_name(), "Audio Source A") == 0, "source name mismatch");

	uint64_t base_ts = os_gettime_ns();
	uint32_t frames_per_pkt = 1024;
	/* 1024 frames @ 48kHz = 21,333,333 ns */
	uint64_t pkt_dur_ns = util_mul_div64(1024, 1000000000ULL, 48000);

	for (int i = 0; i < 5; i++) {
		output_test_audio(src_a, base_ts + (uint64_t)i * pkt_dur_ns, frames_per_pkt, 0.5f);
	}

	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage should return true");
	CHECK(oldest == base_ts, "expected oldest ts %" PRIu64 ", got %" PRIu64, base_ts, oldest);
	CHECK(newest == base_ts + 5 * pkt_dur_ns, "expected newest ts %" PRIu64 ", got %" PRIu64,
	      base_ts + 5 * pkt_dur_ns, newest);
	CHECK(!gap, "continuous packets should not have gap");

	/* Query window overlapping packets 1..3 */
	uint64_t win_start = base_ts + pkt_dur_ns;
	uint64_t win_end = base_ts + 3 * pkt_dur_ns;
	ret = replay_audio_capture_window(win_start, win_end, &pkts, &pkt_count, &out_oai);
	CHECK(ret, "window() query failed");
	CHECK(pkt_count == 2, "expected 2 overlapping packets for [1*dur, 3*dur], got %" PRIu64, pkt_count);
	CHECK(pkts != NULL, "expected non-NULL packets array");
	CHECK(out_oai.samples_per_sec == 48000, "oai samples_per_sec mismatch");
	CHECK(out_oai.format == AUDIO_FORMAT_FLOAT_PLANAR, "oai format mismatch");

	/* Verify sample data and refcount behavior */
	float *left_samples = (float *)pkts[0].data[0];
	CHECK(left_samples != NULL, "packet sample plane NULL");
	CHECK(fabsf(left_samples[0] - 0.5f) < 1e-4f, "sample content mismatch, expected 0.5f, got %f",
	      left_samples[0]);

	/* Free the returned array with free_audio_packet each + bfree */
	for (uint64_t i = 0; i < pkt_count; i++)
		free_audio_packet(&pkts[i]);
	bfree(pkts);
	pkts = NULL;

	/* Buffer must be completely unaffected */
	ret = replay_audio_capture_window(win_start, win_end, &pkts, &pkt_count, NULL);
	CHECK(ret && pkt_count == 2, "buffer was affected by external packet release");
	for (uint64_t i = 0; i < pkt_count; i++)
		free_audio_packet(&pkts[i]);
	bfree(pkts);
	pkts = NULL;

	/* =====================================================================
	 * 4. History trimming to history_ns
	 * ===================================================================== */
	printf("[test-audio-capture] 4. History trimming to history_ns...\n");
	/* Set history to 300 ms */
	uint64_t test_hist = 300000000ULL;
	ret = replay_audio_capture_configure("Audio Source A", 0, test_hist, err, sizeof(err));
	CHECK(ret, "configure history 300ms failed");

	base_ts = os_gettime_ns();
	/* Push 50 packets of 20 ms each (~1000 ms total) */
	for (int i = 0; i < 50; i++) {
		output_test_audio(src_a, base_ts + (uint64_t)i * pkt_dur_ns, frames_per_pkt, 0.25f);
	}

	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed after push");
	uint64_t span = newest - oldest;
	/* The buffered span should be around test_hist (at most test_hist + 1 packet duration) */
	CHECK(span <= test_hist + pkt_dur_ns, "history not trimmed: span %" PRIu64 " ns > hist %" PRIu64 " + dur",
	      span, test_hist);
	CHECK(span >= test_hist - pkt_dur_ns, "history trimmed too aggressively: span %" PRIu64 " ns", span);

	/* =====================================================================
	 * 5. Offset applied
	 * ===================================================================== */
	printf("[test-audio-capture] 5. Offset applied...\n");
	int64_t test_offset = 200000000LL; /* +200 ms */
	ret = replay_audio_capture_configure("Audio Source A", test_offset, 30000000000ULL, err, sizeof(err));
	CHECK(ret, "configure with offset failed");

	base_ts = os_gettime_ns();
	output_test_audio(src_a, base_ts, frames_per_pkt, 0.1f);

	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed with offset");
	CHECK(oldest == (uint64_t)((int64_t)base_ts + test_offset),
	      "offset not applied to oldest: expected %" PRIu64 ", got %" PRIu64,
	      (uint64_t)((int64_t)base_ts + test_offset), oldest);

	/* Negative offset */
	int64_t neg_offset = -100000000LL; /* -100 ms */
	ret = replay_audio_capture_configure("Audio Source A", neg_offset, 30000000000ULL, err, sizeof(err));
	CHECK(ret, "configure with negative offset failed");

	base_ts = os_gettime_ns();
	output_test_audio(src_a, base_ts, frames_per_pkt, 0.1f);

	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed with negative offset");
	CHECK(oldest == (uint64_t)((int64_t)base_ts + neg_offset),
	      "negative offset not applied: expected %" PRIu64 ", got %" PRIu64,
	      (uint64_t)((int64_t)base_ts + neg_offset), oldest);

	/* =====================================================================
	 * 6. Gap detection
	 * ===================================================================== */
	printf("[test-audio-capture] 6. Gap detection...\n");
	ret = replay_audio_capture_configure("Audio Source A", 0, 30000000000ULL, err, sizeof(err));
	CHECK(ret, "configure for gap detection failed");

	base_ts = os_gettime_ns();
	/* Push packet 0 and packet 1 continuous */
	output_test_audio(src_a, base_ts, frames_per_pkt, 0.1f);
	output_test_audio(src_a, base_ts + pkt_dur_ns, frames_per_pkt, 0.1f);
	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed");
	CHECK(!gap, "continuous packets should not have gap");

	/* Push packet 2 with 1 ms gap (1,000,000 ns <= 2 ms) -> no gap */
	uint64_t small_gap_ts = base_ts + 2 * pkt_dur_ns + 1000000ULL;
	output_test_audio(src_a, small_gap_ts, frames_per_pkt, 0.1f);
	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed");
	CHECK(!gap, "1 ms gap should not trigger gap (> 2 ms required)");

	/* Push packet 3 with 50 ms gap (> 2 ms) -> gap true! */
	uint64_t big_gap_ts = small_gap_ts + pkt_dur_ns + 50000000ULL;
	output_test_audio(src_a, big_gap_ts, frames_per_pkt, 0.1f);
	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed");
	CHECK(gap, "50 ms hole should trigger gap_out == true");

	/* =====================================================================
	 * 7. Backwards-jump reset
	 * ===================================================================== */
	printf("[test-audio-capture] 7. Backwards-jump reset...\n");
	ret = replay_audio_capture_configure("Audio Source A", 0, 30000000000ULL, err, sizeof(err));
	CHECK(ret, "configure for backwards jump failed");

	base_ts = os_gettime_ns();
	output_test_audio(src_a, base_ts, frames_per_pkt, 0.1f);
	output_test_audio(src_a, base_ts + pkt_dur_ns, frames_per_pkt, 0.1f);
	output_test_audio(src_a, base_ts + 2 * pkt_dur_ns, frames_per_pkt, 0.1f);

	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed");
	CHECK(oldest == base_ts, "expected oldest ts %" PRIu64, base_ts);

	/* Backwards jump: timestamp 500 ms earlier than base_ts */
	uint64_t jump_ts = base_ts - 500000000ULL;
	output_test_audio(src_a, jump_ts, frames_per_pkt, 0.9f);

	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage failed after jump");
	CHECK(oldest == jump_ts, "history should be cleared; expected oldest %" PRIu64 ", got %" PRIu64, jump_ts,
	      oldest);
	CHECK(newest == jump_ts + pkt_dur_ns, "expected only 1 packet in buffer");

	/* =====================================================================
	 * 8. Reconfigure to another source and to ""
	 * ===================================================================== */
	printf("[test-audio-capture] 8. Reconfigure to another source and to \"\"...\n");
	ret = replay_audio_capture_configure("Audio Source B", 0, 30000000000ULL, err, sizeof(err));
	CHECK(ret, "configure to Audio Source B failed");
	CHECK(strcmp(replay_audio_capture_source_name(), "Audio Source B") == 0, "source name mismatch");

	/* Buffer was cleared on reconfigure */
	CHECK(!replay_audio_capture_coverage(&oldest, &newest, &gap), "buffer should be empty after reconfigure");

	/* Output to Source A must NOT reach the buffer */
	base_ts = os_gettime_ns();
	output_test_audio(src_a, base_ts, frames_per_pkt, 0.1f);
	CHECK(!replay_audio_capture_coverage(&oldest, &newest, &gap),
	      "output to old source reached buffer after reconfigure");

	/* Output to Source B DOES reach buffer */
	output_test_audio(src_b, base_ts, frames_per_pkt, 0.77f);
	CHECK(replay_audio_capture_coverage(&oldest, &newest, &gap), "output to Source B did not reach buffer");

	/* Reconfigure to "" */
	ret = replay_audio_capture_configure("", 0, 0, err, sizeof(err));
	CHECK(ret, "reconfigure to empty failed");
	CHECK(strcmp(replay_audio_capture_source_name(), "") == 0, "source name should be empty");

	/* Output to Source B must NOT reach buffer */
	output_test_audio(src_b, base_ts + pkt_dur_ns, frames_per_pkt, 0.77f);
	CHECK(!replay_audio_capture_coverage(&oldest, &newest, &gap), "output reached buffer when disabled");

	/* =====================================================================
	 * 9. Direct push helper and muting test
	 * ===================================================================== */
	printf("[test-audio-capture] 9. Direct push helper and muting test...\n");
	ret = replay_audio_capture_configure("Audio Source A", 0, 30000000000ULL, err, sizeof(err));
	CHECK(ret, "re-configure Audio Source A failed");

	float unmuted_samples[256];
	for (int i = 0; i < 256; i++)
		unmuted_samples[i] = 0.88f;
	struct audio_data ad_unmuted = {
		.data = {(uint8_t *)unmuted_samples, (uint8_t *)unmuted_samples},
		.frames = 256,
		.timestamp = os_gettime_ns(),
	};
	replay_audio_capture_push_for_test(&ad_unmuted, false);

	float muted_samples[256];
	for (int i = 0; i < 256; i++)
		muted_samples[i] = 0.88f;
	struct audio_data ad_muted = {
		.data = {(uint8_t *)muted_samples, (uint8_t *)muted_samples},
		.frames = 256,
		.timestamp = ad_unmuted.timestamp + util_mul_div64(256, 1000000000ULL, 48000),
	};
	replay_audio_capture_push_for_test(&ad_muted, true);

	ret = replay_audio_capture_window(ad_unmuted.timestamp, ad_muted.timestamp + 1000000ULL, &pkts, &pkt_count,
					  NULL);
	CHECK(ret && pkt_count == 2, "expected 2 packets from push_for_test");
	float *s0 = (float *)pkts[0].data[0];
	float *s1 = (float *)pkts[1].data[0];
	CHECK(fabsf(s0[0] - 0.88f) < 1e-4f, "unmuted packet data altered");
	CHECK(s1[0] == 0.0f, "muted packet data not zeroed");

	for (uint64_t i = 0; i < pkt_count; i++)
		free_audio_packet(&pkts[i]);
	bfree(pkts);
	pkts = NULL;

	/* =====================================================================
	 * 10. Shutdown twice
	 * ===================================================================== */
	printf("[test-audio-capture] 10. Shutdown twice...\n");
	replay_audio_capture_shutdown();
	CHECK(strcmp(replay_audio_capture_source_name(), "") == 0, "name should be empty after shutdown");
	CHECK(!replay_audio_capture_coverage(&oldest, &newest, &gap), "coverage should be false after shutdown");

	/* Second shutdown call must be safe */
	replay_audio_capture_shutdown();
	CHECK(strcmp(replay_audio_capture_source_name(), "") == 0, "name should be empty after 2nd shutdown");

	obs_source_release(src_a);
	obs_source_release(src_b);
	obs_shutdown();

	printf("[test-audio-capture] All tests passed successfully!\n");
	return 0;
}
