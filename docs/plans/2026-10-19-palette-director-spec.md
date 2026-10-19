# Palette Director — Implementation Spec + Card Graph (P1–P6)

> **Status:** draft for Ian's approval. Not an implementation plan for code; it is the spec that the P1–P6 implementer cards are cut from.
> **Repo:** `EnZhm/obs-palette-director` (private), a fresh copy of `obsproject/obs-plugintemplate` at `~/Projects/obs-palette-director`.
> **Authoritative inputs:** design doc `E - Vault/Projects/Palette Director/Palette Director - Design.md` (ratified 2026-10-19, rewritten) and UI mock v2 `E - Vault/Projects/Palette Director/Palette Director UI - Mock.html`. The mock v2 is authoritative for the filter's properties layout, set lifecycle, the two recolour modes, and the reference shader maths. Where the design doc and mock disagree, the mock v2 wins for UI/maths; the design doc wins for scheduling and threading.

---

## 1. Fixed decisions (stamped, do not re-litigate)

| # | Decision |
|---|---|
| D1 | Native C++ OBS plugin; one video filter **"Palette Lock"** (`OBS_SOURCE_TYPE_FILTER`), one embedded **Director** per filter instance, plus one **Palette Director dock** (Qt, frontend API). |
| D2 | Target: Windows x64, OBS **32.2.2** (VJ machine). Linux build required for the dev loop on TechnoCore (Ubuntu 26.04). macOS out of scope. |
| D3 | Visual source: Synesthesia → Spout2 source in OBS. The filter attaches to the Spout source. |
| D4 | Audio: DJ mixer line feed into the OBS machine, read with `obs_source_add_audio_capture_callback`. The Director derives all timing (BPM, phase, bars, sections, breakdown/drop) from that feed; it takes **no** OSC or beat data from Synesthesia. |
| D5 | A **performance set** holds **1–8 named palettes** (2–8 colours each); **one is live at a time**. Stored as a JSON set file, **schema 2** (`Documents\PaletteDirector\sets\`). The breakdown palette is referenced **by name**. |
| D6 | The Director core is **pure C++ with no libobs dependency**, so offline WAV-replay tests drive it. Only the filter + shim + dock layers depend on libobs/Qt. |
| D7 | Recolour **modes** (exactly two, chosen **per palette**): **Gradient map** (default) and **Duotone**. Posterize and hue-remap are dropped. |
| D8 | Every on/off change fades through `mix`; never a hard cut. Palette changes crossfade via shader `pfade` (0→1), so gradient↔duotone changes blend smoothly. |
| D9 | Deterministic PRNG = **mulberry32** (matches the mock), seeded per set. Same seed ⇒ same random-off pattern and same shuffle order. |
| D10 | The Director owns all timing/scheduling; the dock is a **view + command surface only** — closing it never affects the Director. |

---

## 2. P1 — Scaffold: bump to OBS 32.2.2, rename, enable frontend API + Qt, CI

### 2.1 How `buildspec.json` is consumed (verified in this repo)

- `cmake/common/buildspec_common.cmake::_check_dependencies` reads `buildspec.json`, iterates `dependencies_list`, and downloads each with `file(DOWNLOAD ... EXPECTED_HASH SHA256=<hash>)`. For `obs-studio` the URL is `${baseUrl}/${file}` (a GitHub source archive); for `prebuilt`/`qt6` it is `${baseUrl}/${version}/${file}` (obs-deps releases).
- `cmake/windows/buildspec.cmake` sets `dependencies_list = prebuilt qt6 obs-studio` and `platform = windows-x64`. Filenames: `windows-deps-VERSION-x64.zip` (prebuilt), `windows-deps-qt6-VERSION-x64.zip` (qt6), `VERSION.zip` (obs-studio). No `revision` is set, so `buildspec_common.cmake` strips the `-REVISION`/`_REVISION` token.
- **Linux does not read `buildspec.json`.** There is no `cmake/linux/buildspec.cmake`; Linux builds against system libobs from the OBS PPA (`.github/scripts/utils.zsh/setup_ubuntu` → `ppa:obsproject/obs-studio`, packages `obs-studio`, `libsimde-dev`, `libgles2-mesa-dev`, `qt6-base-dev` + `qt6-base-private-dev`). So the buildspec bump affects Windows (and macOS, left untouched) only.

### 2.2 Source of truth for the 32.2.2 dependency pin (verified)

`obs-studio` tag `32.2.2`, `CMakePresets.json`, the `dependencies` preset under `vendor.obsproject.com/obs-studio.dependencies` pins the obs-deps / Qt6 versions and hashes OBS 32.2.2 itself was built against:

```
prebuilt (obs-deps)  version "2026-07-15"
  windows-x64  sha256 6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9
qt6                  version "2026-07-15"
  windows-x64  sha256 7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579
  debugSymbols.windows-x64 sha256 471d0b2191c424a520a51d064f3741084f117e1ff6ee5af1d16aabcdbacc6659
```

### 2.3 Exact `buildspec.json` edits for P1

- `dependencies.obs-studio.version` → `"32.2.2"`.
- `dependencies.obs-studio.hashes.windows-x64` → sha256 of `https://github.com/obsproject/obs-studio/archive/refs/tags/32.2.2.zip`. **Compute once at P1 with `curl -sSL https://github.com/obsproject/obs-studio/archive/refs/tags/32.2.2.zip | sha256sum`** and paste the hex digest. (obs-studio does not download its own source, so there is no pre-published source-archive hash to copy; CI's `EXPECTED_HASH` check fails loudly if wrong, so it is self-verifying.)
- `dependencies.prebuilt.version` → `"2026-07-15"`; `dependencies.prebuilt.hashes.windows-x64` → `6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9`.
- `dependencies.qt6.version` → `"2026-07-15"`; `dependencies.qt6.hashes.windows-x64` → `7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579`; `dependencies.qt6.debugSymbols.windows-x64` → `471d0b2191c424a520a51d064f3741084f117e1ff6ee5af1d16aabcdbacc6659`.
- **Leave the `macos` keys alone** (out of scope; `cmake/macos/buildspec.cmake` still references them).
- Plugin metadata: `name` → `"palette-director"`; `displayName` → `"Palette Director"`; `version` → `"0.1.0"`; `author` → `"EnZhm"`; `website` → `"https://github.com/EnZhm/obs-palette-director"`; `email` → Ian's (leave placeholder if unknown); `platformConfig.macos.bundleId` → `"com.enzhm.palette-director"` (cosmetic, macos unused).
- `src/plugin-support.c.in` and `src/plugin-main.c` are already templated off `CMAKE_PROJECT_NAME`/`buildspec.json`, so renaming propagates. Fill `data/locale/en-US.ini` (currently 0 bytes) with the "Palette Lock" filter name and "Palette Director" dock title strings.

### 2.4 Enable the frontend API and Qt (verified — this is the dock's build prerequisite)

- The template already carries the two options, both **OFF by default**: `option(ENABLE_FRONTEND_API ... OFF)` and `option(ENABLE_QT ... OFF)` in `CMakeLists.txt:7-8`, and both forced `false` in the shared `template` configure preset at `CMakePresets.json:13-14`. P1 flips them **ON**.
- **Windows (verified buildable):** with `ENABLE_FRONTEND_API` on, `CMakeLists.txt:20` does `find_package(obs-frontend-api REQUIRED)` and links `OBS::obs-frontend-api`. That config comes from the template's own `cmake/common/buildspec_common.cmake::_setup_obs_studio`, which configures the downloaded obs-studio source with `-DENABLE_PLUGINS=OFF -DENABLE_FRONTEND=OFF`, then builds and installs the `obs-frontend-api` target (`--component Development`). So the frontend API is **already built from the 32.2.2 source on Windows** — P1 only needs the option flip. `ENABLE_QT` pulls `Qt6::Core`/`Qt6::Widgets` from the downloaded Qt6 obs-deps (already in `buildspec.json`).
- **Linux (flag, do not assume):** Linux bypasses `_setup_obs_studio` and uses the PPA. The template's `cmake/linux/defaults.cmake:55-81` attempts `ENABLE_FRONTEND_API` via a `find_path(obs-frontend-api_INCLUDE_DIR ... /usr/include ...)` + `find_library(obs-frontend-api_LIBRARY ...)` fallback (inside the non-CMake `libobs` path). Whether the Ubuntu PPA's `obs-studio`/`libobs-dev` package ships `obs-frontend-api.h` + `libobs-frontend-api.so` is **unverified** — see risk R4. P1's Linux job keeps the dock **conditionally compiled**: the dock TU is `#ifdef`-guarded on `ENABLE_FRONTEND_API && ENABLE_QT`, so if the PPA lacks the frontend API the Linux CI still builds the filter + Director and runs the unit tests, and the TechnoCore dev loop is unaffected.

### 2.5 CI / Linux decision

- **Windows CI:** keep the template's `windows-build` job (`windows-2022`, `Build-Windows.ps1`), now building libobs 32.2.2 + `obs-frontend-api` from source via the bumped buildspec, with `ENABLE_FRONTEND_API`+`ENABLE_QT` ON. This is the pin that matters.
- **Linux CI:** keep the template's `ubuntu-build` job on `ubuntu-24.04` (PPA-based), with `ENABLE_FRONTEND_API`+`ENABLE_QT` ON. It compiles the C++ and runs the offline Director unit tests (which need no OBS). **Decision:** the TechnoCore (Ubuntu 26.04) dev-loop build is **local-only** and documented in `README.md` (obs-studio 32.2.2's own CI builds `ubuntu-24.04` *and* `ubuntu-26.04`, but the PPA frontend-api availability on 26.04 is not guaranteed). Do not add a 26.04 CI job in P1.
- **No macOS job** is added; the template's macos job is left but unused/out of scope.
- **Toolchain risk (flag, do not act in P1):** the template's `CMakePresets.json` targets Visual Studio 17 2022 + SDK `10.0.22621`; obs-studio 32.2.2's own CI uses VS 18 2026 + SDK `10.0.26100` on `windows-2025-vs2026`. Because the template compiles libobs from the 32.2.2 source (headers + ABI match guaranteed), keeping VS2022/windows-2022 is the expected-safe default. Revisit only if the P2 .dll fails to load in OBS 32.2.2 on the VJ machine.

### 2.6 P1 verification

- `cmake --preset windows-ci-x64 && cmake --build --preset windows-ci-x64` green in CI (Windows job, `ENABLE_FRONTEND_API`+`ENABLE_QT` ON).
- `cmake --preset ubuntu-ci-x86_64 && cmake --build --preset ubuntu-ci-x86_64` green in CI (Ubuntu job).
- CI `check-format` job passes (clang-format + gersemi).
- Downloaded obs-studio archive hash matches `buildspec.json` (CI fails on mismatch).
- Empty filter (`Palette Lock`) + empty dock (`Palette Director`) register and load in OBS 32.2.2 without error.

---

## 3. The filter's `source_info`, render path, and thread handoff

### 3.1 `struct obs_source_info` fields (verified against `libobs/obs-source.h` @ 32.2.2)

```c
struct obs_source_info palette_lock_info = {
    .id          = "palette_lock_filter",
    .type        = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
    .get_name    = palette_lock_get_name,      // "Palette Lock" (locale)
    .create      = palette_lock_create,
    .destroy     = palette_lock_destroy,
    .get_defaults = palette_lock_defaults,
    .get_properties = palette_lock_properties,
    .update      = palette_lock_update,
    .load        = palette_lock_load,          // scene-collection restore (schema 2)
    .save        = palette_lock_save,
    .filter_remove = palette_lock_filter_remove, // detach audio callback
    .filter_add  = palette_lock_filter_add,
    .activate    = palette_lock_activate,
    .deactivate  = palette_lock_deactivate,
    .video_tick  = palette_lock_tick,          // advance fade + poll Director state
    .video_render = palette_lock_render,
};
obs_register_source(&palette_lock_info);
```

Register in `obs_module_load()` (replace the template's empty `plugin-main.c` body); also register the dock (§8).

### 3.2 Render path (`.effect` shader, A/B palette crossfade)

`palette_lock_render(void *data, gs_effect_t *effect)` uses the standard filter pipeline:

```c
if (!obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
    return;
// set effect params: palA[8], nA, modeA, palB[8], nB, modeB, pfade,
//                    amt (strength*breathing*mix), contrast, preserve
// obs_source_process_filter_end(filter->context, effect, 0, 0);
```

Verified names in `libobs/obs-source.c` @ 32.2.2: `obs_source_process_filter_begin` / `obs_source_process_filter_begin_with_color_space` / `obs_source_process_filter_tech_end` / `obs_source_process_filter_end`, and `obs_source_get_frame` for the parent (Spout) frame. The effect file `data/palette_lock.effect` is HLSL; OBS translates it for D3D11 and OpenGL.

**Uniforms** (authoritative from mock v2 `FS`): `float3 palA[8]; int nA; int modeA; float3 palB[8]; int nB; int modeB; float pfade; float amt; float contrast; float preserve;`. The rotation offset is applied CPU-side by rotating the uploaded colour array (mock's `rotated()`), never in the shader.

### 3.3 The two recolour modes (authoritative maths from mock v2 `lockColour`, GLSL)

Input: `float luma(src)` (Rec.709), contrast pre-map `l = clamp((luma-0.5)*contrast+0.5, 0, 1)`. `lumaMatch(c,l) = c*(l/max(luma(c),0.002))`.

| mode int | name | `mapX(l)` maths |
|---|---|---|
| 0 | **Gradient map** | `x = l*(nX-1); i = floor(x); m = mix(palX[i], palX[i+1], x-i)` (linear blend between neighbours) |
| 1 | **Duotone** | `m = mix(palX[0], palX[nX-1], smoothstep(0,1,l))` (first colour = shadows, last = highlights; middle colours ignored) |

Crossfade: `m = mix(mapA(l), mapB(l), pfade)`; then `m = mix(m, lumaMatch(m,l), preserve)`. Final `col = mix(src, m, amt)` with `amt = strength × breathing × lock_mix`. `modeX` is **per palette**, so a gradient→duotone change is itself a smooth `pfade` blend.

### 3.4 Properties layout (mirror mock v2 right column)

Section → controls (all via `obs_properties_*`):

1. **Performance set** — set selector (list), New / Duplicate / Rename / Delete / Save / **Load file…**, Synesthesia playlist (text), path readout. Dirty tracking; Delete falls back to the previous set (mock `setDel`).
2. **Palettes** — list with live marker (●) and per-row breakdown tag + `grad`/`duo` tag; editor for the selected palette: **Name**, **Mode** (dropdown: Gradient map | Duotone), ramp preview, 2–8 swatches (hex + colour picker + up/down/remove), **Add colour**, **Sort dark→bright**, **Reverse**; palette actions: **Go live**, move up/down, Duplicate, Rename, Delete. Duotone dims the middle swatches (unused). Max 8 palettes; min 1 (Delete disabled at 1).
3. **Colour** — Strength (0–1), Contrast in (0.5–2, default 1.1), Keep brightness (0–1, default 0). Applies to every palette.
4. **Palette changes** — Change on (dropdown: Every drop | Every N phrases | Random per phrase | Manual only), Every N (1–16, default 4, shown only for "Every N"), Chance/phrase (0–1, default 0.25, shown only for "Random"), Order (In list order | Shuffle), Crossfade (0–16 beats, default 2, 0 = cut), Breakdown palette (dropdown: None + palette names, referenced by name).
5. **Director** — Enabled (bool), Audio source (list of audio-capable sources, default "Mixer"/first), Fallback BPM (60–200, default 128), Phrase length (16|32, default 32), Lock fade (0.25–8 beats, default 1).
6. **Lock behaviours** — Random off (bool, default on) + Chance/phrase (0–1, 0.15), Bars off min (1–8, 1), Bars off max (1–16, 4), Cooldown (0–8 phrases, 2); Off on breakdown (off); Off on drop (off) + Raw bars at drop (1–8, 2); Strength breathing (0–1, 0); Colour rotation (off); Seed (0–99999, 7, + reroll).
7. **Hotkeys** — read-only listing (bindings set in OBS Settings → Hotkeys), matching §7.2.

Defaults table is the mock v2 `DEF_DIR` / `DEF_COL` (copy exactly).

### 3.5 Audio capture lifecycle + thread handoff

- In `filter_add` / `create`, resolve the chosen audio source (weak ref) and call `obs_source_add_audio_capture_callback(src, on_audio, filter)`. In `filter_remove`/`destroy`, `obs_source_remove_audio_capture_callback` then `obs_weak_source_release`.
- Verified callback shape (`libobs/obs-source.c` @ 32.2.2): the callback runs on the **audio thread under `audio_cb_mutex`** and is `void (*obs_source_audio_capture_t)(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)`.
- `struct audio_data` (verified `libobs/media-io/audio-io.h` @ 32.2.2) is `{ uint8_t *data[MAX_AV_PLANES]; uint32_t frames; uint64_t timestamp; }`. Audio is float-planar; sample rate/layout are NOT in `audio_data` — obtain the sample rate from `audio_output_get_sample_rate(obs_get_audio())` (or the source's `sample_info`) and downmix to mono in the callback using `get_audio_channels()`/`get_audio_planes()` from `audio-io.h`.
- The callback must not block: copy the mono frame into a lock-free ring buffer / SPSC queue and return. The Director consumes the ring on the audio thread (or a dedicated thread) and publishes a `DirectorState` snapshot via `std::atomic` / seqlock; `palette_lock_tick` (render thread) reads the snapshot to drive the fade, and `palette_lock_render` sets the shader `amt` and the A/B palette uniforms + `pfade`.

---

## 4. Director: module boundaries, public API, test strategy

### 4.1 Module layout (pure C++, no libobs / no Qt)

```
src/director/          # pure C++ — no obs headers
  director.h           # public API + config/state/SetFile structs (below)
  palette.h            # Colour, PaletteSpec (name, mode, 2-8 colours)
  analysis.h/.cpp      # band energy (biquad bass<150, mid 150-2k, high>2k), onset detection
  beat_tracker.h/.cpp  # BPM (autocorr 90-180) + soft phase-lock
  section_state.h/.cpp # GROOVE / BREAKDOWN / DROP from bass presence
  scheduler.h/.cpp     # palette-change scheduler + lock behaviours -> mix target + pfade
  command_queue.h      # lock-free SPSC command queue (dock -> Director)
  rng.h                # mulberry32
src/                   # libobs-bound shim only
  palette-lock-filter.c/h    # the filter + audio capture + thread handoff
  palette-lock-host.h/.cpp   # per-instance runtime: Director + snapshot + timeline/log rings + registry
  palette-director-dock.cpp/.h  # Qt dock (frontend API + Qt), AUTOMOC'd
```

### 4.2 Public API

```cpp
namespace pd {

enum class Section { Groove, Breakdown, Drop };
enum class RecolourMode { GradientMap = 0, Duotone = 1 };
enum class ChangeTrigger { Drop, Phrases, Random, Manual };
enum class ChangeOrder { Sequence, Shuffle };

struct Colour { float r, g, b; };                       // 0..1
struct PaletteSpec {
    std::string name;
    RecolourMode mode = RecolourMode::GradientMap;
    std::vector<Colour> colours;                        // 2..8
};
struct ColourSettings { float strength = 1.0f, contrast = 1.1f, keep_brightness = 0.0f; };
struct PaletteChanges {
    ChangeTrigger trigger = ChangeTrigger::Drop;
    int   every_phrases = 4;                            // Phrases trigger
    float chance = 0.25f;                               // Random trigger
    ChangeOrder order = ChangeOrder::Sequence;
    float crossfade_beats = 2.0f;                       // 0 = cut
    std::string breakdown_palette;                      // "" = none; resolved BY NAME
};
struct DirectorConfig {
    int    phrase_beats = 32;
    float  fade_beats   = 1.0f;
    float  fallback_bpm = 128.0f;
    bool   random_off   = true;
    float  random_chance= 0.15f;
    int    random_bars_min = 1, random_bars_max = 4, cooldown_phrases = 2;
    bool   off_on_breakdown = false, off_on_drop = false;
    int    drop_raw_bars = 2;
    float  breathing   = 0.0f;
    bool   rotation    = false;
    uint32_t seed      = 7;
};
struct SetFile {
    std::string name;
    std::string synesthesia_playlist;
    std::vector<PaletteSpec> palettes;                  // 1..8
    ColourSettings colour;
    PaletteChanges palette_changes;
    DirectorConfig director;
};
struct DirectorState {                                  // lock-free snapshot for the render thread
    float bpm, confidence, bass, mix_target;            // mix_target in [0,1]
    int   rotation_offset;
    Section section;
    uint64_t beat, phrase;
    int    live_palette;                                // index into SetFile::palettes
    int    prev_palette;                                // outgoing palette, -1 if none
    float  pfade;                                       // 0..1 crossfade progress
    int    cooldown_phrases_left;
    bool   lock_on;
};
enum class Command { ForceOn, ForceOff, ReleaseForce, Pause, Resume, Next, Prev, GoLive, RotateNow, Panic };

class Director {
 public:
    explicit Director(const SetFile&);
    void push_audio(const float* mono, size_t frames, uint32_t sample_rate); // audio/WAV thread
    DirectorState snapshot() const;                                          // render thread
    void push_command(Command, int palette_index = -1);                      // UI thread (SPSC)
    void apply_set(const SetFile&);                                          // re-seed mulberry32
};
}
```

Pipeline: `analysis → beat_tracker → section_state → scheduler`. Confidence-gated: below threshold or silent feed ⇒ fall back to `fallback_bpm`. Fade model (from mock): `rate = (bpm/60)/max(0.25, fade_beats); mix += sign(target-mix)*min(|target-mix|, rate*dt)`. Crossfade model: `pfade += (bpm/60)*dt / max(0.01, crossfade_beats)` clamped to 1.

**Palette-change scheduler (mock v2 `pickNext`/`goLive`/`onSectionChange`/`phraseBoundary` semantics):**
- Rotation pool = all palettes **except** the breakdown palette (mock `normalIdx`).
- `Drop` trigger → `goLive(pickNext(), "drop")` on each DROP; if a breakdown palette was active, the next normal palette lands on the drop's downbeat.
- `Phrases` trigger → change on `phrase % every_phrases == 0`.
- `Random` trigger → change when `rng() < chance` at a phrase boundary.
- `Manual` → only via command/hotkey.
- `Shuffle` order → a seeded permutation with **no repeats until every palette has played** (mock `S.order`); `Sequence` → list order. Breakdown palette is left out of the rotation.
- `BREAKDOWN` start → if `breakdown_palette` set, go live on it; if `off_on_breakdown`, fade lock out instead (takes precedence). On the DROP, restore the normal rotation.

### 4.3 Test strategy (justified)

- **Runner: doctest** (single header, `third_party/doctest/doctest.h`, `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` in one TU). Justification: zero build/toolchain deps (vs Catch2's heavier build), vendored so no network/package install, SECTION/SUBCASE fits event-log assertions. A hand-rolled assert runner is the fallback if vendoring is unwanted, but doctest costs nothing here.
- **WAV fixtures without committing copyrighted audio:** commit a pure-C++ generator `tests/wav_gen.cpp` that synthesizes deterministic click/kick loops at a target BPM (16-bit PCM WAV writer, no deps) and writes fixtures into a temp dir at test time. Tests assert: BPM within ±1 of the known value, phrase boundaries on the downbeat, same seed ⇒ identical event log, no off-window during cooldown, shuffle never repeats within a cycle, palette-change triggers fire exactly as specified, breakdown palette goes live on breakdown and the next normal palette lands on the drop.
- **Real-track tests (local only):** a doctest group gated on env `PALETTE_REAL_TRACKS_DIR` (skipped via `DOCTEST_SKIP` when unset). Never committed; Ian points it at his own WAVs on TechnoCore.
- CI runs only the synthetic-fixture suite (`ctest --test-dir build_... --output-on-failure`), so the repo stays free of copyrighted audio.

---

## 5. Set files (schema 2) + hotkeys

### 5.1 Set file load/save (filter-side, uses libobs obs_data JSON)

- Path: Windows `%USERPROFILE%\Documents\PaletteDirector\sets\<slug>.json`; Linux dev `$XDG_CONFIG_HOME/PaletteDirector/sets/<slug>.json`.
- I/O via `obs_data_create_from_json_file` / `obs_data_save_json_pretty` (filter owns persistence; the Director only receives the parsed `SetFile`). The breakdown palette is referenced **by name**, so reordering never breaks it. Schema 2 (verbatim from the design doc):

```json
{
  "schema": 2,
  "name": "Warehouse 2026-11-07",
  "synesthesia_playlist": "Techno Nov",
  "palettes": [
    { "name": "Night Neon",    "mode": "gradient_map", "colours": ["#0B0B12", "#3A0CA3", "#F72585", "#4CC9F0"] },
    { "name": "Infrared",      "mode": "gradient_map", "colours": ["#07070A", "#5A0000", "#FF2A00", "#FFD166"] },
    { "name": "Ice",           "mode": "gradient_map", "colours": ["#05060A", "#0D3B66", "#7FDBFF", "#F0FBFF"] },
    { "name": "Breakdown Duo", "mode": "duotone",      "colours": ["#0A0A12", "#C9C9FF"] }
  ],
  "colour": { "strength": 1.0, "contrast": 1.1, "keep_brightness": 0.0 },
  "palette_changes": {
    "trigger": "drop",
    "every_phrases": 4,
    "chance": 0.25,
    "order": "sequence",
    "crossfade_beats": 2,
    "breakdown_palette": "Breakdown Duo"
  },
  "director": {
    "enabled": true,
    "phrase_beats": 32,
    "lock_fade_beats": 1,
    "fallback_bpm": 128,
    "random_off": { "enabled": true, "chance": 0.15, "bars_min": 1, "bars_max": 4, "cooldown_phrases": 2 },
    "off_on_breakdown": false,
    "off_on_drop": { "enabled": false, "bars": 2 },
    "breathing": 0.0,
    "rotation": false,
    "seed": 7
  }
}
```

Lifecycle rules (mock v2 `sets lifecycle`): at least one set always exists; deleting the current set (after confirm) falls back to the previous; switching with unsaved changes asks for confirm; deleting the live palette hands over to the next; deleting the breakdown palette sets breakdown to None; reordering keeps live/breakdown references (they point at a palette, not a position).

### 5.2 Hotkeys (per filter instance)

Register in `create()` with `obs_hotkey_register_source(filter_source, name, desc, cb, filter)`, unregister in `destroy()` with `obs_hotkey_unregister`. Registerer type is implicitly `OBS_HOTKEY_REGISTERER_SOURCE` (verified `libobs/obs-hotkey.h` @ 32.2.2), so bindings are per-source and saved with the scene collection. Callbacks push `Command`s onto the Director's SPSC queue (no UI-thread work).

Eight named actions + palette 1–8 (defaults from mock v2 hotkey table):

| Hotkey | Default | Command |
|---|---|---|
| Force off (hold) | F13 | `ForceOff`/`ReleaseForce` |
| Force on (toggle) | F14 | `ForceOn` (toggle) |
| Pause / resume director | F15 | `Pause`/`Resume` |
| Next palette | F18 | `Next` |
| Previous palette | F19 | `Prev` |
| Palette 1–8 go live | Ctrl+1…8 | `GoLive(i)` |
| Rotate colours now | F16 | `RotateNow` |
| PANIC (lock on, director paused) | F17 | `Panic` |

---

## 6. Palette Director dock (P5)

A Qt widget registered through the OBS frontend API. **Verified:** `EXPORT bool obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget);` — `widget` is a `QWidget*`, returns `false` if the id was already used — and `EXPORT void obs_frontend_remove_dock(const char *id);`, both in **`frontend/api/obs-frontend-api.h` at the 32.2.2 tag** (the header moved from the old `UI/obs-frontend-api/` path; the UI tree is now `frontend/`). Also available for the instance picker: `obs_frontend_get_scenes` / `obs_frontend_get_current_scene_collection`. Registration is `obs_frontend_add_dock_by_id("palette-director-dock", "Palette Director", dockWidget)` from `obs_module_load()`.

The dock (mirroring mock v2's dock panel):

- **Instance picker:** a dropdown listing every Palette Lock filter in the scene collection (source name / filter name), fed from a plugin-global registry of filter instances (`palette-lock-host.cpp` appends on `create`, removes on `destroy`, under a light mutex). Selecting one attaches the dock to that instance's host.
- **Status:** section chip (GROOVE / BREAKDOWN / DROP), lock state (ON / OFF / FADING), director state (running / paused / forced off / off), beat lights, BPM, confidence, bar·phrase, lock mix, cooldown.
- **Palette pads:** one per palette, showing its ramp, mode (`grad`/`duo`), breakdown tag and Ctrl+N hint. Live pad outlined with a crossfade progress bar; next pad dashed (mock `pickNextPreview`). Tap ⇒ `GoLive(i)`.
- **Controls:** force off (hold), force on, pause/resume, prev/next palette, rotate colours, **PANIC**.
- **Timeline:** last 64 beats — live palette strip, section band, lock-mix line, bass envelope, phrase ticks (mock `drawTimeline`).
- **Log:** the Director's decisions with reasons (e.g. `palette → Ice (drop, 2-beat crossfade)`, `random off (0.08 < 0.15) for 3 bars`).

**Threading (verified design):** the filter publishes a `DirectorState` snapshot (seqlock or double-buffer swapped under a light mutex) plus fixed-size ring buffers for the timeline and log. The dock polls them on a `QTimer` at ~30 Hz. Dock commands go into the Director's lock-free SPSC queue (`command_queue.h`), which the Director drains on its thread. Closing the dock never affects the Director (D10). Text uses `obs_frontend_push_ui_translation`/`obs_frontend_pop_ui_translation` + `obs_module_text` (per the header's translation note).

---

## 7. Card graph P1–P6

Routing: implementer = `changer`, reviewer = `doorkeeper`. Effort in the roke scale (S/M/L). P0 (mode pick) is **closed** (Gradient map + Duotone). All six are hard requirements for v1.0.0 (`hard = yes`); the two human GO/NO-GO gates are called out separately and are **not** implementation cards.

| # | Title | Depends on | Effort | Acceptance (summary) | hard |
|---|---|---|---|---|---|
| P1 | Scaffold + CI: bump OBS 32.2.2, rename, enable frontend-API + Qt | — | S | buildspec bumped (§2.3); `ENABLE_FRONTEND_API`+`ENABLE_QT` ON; plugin "Palette Director" / filter "Palette Lock"; Windows + Ubuntu CI green; check-format green; empty filter+dock load in OBS 32.2.2. | yes |
| P2 | Palette Lock filter: 2 modes per palette, A/B crossfade, manual go-live | P1 | M | `.effect` ports mock v2 `lockColour` (gradient + duotone) with `palA/palB/nA/nB/modeA/modeB/pfade`; properties UI matches mock v2 (§3.4); manual palette switching + strength visibly re-colour the Spout source; Windows zip loads in OBS 32.2.2. | yes |
| P3 | Director core + offline WAV tests | — (parallel to P2) | M | §4 pipeline + palette-change scheduler implemented; doctest suite green on synthetic click/kick fixtures; BPM ±1, phrase-on-downbeat, seed-determinism, shuffle-no-repeat, cooldown, breakdown-palette assertions pass. | yes |
| P4 | Wire Director → filter: audio capture, behaviours, hotkeys | P2, P3 | M | audio → ring → Director; behaviours + fade drive `amt`; 8 hotkeys + palette 1–8 work; panic holds lock on + paused. | yes |
| P5 | Palette Director dock (snapshot/command plumbing, status, pads, controls, timeline, log) | P2, P4 | M | `obs_frontend_add_dock_by_id` registers the dock; instance picker lists filters; dock drives + mirrors a live filter at ~30 Hz; closing/reopening the dock does not disturb the Director. | yes |
| P6 | Set files (schema 2) load/save + INSTALL.txt + release bundle | P2, P5 | S | §5.1 schema-2 round-trips; breakdown palette resolved by name; OBS restart restores last set + live palette; INSTALL.txt + Windows zip in `release/`; show-ready v1.0.0. | yes |

Proposed human GO/NO-GO gates (for the orchestrator to create **unassigned + blocked**, pinned to Opus, **not** implementer cards):
- **G1** after P2 — load the Windows zip on the VJ machine's Spout source; confirm manual lock + palette switching render correctly and the filter shows in the Stats dock (render < 1 ms target). GO/NO-GO before P4/P5 proceed.
- **G2** after P6 — 2-hour VJ-machine soak with the mixer feed + dock open; GO/NO-GO to tag v1.0.0.

*(These are proposed cards only — not created by this task. The implementer cards are cut from this table by the orchestrator after Ian approves.)*

---

## 8. Acceptance tests (final, cross-cutting — pulled from the design doc §Acceptance tests)

1. Mixer silent → lock held at fallback BPM; no flicker, no random off, no palette changes except manual.
2. 128 BPM track → Director reports 128 ± 1; phrase boundaries on downbeats.
3. Random off at chance 1.0 → off at the very next phrase boundary; next off waits out cooldown.
4. Panic mid-off or mid-crossfade → lock + live palette settle within fade time; Director stops changing.
5. "Off on breakdown" → lock fades out within 2 bars, returns on the drop's downbeat.
6. Trigger "Every drop", 3 normal palettes + a breakdown palette → breakdown palette live within 2 bars, next normal palette on the drop's downbeat, crossfade finishes in the set beats.
7. Trigger "Every N phrases" (N=2) → change on every 2nd phrase boundary in list order; Shuffle plays every palette once before any repeat.
8. Deleting the live palette → next palette takes over, no black frame; deleting the breakdown palette → breakdown set to None.
9. Dock: tapping a pad → goes live with crossfade; dock mirrors the selected instance; close/reopen doesn't interrupt the Director.
10. Ctrl+1…8, next/prev, force off/on, pause, panic all work from hotkeys and from the dock.
11. Set file loaded → all palettes, modes, settings apply; Save round-trips to an identical file.
12. Spout 1080p60 → filter render < 1 ms (Stats dock, VJ GPU; target).
13. OBS restarts → filter returns with last set, live palette and settings.

---

## 9. Residual risks

- **R1 Toolchain drift** (§2.5): VS2022 vs VS2026 ABI. *Mitigation:* build libobs from 32.2.2 source (ABI matched); revisit only on a real load failure. *Status:* bounded, not eliminated.
- **R2 PPA version on TechnoCore 26.04** not pinned to 32.2.2. *Mitigation:* Linux is a dev-loop/compile+unit-test target; Windows is the pinned target. *Status:* bounded.
- **R3 `struct audio_data` carries no rate/layout** — downmix must source rate/layout from `audio_output_get_sample_rate`/`sample_info`. *Mitigation:* documented §3.5. *Status:* eliminated by design.
- **R4 Linux frontend-API availability** (§2.4): the Ubuntu PPA may not ship `obs-frontend-api.h`/`libobs-frontend-api.so`, which would block the dock build on Linux CI. *Mitigation:* dock TU `#ifdef`-guarded on `ENABLE_FRONTEND_API && ENABLE_QT`; Linux CI falls back to filter+Director+unit-tests; Windows (the real target) is unaffected because the template builds `obs-frontend-api` from source. *Status:* bounded — P5's first step is to confirm/deny PPA frontend-api on ubuntu-24.04.
- **R5 Source-archive sha256** (§2.3) computed at P1, not copied from a published table. *Mitigation:* CI `EXPECTED_HASH` fails loudly if wrong. *Status:* eliminated by verification.
- **R6 Header path drift:** the frontend-API header moved from `UI/obs-frontend-api/` to `frontend/api/` in the 32.x tree. *Mitigation:* this spec cites the 32.2.2 path (`frontend/api/obs-frontend-api.h`); `#include <obs-frontend-api.h>` is resolved by the installed CMake package, not a relative path, so downstream moves are absorbed. *Status:* eliminated by using the installed package include.
