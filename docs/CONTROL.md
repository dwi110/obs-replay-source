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
      "bufferedMs": 5000
    }
  ],
  "saveQueue": {
    "pending": 0,
    "completed": 4
  }
}
```

#### `Action`
Runs any existing `execute_action` command (`Load`, `Next`, `Previous`, `First`, `Last`, `Remove`, `Clear`, `Save`, `Restart`, `Pause`, `Faster`, `Slower`, `Faster5Percent`, `Slower5Percent`, `NormalOrFaster`, `NormalOrSlower`, `NormalSpeed`, `HalfSpeed`, `DoubleSpeed`, `TrimFront`, `TrimEnd`, `TrimReset`, `Reverse`, `Backward`, `Forward`, `ForwardOrFaster`, `BackwardOrFaster`, `DisableNextScene`, `EnableNextScene`, `Disable`, `Enable`, `SetNextSceneToCurrent`, `SwitchToNextScene`, `NextFrame`, `PrevFrame`, `NextNFrames`, `PrevNFrames`).
Omit both `source` and `sources` to target **all** replay sources. Note: because `libobs`'s `obs_data_t` represents JSON arrays as arrays of objects, `sources` accepts either `[ { "name": "Replay Cam 1" } ]` or a comma-separated string `"Replay Cam 1,Replay Cam 2"`.

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

### Vendor Events

Emitted under `vendorName`: **`ReplaySource`**:

- **`ReplayLoaded`**: `{ "source": "Replay Cam 1", "durationMs": 4850 }`
- **`SaveStarted`**: `{ "source": "Replay Cam 1", "path": "C:/Replays/2026-10-13 18.42.10 - Replay Cam 1.mp4" }`
- **`SaveFinished`**: `{ "source": "Replay Cam 1", "path": "C:/Replays/2026-10-13 18.42.10 - Replay Cam 1.mp4", "ok": true }`

---

## 4. Bitfocus Companion Setup

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
- **Data Key / Value**: e.g. `source` = `Replay Cam 1` (or `ok` = `true` on `SaveFinished`)
