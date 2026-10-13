#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <unistd.h>
#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include "replay.h"
#include "replay-save-queue.h"
#include "replay-pool.h"

extern bool obs_module_load(void);
extern void obs_module_unload(void);

static void fill_nv12_frame(struct obs_source_frame *f, uint32_t frame_idx)
{
	const uint32_t w = f->width;
	const uint32_t h = f->height;
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = f->data[0] + y * f->linesize[0];
		uint8_t luma = (uint8_t)(16 + ((y + frame_idx * 3) % 200));
		memset(row, luma, w);
	}
	for (uint32_t y = 0; y < h / 2; y++) {
		uint8_t *row = f->data[1] + y * f->linesize[1];
		for (uint32_t x = 0; x < w; x += 2) {
			row[x] = (uint8_t)(90 + (frame_idx % 80));
			row[x + 1] = (uint8_t)(140 + (frame_idx % 60));
		}
	}
}

static void fill_uyvy_frame(struct obs_source_frame *f, uint32_t frame_idx)
{
	const uint32_t w = f->width;
	const uint32_t h = f->height;
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = f->data[0] + y * f->linesize[0];
		uint8_t yval = (uint8_t)(32 + ((y + frame_idx * 4) % 180));
		uint8_t uval = (uint8_t)(100 + (frame_idx % 50));
		uint8_t vval = (uint8_t)(150 - (frame_idx % 50));
		for (uint32_t x = 0; x < w * 2; x += 4) {
			row[x + 0] = uval;
			row[x + 1] = yval;
			row[x + 2] = vval;
			row[x + 3] = yval;
		}
	}
}

/* Stored frames always come from a replay pool (see the ownership contract in replay.h); the pool is closed
 * right away and frees itself once the last frame reference is released. */
static struct obs_source_frame **create_clip_video_frames(enum video_format fmt, uint32_t width, uint32_t height,
							  uint64_t count, uint64_t start_ts, uint64_t interval_ns)
{
	struct replay_pool *pool = replay_pool_create(fmt, width, height, 0);
	struct obs_source_frame **frames = bzalloc((size_t)count * sizeof(struct obs_source_frame *));
	for (uint64_t i = 0; i < count; i++) {
		struct obs_source_frame *f = replay_pool_acquire(pool);
		f->timestamp = start_ts + i * interval_ns;
		video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, fmt, f->color_matrix,
						       f->color_range_min, f->color_range_max);
		if (fmt == VIDEO_FORMAT_NV12)
			fill_nv12_frame(f, (uint32_t)i);
		else if (fmt == VIDEO_FORMAT_UYVY)
			fill_uyvy_frame(f, (uint32_t)i);
		frames[i] = f;
	}
	replay_pool_close(pool);
	return frames;
}

static struct obs_audio_data *create_clip_audio_frames(uint64_t start_ts, uint64_t duration_ns, uint32_t sample_rate,
						       uint64_t *out_count)
{
	const uint32_t frames_per_pkt = 1024;
	const uint64_t pkt_ns = util_mul_div64(frames_per_pkt, 1000000000ULL, sample_rate);
	const uint64_t count = (duration_ns / pkt_ns) + 1;
	struct obs_audio_data *pkts = bzalloc((size_t)count * sizeof(struct obs_audio_data));

	double phase = 0.0;
	const double step = 2.0 * 3.14159265358979323846 * 440.0 / (double)sample_rate;

	float *ch0 = bmalloc(frames_per_pkt * sizeof(float));
	float *ch1 = bmalloc(frames_per_pkt * sizeof(float));
	for (uint64_t i = 0; i < count; i++) {
		for (uint32_t s = 0; s < frames_per_pkt; s++) {
			float v = (float)(0.25 * sin(phase));
			ch0[s] = v;
			ch1[s] = v;
			phase += step;
		}
		struct obs_audio_data src = {.frames = frames_per_pkt, .timestamp = start_ts + i * pkt_ns};
		src.data[0] = (uint8_t *)ch0;
		src.data[1] = (uint8_t *)ch1;
		replay_audio_packet_copy(&pkts[i], &src);
	}
	bfree(ch0);
	bfree(ch1);
	*out_count = count;
	return pkts;
}

static bool find_save_all_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	UNUSED_PARAMETER(id);
	bool *found = data;
	const char *name = obs_hotkey_get_name(key);
	const char *desc = obs_hotkey_get_description(key);
	if (name && strcmp(name, "ReplaySource.SaveAll") == 0) {
		printf("[test] Found global hotkey: name='%s' desc='%s'\n", name, desc ? desc : "");
		*found = true;
		return false;
	}
	return true;
}

static const char *mock_tex_enc_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Mock Texture NVENC";
}

static void *mock_tex_enc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(encoder);
	return NULL;
}

static void mock_tex_enc_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static bool mock_tex_encode(void *data, uint32_t handle, int64_t pts, uint64_t lock_key, uint64_t *next_key,
			    struct encoder_packet *packet, bool *received_packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(handle);
	UNUSED_PARAMETER(pts);
	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);
	UNUSED_PARAMETER(packet);
	UNUSED_PARAMETER(received_packet);
	return false;
}

static bool validate_with_ffprobe(const char *filepath, const char *expected_vcodec, int expected_frames)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		 "ffprobe -v error -count_frames -select_streams v:0 "
		 "-show_entries stream=codec_name,width,height,nb_read_frames "
		 "-show_entries format=duration -of default=noprint_wrappers=1 \"%s\"",
		 filepath);

	printf("[test] Running: %s\n", cmd);
	FILE *fp = popen(cmd, "r");
	if (!fp) {
		fprintf(stderr, "[test] popen ffprobe failed\n");
		return false;
	}

	char line[256];
	char codec_name[64] = {0};
	int width = 0;
	int height = 0;
	int nb_read_frames = 0;
	double duration = 0.0;

	while (fgets(line, sizeof(line), fp)) {
		printf("  [ffprobe-v] %s", line);
		if (strncmp(line, "codec_name=", 11) == 0) {
			strncpy(codec_name, line + 11, sizeof(codec_name) - 1);
			char *nl = strchr(codec_name, '\n');
			if (nl)
				*nl = '\0';
		} else if (strncmp(line, "width=", 6) == 0) {
			width = atoi(line + 6);
		} else if (strncmp(line, "height=", 7) == 0) {
			height = atoi(line + 7);
		} else if (strncmp(line, "nb_read_frames=", 15) == 0) {
			nb_read_frames = atoi(line + 15);
		} else if (strncmp(line, "duration=", 9) == 0) {
			duration = atof(line + 9);
		}
	}
	int rc = pclose(fp);
	if (rc != 0) {
		fprintf(stderr, "[test] ffprobe video exited with %d\n", rc);
		return false;
	}

	snprintf(cmd, sizeof(cmd),
		 "ffprobe -v error -select_streams a:0 "
		 "-show_entries stream=codec_name,sample_rate,channels "
		 "-of default=noprint_wrappers=1 \"%s\"",
		 filepath);
	fp = popen(cmd, "r");
	if (!fp)
		return false;

	char acodec_name[64] = {0};
	int sample_rate = 0;
	int channels = 0;
	while (fgets(line, sizeof(line), fp)) {
		printf("  [ffprobe-a] %s", line);
		if (strncmp(line, "codec_name=", 11) == 0) {
			strncpy(acodec_name, line + 11, sizeof(acodec_name) - 1);
			char *nl = strchr(acodec_name, '\n');
			if (nl)
				*nl = '\0';
		} else if (strncmp(line, "sample_rate=", 12) == 0) {
			sample_rate = atoi(line + 12);
		} else if (strncmp(line, "channels=", 9) == 0) {
			channels = atoi(line + 9);
		}
	}
	rc = pclose(fp);
	if (rc != 0) {
		fprintf(stderr, "[test] ffprobe audio exited with %d\n", rc);
		return false;
	}

	if (strcmp(codec_name, expected_vcodec) != 0) {
		fprintf(stderr, "[test] Expected vcodec '%s', got '%s'\n", expected_vcodec, codec_name);
		return false;
	}
	if (width != 640 || height != 360) {
		fprintf(stderr, "[test] Expected 640x360, got %dx%d\n", width, height);
		return false;
	}
	if (nb_read_frames < expected_frames - 5 || nb_read_frames > expected_frames + 5) {
		fprintf(stderr, "[test] Expected ~%d frames, got %d\n", expected_frames, nb_read_frames);
		return false;
	}
	if (duration < 0.80 || duration > 1.35) {
		fprintf(stderr, "[test] Expected duration ~1.0s, got %.3fs\n", duration);
		return false;
	}
	if (acodec_name[0] == '\0' || sample_rate != 48000 || channels != 2) {
		fprintf(stderr, "[test] Audio stream invalid: codec='%s' rate=%d ch=%d\n", acodec_name, sample_rate,
			channels);
		return false;
	}

	return true;
}

int main(int argc, char **argv)
{
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
		printf("[test] No DISPLAY set, re-executing under xvfb-run -a...\n");
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

	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "[test] obs_startup failed\n");
		return 1;
	}

	obs_add_data_path("/usr/share/obs/libobs/");

	struct obs_video_info ovi = {
		.graphics_module = "libobs-opengl.so.30",
		.fps_num = 60,
		.fps_den = 1,
		.base_width = 640,
		.base_height = 360,
		.output_width = 640,
		.output_height = 360,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = 0,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BILINEAR,
	};
	int vret = obs_reset_video(&ovi);
	if (vret != OBS_VIDEO_SUCCESS) {
		ovi.graphics_module = "libobs-opengl";
		vret = obs_reset_video(&ovi);
	}
	if (vret != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "[test] obs_reset_video failed: %d\n", vret);
		return 1;
	}

	struct obs_audio_info oai = {
		.samples_per_sec = 48000,
		.speakers = SPEAKERS_STEREO,
	};
	if (!obs_reset_audio(&oai)) {
		fprintf(stderr, "[test] obs_reset_audio failed\n");
		return 1;
	}

	obs_module_t *mod_ffmpeg = NULL;
	if (obs_open_module(&mod_ffmpeg, "/usr/lib/x86_64-linux-gnu/obs-plugins/obs-ffmpeg.so",
			    "/usr/share/obs/obs-plugins/obs-ffmpeg") != MODULE_SUCCESS ||
	    !obs_init_module(mod_ffmpeg)) {
		fprintf(stderr, "[test] Failed to load obs-ffmpeg\n");
		return 1;
	}

	obs_module_t *mod_x264 = NULL;
	if (obs_open_module(&mod_x264, "/usr/lib/x86_64-linux-gnu/obs-plugins/obs-x264.so",
			    "/usr/share/obs/obs-plugins/obs-x264") != MODULE_SUCCESS ||
	    !obs_init_module(mod_x264)) {
		fprintf(stderr, "[test] Failed to load obs-x264\n");
		return 1;
	}

	obs_post_load_modules();

	/* Register a mock texture-only jim_nvenc encoder to verify auto selection skips texture-only encoders */
	struct obs_encoder_info mock_jim_nvenc = {
		.id = "jim_nvenc",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",
		.caps = OBS_ENCODER_CAP_PASS_TEXTURE,
		.get_name = mock_tex_enc_name,
		.create = mock_tex_enc_create,
		.destroy = mock_tex_enc_destroy,
		.encode_texture = mock_tex_encode,
	};
	obs_register_encoder(&mock_jim_nvenc);

	if (!obs_module_load()) {
		fprintf(stderr, "[test] obs_module_load failed\n");
		return 1;
	}

	const char *chosen_enc = replay_save_select_encoder(SAVE_ENCODER_AUTO);
	printf("[test] Auto-selected encoder on this host: '%s'\n", chosen_enc);
	if (strcmp(chosen_enc, "obs_x264") != 0) {
		fprintf(stderr, "[test] Expected auto encoder 'obs_x264' (skipping texture-only jim_nvenc), got '%s'\n",
			chosen_enc);
		return 1;
	}

	bool hotkey_found = false;
	obs_enum_hotkeys(find_save_all_hotkey_cb, &hotkey_found);
	if (!hotkey_found) {
		fprintf(stderr, "[test] Global hotkey ReplaySource.SaveAll was not registered\n");
		return 1;
	}

	const char *out_dir = "/tmp/replay-save-queue-test";
	os_mkdirs(out_dir);
	os_unlink("/tmp/replay-save-queue-test/test_clip - Cam_NV12.mp4");
	os_unlink("/tmp/replay-save-queue-test/test_clip - Cam_UYVY.mkv");
	os_unlink("/tmp/replay-save-queue-test/test_clip - Cam_Lossless.avi");

	const uint64_t frame_count = 60;
	const uint64_t interval_ns = 1000000000ULL / 60ULL;
	const uint64_t start_ts = 1000000000ULL;
	const uint64_t duration_ns = (frame_count - 1) * interval_ns;

	struct obs_source_frame **nv12_frames =
		create_clip_video_frames(VIDEO_FORMAT_NV12, 640, 360, frame_count, start_ts, interval_ns);
	uint64_t nv12_audio_count = 0;
	struct obs_audio_data *nv12_audio =
		create_clip_audio_frames(start_ts, duration_ns, 48000, &nv12_audio_count);

	struct obs_source_frame **uyvy_frames =
		create_clip_video_frames(VIDEO_FORMAT_UYVY, 640, 360, frame_count, start_ts, interval_ns);
	uint64_t uyvy_audio_count = 0;
	struct obs_audio_data *uyvy_audio =
		create_clip_audio_frames(start_ts, duration_ns, 48000, &uyvy_audio_count);

	struct audio_convert_info clip_oai = {
		.samples_per_sec = 48000,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = SPEAKERS_STEREO,
	};

	struct replay_save_job_params job1 = {
		.source_name = "Cam_NV12",
		.directory = out_dir,
		.file_format = "test_clip",
		.save_encoder = SAVE_ENCODER_AUTO,
		.save_container = SAVE_CONTAINER_MP4,
		.lossless = false,
		.video_frames = nv12_frames,
		.video_frame_count = frame_count,
		.audio_frames = nv12_audio,
		.audio_frame_count = nv12_audio_count,
		.oai = clip_oai,
		.first_frame_timestamp = start_ts,
		.last_frame_timestamp = start_ts + duration_ns,
		.duration = duration_ns,
		.trim_front = 0,
		.trim_end = 0,
	};

	struct replay_save_job_params job2 = {
		.source_name = "Cam_UYVY",
		.directory = out_dir,
		.file_format = "test_clip",
		.save_encoder = SAVE_ENCODER_AUTO,
		.save_container = SAVE_CONTAINER_MKV,
		.lossless = false,
		.video_frames = uyvy_frames,
		.video_frame_count = frame_count,
		.audio_frames = uyvy_audio,
		.audio_frame_count = uyvy_audio_count,
		.oai = clip_oai,
		.first_frame_timestamp = start_ts,
		.last_frame_timestamp = start_ts + duration_ns,
		.duration = duration_ns,
		.trim_front = 0,
		.trim_end = 0,
	};

	struct replay_save_job_params job3 = {
		.source_name = "Cam_Lossless",
		.directory = out_dir,
		.file_format = "test_clip",
		.save_encoder = SAVE_ENCODER_AUTO,
		.save_container = SAVE_CONTAINER_MP4,
		.lossless = true,
		.video_frames = nv12_frames,
		.video_frame_count = frame_count,
		.audio_frames = nv12_audio,
		.audio_frame_count = nv12_audio_count,
		.oai = clip_oai,
		.first_frame_timestamp = start_ts,
		.last_frame_timestamp = start_ts + duration_ns,
		.duration = duration_ns,
		.trim_front = 0,
		.trim_end = 0,
	};

	/* Enqueue both jobs back-to-back */
	if (!replay_save_queue_enqueue(&job1)) {
		fprintf(stderr, "[test] Failed to enqueue job1 (NV12 MP4)\n");
		return 1;
	}
	if (!replay_save_queue_enqueue(&job2)) {
		fprintf(stderr, "[test] Failed to enqueue job2 (UYVY MKV)\n");
		return 1;
	}
	if (!replay_save_queue_enqueue(&job3)) {
		fprintf(stderr, "[test] Failed to enqueue job3 (Lossless AVI)\n");
		return 1;
	}

	const size_t pinned_after_enqueue = replay_save_queue_pinned_bytes();
	const size_t pending_after_enqueue = replay_save_queue_pending_count();
	printf("[test] Immediately after enqueueing 2 jobs: pending=%zu, pinned=%zu bytes\n", pending_after_enqueue,
	       pinned_after_enqueue);
	if (pinned_after_enqueue == 0 || pending_after_enqueue == 0) {
		fprintf(stderr, "[test] Expected non-zero pinned bytes and pending count after enqueue\n");
		return 1;
	}

	/* Immediately release caller's references and free caller's audio buffers while jobs are still running/pending! */
	for (uint64_t i = 0; i < frame_count; i++) {
		replay_frame_release(nv12_frames[i]);
		replay_frame_release(uyvy_frames[i]);
	}
	bfree(nv12_frames);
	bfree(uyvy_frames);
	for (uint64_t i = 0; i < nv12_audio_count; i++)
		free_audio_packet(&nv12_audio[i]);
	bfree(nv12_audio);
	for (uint64_t i = 0; i < uyvy_audio_count; i++)
		free_audio_packet(&uyvy_audio[i]);
	bfree(uyvy_audio);

	/* Wait for both jobs to complete sequentially */
	if (!replay_save_queue_wait_idle(30000)) {
		fprintf(stderr, "[test] Timeout waiting for save queue to finish\n");
		return 1;
	}

	const uint64_t completed = replay_save_queue_completed_count();
	const long max_concurrent = replay_save_queue_max_concurrent_encoders();
	const size_t pinned_after_idle = replay_save_queue_pinned_bytes();
	printf("[test] Queue idle: completed=%" PRIu64 ", max_concurrent_encoders=%ld, pinned_after=%zu\n", completed,
	       max_concurrent, pinned_after_idle);

	if (completed != 3) {
		fprintf(stderr, "[test] Expected 3 completed jobs, got %" PRIu64 "\n", completed);
		return 1;
	}
	if (max_concurrent != 1) {
		fprintf(stderr, "[test] Expected max_concurrent_encoders == 1, got %ld\n", max_concurrent);
		return 1;
	}
	if (pinned_after_idle != 0) {
		fprintf(stderr, "[test] Expected 0 pinned bytes after idle, got %zu\n", pinned_after_idle);
		return 1;
	}

	if (!validate_with_ffprobe("/tmp/replay-save-queue-test/test_clip - Cam_NV12.mp4", "h264", 60)) {
		fprintf(stderr, "[test] Validation failed for NV12 MP4\n");
		return 1;
	}
	if (!validate_with_ffprobe("/tmp/replay-save-queue-test/test_clip - Cam_UYVY.mkv", "h264", 60)) {
		fprintf(stderr, "[test] Validation failed for UYVY MKV\n");
		return 1;
	}
	if (!validate_with_ffprobe("/tmp/replay-save-queue-test/test_clip - Cam_Lossless.avi", "utvideo", 60)) {
		fprintf(stderr, "[test] Validation failed for Lossless AVI\n");
		return 1;
	}

	obs_module_unload();
	obs_shutdown();
	printf("[test] ALL SAVE QUEUE TESTS PASSED!\n");
	return 0;
}
