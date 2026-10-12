#pragma once
/*
 * Frame tap contract between an async video source (DistroAV "multicam" fork) and
 * replay-source (multicam fork). Identical copies of this header live in both repos.
 *
 * A source that supports tapping registers two procs on its proc handler
 * (obs_source_get_proc_handler):
 *
 *   "void frame_tap_add(in ptr callback, in ptr param)"
 *   "void frame_tap_remove(in ptr callback, in ptr param)"
 *
 * The source invokes every registered callback on its own receive thread for each
 * video frame, immediately before obs_source_output_video(), with the identical
 * struct obs_source_frame it hands to OBS (format, data/linesize, width/height,
 * timestamp, color_matrix, color_range_min/max, full_range, trc, flip, flags).
 * Frame memory is only valid for the duration of the call; the callback must copy.
 *
 * Callbacks must be thread-safe and fast (copy/convert only, no blocking I/O).
 * After frame_tap_remove() returns, the callback is not running and is never invoked
 * again for that (callback, param) pair. Adding an already registered pair is a no-op.
 *
 * Capability check: proc_handler_call(ph, FRAME_TAP_PROC_ADD, &cd) returns false when
 * the source does not support the contract.
 */

#include <obs.h>

#define FRAME_TAP_PROC_ADD "frame_tap_add"
#define FRAME_TAP_PROC_REMOVE "frame_tap_remove"
#define FRAME_TAP_PROC_ADD_DECL "void frame_tap_add(in ptr callback, in ptr param)"
#define FRAME_TAP_PROC_REMOVE_DECL "void frame_tap_remove(in ptr callback, in ptr param)"

typedef void (*frame_tap_cb)(void *param, obs_source_t *source, const struct obs_source_frame *frame);
