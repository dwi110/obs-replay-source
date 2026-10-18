#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* source_name "" disables. offset_ns (-5 s .. +5 s) shifts the buffered timestamps (positive = audio later). Re-resolves
 * the source by name when it is created later or renamed? -> keep it simple: resolve by name now and via a weak ref;
 * if the source is missing now, return false with err "source not found". */
bool replay_audio_capture_configure(const char *source_name, int64_t offset_ns, uint64_t history_ns, char *err,
				    size_t err_size);

/* removes callback, frees buffer; safe to call twice */
void replay_audio_capture_shutdown(void);

/* Copies (shares by reference) every buffered packet overlapping [start_ts, end_ts] into a new bzalloc'ed array.
 * Returns false if nothing is configured. covered_from/covered_to report the buffered coverage [oldest, newest end]. */
bool replay_audio_capture_window(uint64_t start_ts, uint64_t end_ts, struct obs_audio_data **packets_out,
				 uint64_t *count_out, struct audio_convert_info *oai_out);

bool replay_audio_capture_coverage(uint64_t *oldest_ts, uint64_t *newest_end_ts, bool *gap_out);

/* "" if off; pointer valid until next configure */
const char *replay_audio_capture_source_name(void);

/* Test helper: pushes audio directly into the ring buffer without an OBS audio thread. */
void replay_audio_capture_push_for_test(const struct audio_data *audio_data, bool muted);

#ifdef __cplusplus
}
#endif
