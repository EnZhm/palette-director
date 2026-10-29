# Palette Director

> **This repository is archived. Palette Director now lives in [EnZhm/director](https://github.com/EnZhm/director)** (`plugins/palette-director`), released as part of [Director 2.0.0](https://github.com/EnZhm/director/releases/tag/2.0.0) together with Music Director, Palette Director, Clip Director and Scene Director. Download and install from there. The releases below stay available as history.

Palette Director is an OBS Studio plugin (Windows x64) for VJ sets. It locks a
Synesthesia (Spout) feed to a set of colour palettes, listens to the DJ mix in OBS, and
moves through those palettes on drops and phrases; sometimes the lock lets go so the raw
visuals show through. Part of the [Director](https://github.com/EnZhm/director) suite.

| | |
|---|---|
| Version | 1.2.0 |
| Platform | Windows 10/11 x64 |
| OBS Studio | 32.2.x (built against 32.2.2) |
| Licence | GPL-2.0 |

## What it adds

| Added | Name | Where it goes |
|---|---|---|
| Video filter | Palette Lock | Filters on the Synesthesia Spout source; recolours it with the live palette |
| Dock | Palette Director | Palettes, look, palette changes, lock behaviours, decision log |
| Dock | Music | Beat clock, section, level and bass meter, 64-beat graph, audio controls, beat-matching buttons |

Palette Director owns the beat clock. It reads one OBS audio source, finds the BPM, bars,
phrases, breakdowns and drops, and publishes the clock to other plugins (see
[Beat clock API](#beat-clock-api-for-other-plugins)). Clip Director cuts clips and
switches Synesthesia scenes on that same grid.

## Requirements

| Item | Notes |
|---|---|
| Windows 10 or 11, x64 | Windows only. |
| OBS Studio 32.2.x | 32.2.2 at release. |
| Synesthesia, Pro licence | For the Spout input that Palette Lock sits on. |
| The DJ mix as an OBS audio source | For example an "Audio Input Capture" named `Mixer`. Without one the Director runs at the Fallback BPM. |

## Install

1. Install the suite first: download `director-1.0.0-windows-x64.zip` from
   [EnZhm/director releases](https://github.com/EnZhm/director/releases) and copy both
   folders into `C:\ProgramData\obs-studio\plugins\`. That gets you Palette Director and
   Clip Director in one step.
2. If you only want Palette Director, take
   `palette-director-1.2.0-windows-x64.zip` from
   [this repo's releases](https://github.com/EnZhm/palette-director/releases) and copy
   the folder `palette-director` into `C:\ProgramData\obs-studio\plugins\`.
3. Start OBS and check Help > Log Files > View Current Log for:

   ```
   [palette-director] plugin loaded successfully (version 1.2.0)
   ```

Step-by-step install, first run, troubleshooting and uninstall:
[release/INSTALL.txt](release/INSTALL.txt).

## The Palette Director dock

Open it from Docks > "Palette Director". The top strip always shows what the Director is
doing: the filter it controls, a section chip (GROOVE / BREAKDOWN / DROP), the lock state,
the director state, Coverage and a one-line summary (for example
`Live: Neon Grid - next: Outrun on the next drop - lock ON`), then the controls Force off /
Force on / Pause / Prev / Next / Rotate colours / PANIC. The BPM, confidence, beat lights,
the meter and the 64-beat timeline live in the Music dock.

Below it, Set-up opens the tabs:

| Tab | What is in it |
|---|---|
| Set | Pick a set; New / Duplicate / Rename / Delete / Save / Load file...; Synesthesia playlist |
| Palettes | Reorder, Go live, Set as breakdown, New / Duplicate / Delete, and the palette editor (name, gradient map or duotone, 2-8 colours, sort dark to bright, reverse) |
| Look | Coverage, Input black / white, Brightness from, Contrast, Keep brightness |
| Changes | When palettes change (every drop, every N phrases, random, manual), order, crossfade, breakdown palette |
| Director | Director enabled, lock behaviours (random off, off on breakdown, off on drop, breathing, colour rotation, seed) and the drop colour cycle |
| Log | The Director's full decision log |

## The Music dock

Open it from Docks > "Music". Everything about hearing the music lives here; the same
audio settings keys are used as before, so an existing scene collection keeps its values.

| Area | Contents |
|---|---|
| Clock | Instance picker (the same filter list as the Palette Director dock), BPM, confidence, phrase.bar.beat, section and how long it has held |
| Beat lights | One per beat in the bar |
| Meter | LEVEL after Input gain (CLIP above -1 dBFS) and BASS with the kick threshold marked; the kick lamp flashes on each detected kick |
| Graph | The last 64 beats: the live palette as the top strip, the section as a coloured band, the lock as a green line, phrase ticks, and the bass as a white line |
| Audio controls | Audio source, Input gain, Kick sensitivity, Fallback BPM, Phrase length |
| Beat matching | Tap tempo, Phrase is now, Lock BPM, `< Nudge` / `Nudge >`, Force breakdown, Drop now |

| Button | What it does |
|---|---|
| Tap tempo | Sets the BPM from your taps and holds it; two or more taps inside a few seconds are averaged |
| Lock BPM | Toggles the tempo hold: locks the clock to the running (or tapped) BPM instead of following the analysis, and toggles it off again |
| Nudge | Shifts the beat phase a small step earlier (`< Nudge`) or later (`Nudge >`) |
| Phrase is now | Re-anchors the phrase and bar to this beat (fixes a grid that is off by a bar) |
| Force breakdown | Switches the section to BREAKDOWN now; Clip Director and Synesthesia follow through the clock |
| Drop now | Switches the section to DROP, on the next beat |

The beat-matching controls sit in a grid: **Tap tempo**, **Phrase is now** and **Lock BPM**
on the top row; **`< Nudge`**, **`Nudge >`** and **Force breakdown** below them; **Drop
now** on its own row. Nudge is the two arrow buttons only, with no numeric field. Every one
of these has a matching hotkey (see [Hotkeys](#hotkeys)), and the Music dock buttons and the
hotkeys do the same thing.

## Hotkeys

Set them in File > Settings > Hotkeys. Each Palette Lock filter has its own set, labelled
with the filter name in brackets.

| Action | Default |
|---|---|
| Force off (hold) | F13 |
| Force on (toggle) | F14 |
| Pause / resume director | F15 |
| Rotate colours now | F16 |
| PANIC (toggle) | F17 |
| Next palette | F18 |
| Previous palette | F19 |
| Palette 1-8 go live | Ctrl+1 ... Ctrl+8 |
| Phrase is now | F20 |
| Drop now | F21 |
| Force breakdown | F22 |
| Tap tempo | F23 |
| Lock BPM | F24 |
| Nudge beat phase back | F9 |
| Nudge beat phase forward | F10 |

Palettes 9-16 have no hotkeys: tap their pads in the Palette Director dock, or use
Next / Previous palette.

The last seven rows are the Music dock's clock commands; they are registered per filter
like the rest, and the dock buttons trigger the same action.

## Install your own set

A set is the palettes plus the look, change rules and lock behaviours. It is stored
inline in the settings of each Palette Lock filter, so a scene collection keeps working
even if a set file goes missing.

To keep a set outside OBS:

1. In the Palette Director dock, Set tab, press Save. The set is written to
   `Documents\PaletteDirector\sets\<name>.json`.
2. Every readable `*.json` file in that folder shows up in the Set list.
3. Put a set file there by hand (copy one from another machine, or export one) and it
   appears in the list after the list is refreshed; press "Load file..." to import one
   from anywhere, for example `D:\PaletteSets\night-drive.json`. Loading copies it into
   the set folder.

A set file is JSON, schema 2. A minimal one:

```json
{
  "schema": 2,
  "name": "Night Drive",
  "synesthesia_playlist": "Neon Default",
  "palettes": [
    {
      "name": "Neon Grid",
      "mode": "gradient_map",
      "colours": ["#05010D", "#2A0A4A", "#FF00A0", "#00F0FF", "#E8FDFF"]
    }
  ],
  "colour": {
    "strength": 1.0,
    "contrast": 1.2,
    "keep_brightness": 0.0,
    "input_black": 0.0,
    "input_white": 1.0,
    "brightness_source": "perceived"
  },
  "palette_changes": {
    "trigger": "phrases",
    "every_phrases": 8,
    "chance": 0.25,
    "order": "shuffle",
    "crossfade_beats": 1,
    "breakdown_palette": "Neon Grid"
  },
  "director": {
    "enabled": true,
    "phrase_beats": 32,
    "lock_fade_beats": 1,
    "fallback_bpm": 132,
    "random_off": {"enabled": true, "chance": 0.12, "bars_min": 1, "bars_max": 2, "cooldown_phrases": 3},
    "off_on_breakdown": false,
    "off_on_drop": {"enabled": true, "bars": 1},
    "breathing": 0.0,
    "rotation": false,
    "seed": 2077,
    "input_gain_db": 0,
    "kick_sensitivity": 0.6,
    "drop_cycle": {"enabled": true, "chance": 0.5, "bars": 1, "rate": "1/8"}
  }
}
```

`mode` is `gradient_map` or `duotone` (duotone uses the first and last colour);
`brightness_source` is `perceived`, `max` or `average`; `palette_changes.trigger` is
`drop`, `phrases`, `random` or `manual`; `order` is `sequence` or `shuffle`;
`phrase_beats` is 16 or 32; `breakdown_palette` is resolved by name and must exist in the
set. The bundled set is `data/sets/neon-default.json` in this repo, and
it is also installed next to the plugin.

## Beat clock API (for other plugins)

Palette Director registers two procedures on the global OBS proc handler while it is
loaded. They are plain calldata calls, so C/C++ plugins and OBS scripts (Lua/Python,
`obs.proc_handler_call`) can both use them. The consumer header is
[include/pd-clock-api.h](include/pd-clock-api.h); it is header-only, so copy it into your
project.

| Procedure | Purpose |
|---|---|
| `palette_director_instances` | Lists the Palette Lock instances: `[{"id":1,"source":"Spout In","filter":"Palette Lock"}]` |
| `palette_director_clock` | Reads one instance: beat position, tempo, confidence, phrase, section, kicks, drop cycle, silence, pause |

Instance selector, in order: `id` greater than 0, else the parent source name (optionally
with the filter name), else the filter name, else the oldest instance. A selector that
matches nothing returns `ok = false`.

| Field | Type | Meaning |
|---|---|---|
| `beats` | float | Beat position at `sample_ts_ns` (absolute, fractional) |
| `beats_now` | float | Beat position extrapolated to the call time, capped at 0.5 s; frozen while paused |
| `bpm` | float | Tempo of the beat grid; the fallback BPM while the feed is silent |
| `confidence` | float | 0 to 1 |
| `beat` | int | Integer beat |
| `phrase` | int | Phrase counter |
| `phrase_beats` | int | 16 or 32 |
| `section` | int | 0 groove, 1 breakdown, 2 drop |
| `kicks` | int | Monotonic kick counter; a change means a kick |
| `drop_cycle` | bool | A drop colour cycle is running |
| `drop_cycle_steps_per_beat` | int | 1, 2 or 4 |
| `silent` | bool | Feed silent, fallback BPM in use |
| `paused` | bool | Director paused or PANIC latched |
| `enabled` | bool | Director enabled in the set |
| `filter_enabled` | bool | The Palette Lock filter itself is enabled in OBS |
| `sample_ts_ns` | int | The audio timestamp `beats` refers to, mapped onto the wall clock |
| `ts_source` | int | 1 = from audio timestamps, 0 = wall clock |
| `now_ns` | int | `os_gettime_ns()` at the call; `beats_now` refers to this |

```c
#include "pd-clock-api.h"

struct pd_clock c;
if (pd_clock_read(0, NULL, "Palette Lock", &c)) {
        double now = c.beats_now;      /* when the next beat falls: floor(now) + 1 */
        int section = c.section;       /* PD_CLOCK_SECTION_DROP while the drop runs */
}
```

Notes:

- Fields are versioned by name: new fields may appear in later versions, existing ones
  keep their meaning. Check `api_version >= PD_CLOCK_API_VERSION`.
- The snapshot is refreshed about every 5 ms. The call takes two short mutexes, never
  blocks on audio or rendering, and is safe from any thread except inside a Palette Lock
  filter's own callbacks. Call it from `video_tick`, not per pixel.
- Clip Director uses this API; its copy of the header is `src/pd-clock-api.h`.

## Build from source

The project is an OBS plugin template layout: `buildspec.json` names the plugin, its
version and the dependency versions, and `CMakePresets.json` holds the presets.

| Requirement | Version |
|---|---|
| CMake | 3.28 or newer |
| Windows | Visual Studio 17 2022 (desktop C++), Windows 10 SDK 10.0.20348 or newer |
| Linux (unit tests) | a C++17 compiler, Ninja, pkg-config |

Windows x64:

```
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

The configure step downloads the dependencies pinned in `buildspec.json` (OBS Studio
32.2.2 sources, obs-deps, Qt6) into `.deps/` and checks their SHA256 hashes.

Unit tests (libobs-free, run anywhere with CMake):

```
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Only the Windows x64 packages are released; the macOS and Ubuntu presets are kept from
the template for development.

## Licence

GPL-2.0. See [LICENSE](LICENSE).

- Suite, install and changelog: <https://github.com/EnZhm/director>
- Clip Director (clips and Synesthesia scenes on the same clock):
  <https://github.com/EnZhm/clip-director>
- Changelog: [CHANGELOG.md](CHANGELOG.md) | Install: [release/INSTALL.txt](release/INSTALL.txt)
