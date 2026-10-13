# Replay Source Control & Companion Integration

This fork extends `replay_source` for multi-camera instant-replay workflows controlled from **Bitfocus Companion** via **obs-websocket 5**.

## 1. Per-Source Hotkeys (`<legacy>.<source_name>`)

Bitfocus Companion's OBS module lists hotkeys with `GetHotkeyList` (names only) and triggers them via `TriggerHotkeyByName` without `contextName`. To ensure every replay angle has a unique hotkey name across `GetHotkeyList`, all 37 per-source hotkeys are registered as:

```text
<legacy_name>.<source_name>
```

For example, a replay source named `Replay Cam 1` registers:

- `ReplaySource.Replay.Replay Cam 1` (Load Replay)
- `ReplaySource.Save.Replay Cam 1` (Save Replay)
- `ReplaySource.Restart.Replay Cam 1`
- `ReplaySource.Pause.Replay Cam 1`
- `ReplaySource.NormalSpeed.Replay Cam 1`
- `ReplaySource.HalfSpeed.Replay Cam 1`
- `ReplaySource.DoubleSpeed.Replay Cam 1`
- `ReplaySource.TrimFront.Replay Cam 1`
- `ReplaySource.TrimEnd.Replay Cam 1`
- `ReplaySource.TrimReset.Replay Cam 1`
- `ReplaySource.Reverse.Replay Cam 1`
- `ReplaySource.Forward.Replay Cam 1`
- `ReplaySource.Backward.Replay Cam 1`
- `ReplaySource.Next.Replay Cam 1`, `ReplaySource.Previous.Replay Cam 1`, `ReplaySource.First.Replay Cam 1`, `ReplaySource.Last.Replay Cam 1`, `ReplaySource.Remove.Replay Cam 1`, `ReplaySource.Clear.Replay Cam 1`
- `ReplaySource.Faster.Replay Cam 1`, `ReplaySource.Slower.Replay Cam 1`, `ReplaySource.FasterBy5.Replay Cam 1`, `ReplaySource.SlowerBy5.Replay Cam 1`, `ReplaySource.NormalOrFaster.Replay Cam 1`, `ReplaySource.NormalOrSlower.Replay Cam 1`, `ReplaySource.ForwardOrFaster.Replay Cam 1`, `ReplaySource.BackwardOrFaster.Replay Cam 1`
- `ReplaySource.Disable.Replay Cam 1`, `ReplaySource.Enable.Replay Cam 1`, `ReplaySource.DisableNextScene.Replay Cam 1`, `ReplaySource.EnableNextScene.Replay Cam 1`, `ReplaySource.NextSceneCurrent.Replay Cam 1`, `ReplaySource.NextScene.Replay Cam 1`
- `ReplaySource.PrevFrame.Replay Cam 1`, `ReplaySource.NextFrame.Replay Cam 1`, `ReplaySource.PrevNFrames.Replay Cam 1`, `ReplaySource.NextNFrames.Replay Cam 1`

### Automatic Legacy Migration

Existing scene collections saved with upstream Replay Source store key bindings under the bare legacy name (e.g. `ReplaySource.Save`). During `replay_source_create`:

1. The plugin registers `<legacy_name>.<source_name>`.
2. If the new hotkey has no saved bindings, it temporarily registers `<legacy_name>` on the same source so `libobs` loads any legacy bindings from the source's saved hotkey data.
3. Any loaded bindings are copied via `obs_hotkey_save` → `obs_hotkey_load` onto the new hotkey, and the temporary hotkey is immediately unregistered.

Rollback to the original Replay Source DLL: once the scene collection has been saved with this fork, bindings are stored
under the new names only, so the original plugin starts without key bindings for replay sources (re-assign them, or keep
a backup of the scene collection JSON from before the switch).

### Source Rename Behavior

Each replay source connects to its `rename` signal (`new_name`) and updates all 37 hotkey names in-place using `obs_hotkey_set_name`. Renaming `Replay Cam 1` to `Replay Angle A` immediately updates its hotkey names to `ReplaySource.<Action>.Replay Angle A`.

---

## 2. Global Multi-Angle Frontend Hotkeys

Registered once at module load (`obs_hotkey_register_frontend`):

| Hotkey Name | Description (en-US / de-DE) | Behavior |
|---|---|---|
| `ReplaySource.LoadAll` | `Replay: load all angles` / `Replay: Alle Winkel laden` | Snapshots `t_end = os_gettime_ns()`, loads all replay sources synchronously on the same thread, and trims each loaded replay (`trim_front`/`trim_end`) to the common time window `[max(first_ts), min(last_ts)]`. Ignores per-source `retrieve_delay` so all angles capture the exact same moment. Empty buffers are skipped. |
| `ReplaySource.SaveAll` | `Replay: save all loaded replays` / `Replay: Alle geladenen Replays speichern` | Enqueues all loaded replays into the sequential save queue. |
| `ReplaySource.AllNormalSpeed` | `Replay: all angles normal speed (100%)` / `Replay: Alle Winkel Normalgeschwindigkeit (100%)` | Sets playback speed to `100%` on all replay sources. |
| `ReplaySource.AllHalfSpeed` | `Replay: all angles half speed (50%)` / `Replay: Alle Winkel halbe Geschwindigkeit (50%)` | Sets playback speed to `50%` on all replay sources. |
| `ReplaySource.AllQuarterSpeed` | `Replay: all angles quarter speed (25%)` / `Replay: Alle Winkel Viertelgeschwindigkeit (25%)` | Sets playback speed to `25%` on all replay sources. |
| `ReplaySource.AllRestart` | `Replay: restart all angles` / `Replay: Alle Winkel neu starten` | Restarts playback on all replay sources. |
| `ReplaySource.AllPause` | `Replay: pause/play all angles` / `Replay: Alle Winkel pausieren/fortsetzen` | Toggles pause/play on all replay sources. |
| `ReplaySource.AllMarkIn` | `Replay: mark in (all angles)` / `Replay: Mark In (alle Winkel)` | Sets Mark In across all loaded angles using the absolute playhead timestamp of the reference camera (the replay source currently active in program if exactly one is active, otherwise the first playing/paused replay source with a loaded replay). Trims are clamped to each angle's aligned base window. |
| `ReplaySource.AllMarkOut` | `Replay: mark out (all angles)` / `Replay: Mark Out (alle Winkel)` | Sets Mark Out across all loaded angles using the absolute playhead timestamp of the reference camera. Trims are clamped to each angle's aligned base window. |
| `ReplaySource.AllMarkReset` | `Replay: reset marks (all angles)` / `Replay: Marken zurücksetzen (alle Winkel)` | Restores the aligned base window across all loaded angles (or start-delay behavior if unaligned), preventing angles from becoming misaligned after custom trims. |

---

## 3. `obs-websocket` Vendor API (`ReplaySource`)

Registered in `obs_module_post_load` under `vendorName`: **`ReplaySource`**. If `obs-websocket` is not installed, the plugin logs one `INFO` message and continues normally.

### Requests

#### `GetReplays`
Returns status for all replay sources and the sequential save queue.

- **Request Data**: `{}`
- **Response Data**:
```json
{
  "ok": true,
  "replays": [
    {
      "name": "Replay Cam 1",
      "videoSource": "Cam 1",
      "loadedReplays": 1,
      "active": true,
      "playing": true,
      "speedPercent": 50.0,
      "backward": false,
      "durationMs": 4850,
      "bufferedMs": 5000,
      "markInMs": 0,
      "markOutMs": 4850,
    }
  ],
  "saveQueue": {
    "pending": 0,
    "completed": 4
  }
}
```

#### `Action`
Runs any existing `execute_action` command (`Load`, `Next`, `Previous`, `First`, `Last`, `Remove`, `Clear`, `Save`, `Restart`, `Pause`, `Faster`, `Slower`, `Faster5Percent`, `Slower5Percent`, `NormalOrFaster`, `NormalOrSlower`, `NormalSpeed`, `HalfSpeed`, `DoubleSpeed`, `TrimFront`, `TrimEnd`, `TrimReset`, `MarkIn`, `MarkOut`, `MarkReset`, `Reverse`, `Backward`, `Forward`, `ForwardOrFaster`, `BackwardOrFaster`, `DisableNextScene`, `EnableNextScene`, `Disable`, `Enable`, `SetNextSceneToCurrent`, `SwitchToNextScene`, `NextFrame`, `PrevFrame`, `NextNFrames`, `PrevNFrames`).
Omit both `source` and `sources` to target **all** replay sources. `sources` accepts `[ { "name": "Replay Cam 1" } ]` or a comma-separated string `"Replay Cam 1,Replay Cam 2"`. A plain string array (`["Replay Cam 1"]`) is dropped by obs-websocket while converting JSON to `obs_data` (only object items survive), so such a request is rejected with `ok: false` instead of silently targeting nothing.

- **Request Data**:
```json
{
  "source": "Replay Cam 1",
  "action": "HalfSpeed"
}
```
- **Response Data**:
```json
{
  "ok": true,
  "action": "HalfSpeed",
  "applied": [{ "name": "Replay Cam 1" }],
  "unknown": [],
  "appliedCount": 1,
  "unknownCount": 0
}
```

#### `SetSpeed`
Sets arbitrary playback speed (`0.01`–`400.0`%) on one, several, or all replay sources.

- **Request Data**:
```json
{
  "speedPercent": 25.0
}
```
- **Response Data**:
```json
{
  "ok": true,
  "speedPercent": 25.0,
  "applied": [{ "name": "Replay Cam 1" }, { "name": "Replay Cam 2" }],
  "unknown": [],
  "appliedCount": 2,
  "unknownCount": 0
}
```

#### `LoadAll`
Synchronously loads and time-aligns all (or selected) replay sources to the common frame timestamp intersection `[max(first_ts), min(last_ts)]`.

- **Request Data**: `{}` (or `{ "sources": [{ "name": "Replay Cam 1" }, { "name": "Replay Cam 2" }] }`)
- **Response Data**:
```json
{
  "ok": true,
  "replays": [
    {
      "name": "Replay Cam 1",
      "startMs": 100450,
      "durationMs": 4850,
      "trimFrontMs": 66,
      "trimEndMs": 0
    },
    {
      "name": "Replay Cam 2",
      "startMs": 100450,
      "durationMs": 4850,
      "trimFrontMs": 0,
      "trimEndMs": 66
    }
  ],
  "skipped": [{ "name": "Replay Cam 3" }],
  "unknown": [],
  "loadedCount": 2,
  "skippedCount": 1,
  "unknownCount": 0
}
```

#### `SaveAll`
Enqueues all loaded replays into the sequential save queue.

- **Request Data**: `{}`
- **Response Data**:
```json
{
  "ok": true,
  "queued": 2
}
```

#### `MarkIn`
Sets Mark In on one, several, or all loaded replay sources using the absolute playhead timestamp of the reference angle (the replay source currently active in program if exactly one is active, otherwise the explicitly targeted angle or the first playing/paused replay source). Mark In is clamped to each replay's aligned base window.

- **Request Data**: `{}` (or `{ "source": "Replay Cam 1" }` or `{ "sources": "Replay Cam 1,Replay Cam 2" }`)
- **Response Data**:
```json
{
  "ok": true,
  "sources": [
    {
      "name": "Replay Cam 1",
      "inMs": 1500,
      "outMs": 4850,
      "durationMs": 3350
    }
  ],
  "applied": [{ "name": "Replay Cam 1" }],
  "unknown": [],
  "appliedCount": 1,
  "unknownCount": 0
}
```
`inMs`, `outMs`, and `durationMs` are reported in milliseconds relative to each source's aligned window start.

#### `MarkOut`
Sets Mark Out on one, several, or all loaded replay sources using the absolute playhead timestamp of the reference angle, clamped to each replay's aligned base window.

- **Request Data**: `{}` (or `{ "source": "Replay Cam 1" }` or `{ "sources": "Replay Cam 1,Replay Cam 2" }`)
- **Response Data**: same format as `MarkIn`.

#### `MarkReset`
Restores the aligned base window on one, several, or all loaded replay sources (or start-delay behavior if not aligned).

- **Request Data**: `{}` (or `{ "source": "Replay Cam 1" }` or `{ "sources": "Replay Cam 1,Replay Cam 2" }`)
- **Response Data**: same format as `MarkIn`, with `inMs: 0` and `outMs: alignedDurationMs`.

### Vendor Events

Emitted under `vendorName`: **`ReplaySource`**:

- **`ReplayLoaded`**: `{ "source": "Replay Cam 1", "durationMs": 4850 }`
- **`SaveStarted`**: `{ "source": "Replay Cam 1", "path": "C:/Replays/2026-10-13 18.42.10 - Replay Cam 1.mp4", "trigger": "manual" }` (`trigger` is `"manual"`, `"auto-load"`, `"auto-release"`, or `"save-all"`)
- **`SaveFinished`**: `{ "source": "Replay Cam 1", "path": "C:/Replays/2026-10-13 18.42.10 - Replay Cam 1.mp4", "ok": true, "status": "ok" }` (`status` is `"ok"` or `"failed"`)


---

## 4. Auto-Save Configuration (`auto_save`)

Configured per replay source under settings:
- **`Off` (`0`, default)**: Auto-save disabled.
- **`On Load` (`1`)**: Enqueue immediately when a replay is loaded (with full or aligned window). When using `LoadAll`, all angles are enqueued in source order after time-alignment so files share the exact same clip window.
- **`On Release (with Mark In/Out)` (`2`)**: Enqueue when the loaded replay is released (`Clear`, `Remove`, purge when exceeding `Maximum Replays`, auto-release on deactivate/hide, or replacement by a newer load). This honors any `Mark In` or `Mark Out` set during playback. Not triggered during source destroy or OBS shutdown.

Each replay is auto-saved at most once (tracked via an internal replay flag). Manual saves (`Save`, `SaveAll`) always remain available.

Saving still uses one global sequential queue. NVENC HEVC/H.264 uses OBS's raw `ffmpeg_output` with the bundled
FFmpeg `hevc_nvenc`/`h264_nvenc` encoders, not OBS's canvas-bound NVENC encoders. The latter crash with the private
replay `video_t` on OBS 31+ (`cuda_surface_init` calls `obs_encoder_video_tex_active` without a core video mix).
If a backend cannot start, the next plan is tried before feeding the clip: HEVC NVENC → H.264 NVENC → x264;
explicit H.264 tries H.264 then x264, and explicit x264 stays on x264.

NVENC uses preset P5 and CQP (quality 10–40, default 18), without B-frames, lookahead or packet delay: `ffmpeg_output`
does not drain delayed video packets when stopping. This preserves all clip frames at a modest compression-efficiency
cost. AAC callbacks continue briefly with silence after the selected end to emit the final audio samples; no extra video
frames are submitted. x264 remains CRF/veryfast through `ffmpeg_muxer`; lossless remains UtVideo/PCM AVI.

Exports preserve source time at the OBS canvas frame rate. Missing reception slots hold the previous available frame;
they do not shorten the video or shift it ahead of audio. A lossless sparse-input regression decodes the result and
checks both duration/frame count and the held-frame positions. UtVideo uses one encoding thread to avoid its final
frame-threading packet delay in `ffmpeg_output`.
---

## 5. Bitfocus Companion Setup

### Hotkeys ("Hotkey - Trigger by ID")
Companion lists the names from `GetHotkeyList`, so each angle now appears with its own entry, e.g.
`ReplaySource.Save.Replay Cam 1`, `ReplaySource.Save.Replay Cam 2`. Companion reads this list when it connects and when
the OBS profile or scene collection changes; after renaming a replay source (or adding a new one) reconnect the OBS
connection in Companion so the new names appear. Buttons store the hotkey name, so renaming a source requires picking the
new name on its buttons.

### Triggering Actions ("Custom - Send Vendor Request")
Add an OBS action **"Custom - Send Vendor Request"**:
- **Vendor Name**: `ReplaySource`
- **Request Type**: `LoadAll` / `SaveAll` / `SetSpeed` / `Action` / `GetReplays`
- **Request Data**:
  - Load all angles aligned: `{}`
  - Set all angles to 25% slow-motion: `{"speedPercent": 25}`
  - Pause a single angle: `{"source": "Replay Cam 1", "action": "Pause"}`

### Button Feedback ("Custom - Vendor Event")
Add an OBS feedback **"Custom - Vendor Event"**:
- **Vendor Name**: `ReplaySource`
- **Event Type**: `ReplayLoaded`, `SaveStarted`, or `SaveFinished`
- **Data Key / Value**: e.g. `source` = `Replay Cam 1`, or `status` = `ok` / `failed` on `SaveFinished`.

Companion compares the value **as a string** (`===`) and only looks at the **most recent** vendor event of any plugin.
Numeric/boolean fields (`durationMs`, `ok`) therefore never match; use the string fields `source` and `status`.
Example: button "Saving Cam 1" active while the latest event is `SaveStarted` with `source` = `Replay Cam 1`.

---

## 6. Slow-Motion Blending (Crossfade)

### Setting: `slowmo_blend`
- **Type**: `int` (property dropdown on the replay source)
- **Values**:
  - `0` (`SLOWMO_BLEND_OFF`): Off (default, original behavior). Each stored frame is output once with a stretched timestamp, resulting in step-stutter at low speeds (e.g., each frame held for 4 output frames at 25%).
  - `1` (`SLOWMO_BLEND_CROSSFADE`): Smooth slow-motion via crossfade blending.
- **UI Labels**:
  - en-US: `Smooth Slow Motion` ("Off" / "Crossfade (motion blur instead of stutter)")
  - de-DE: `Zeitlupe glätten` ("Aus" / "Überblenden (Bewegungsunschärfe statt Ruckeln)")

### Behavior
- When enabled and playback speed is < 100% (both forward and backward), intermediate frames are synthesized between consecutive source frames $A$ and $B$ at the OBS output frame interval.
- Frame weights $w8 = \mathrm{round}(w \cdot 256) \in [0, 256]$ are computed from the exact output timestamp position between $A$ and $B$, supporting arbitrary speeds (e.g., 50%, 25%, 75%, 33%).
- Blending formula: per-plane 8-bit weighted average $\mathrm{out} = (A \cdot (256 - w8) + B \cdot w8 + 128) \gg 8$, implemented in SIMD (SSE2) with a bit-identical scalar fallback for NV12, UYVY, and I420.
- Offloaded to a dedicated per-source worker thread without blocking the OBS graphics thread.
- At $\ge 100\%$ speed, paused playback, or single-frame stepping, blending is bypassed and original behavior is retained.
- Clear, Remove, new replay load, restart, direction change, or speed change immediately flushes pending worker frames to guarantee strict monotonic timestamps and avoid stale frames.

### Motion Quality & Limitations (Ghosting)
- Crossfade blending replaces discrete stuttering/judder with smooth motion transitions.
- Because crossfade blends pixel values directly rather than tracking motion vectors (optical flow), very fast motion across high-contrast edges will produce a double-image/ghosting effect rather than true intermediate movement.
- True motion-compensated frame interpolation (e.g. NVIDIA Optical Flow SDK / FRUC) requires GPU motion estimation and remains a potential future enhancement.
