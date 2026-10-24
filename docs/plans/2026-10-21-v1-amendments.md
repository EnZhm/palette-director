# Palette Director v1 amendments

Date: 2026-10-21 (implemented 2026-10-23). Status: folded into v1 on branch `win/v1-complete` (draft PR #5).
Applies on top of `2026-10-19-palette-director-spec.md` and the ratified design.

Gate G1 passed on Ian's VJ laptop. Ian's feedback:

> "the colours overlay and don't totally take over, especially the white point; I want a control where
> 100 means no other colours show through. On coloured scenes the palettes weren't accurate. Flesh out
> Techno Apocalypse with lots of duotones and on-theme palettes. Improve the filter UI. It's set and
> forget, but put as much as possible in the Palette Director panel while keeping it easy to see
> what's happening."

## Root causes (diagnosed before the change)

1. The preset's breathing of 0.25 made the blend amount dip to 0.75 between kicks, so the raw source
   leaked through.
2. Keep brightness 0.15 mixed source brightness back in and clipped towards white.
3. Every gradient palette tops out near white, so bright areas always went white.
4. Rec.709 luma pushes saturated blues to near black, so blue scenes mapped to the bottom of the palette.

## What changed versus the ratified design, and why

### 1. Coverage (was "Strength")

- Same key (`strength` in settings, `colour.strength` in the set file), now labelled **Coverage** and
  shown as 0-100%.
- The maths is unchanged: `amt = coverage x breathing_factor x lock_mix` with the mock fade model.
  `breathing_factor = 1 - b + b * min(1, bass * 1.2)` is at most 1, and `lock_mix` is at most 1, so
  breathing and random-off can only ever dip **below** coverage. The UI says so.
- Default breathing is 0, default keep brightness is 0.
- Clarification (small deviation): breathing is suspended while the Director reports the feed as
  silent (no audio source, or a silent feed). Before this, a silent feed meant `bass = 0`, so a
  non-zero breathing value held `amt` at `1 - b` permanently, which is the "colours don't fully take
  over" symptom with nothing to breathe with. With this rule, coverage 100% plus lock on means pure
  palette output whenever there is no music. Implemented in `pl_lock_amount()` (src/palette/palette.c)
  and unit-tested.

### 2. Input range

- New optional set-file keys `colour.input_black` (0-0.5, default 0) and `colour.input_white`
  (0.5-1, default 1).
- Shader, after the sRGB conversion: `l = saturate((l - black) / max(white - black, 0.01))`, applied
  before contrast. Lowering input white lets dim or coloured scenes reach the top of the palette.

### 3. Brightness from

- New optional key `colour.brightness_source`: `"perceived"` (Rec.709, default), `"max"`
  (max(r,g,b)) or `"average"`. A uniform int in the shader.
- The new uniforms (`in_black`, `in_white`, `bsrc`) are scalars appended after the existing ones so
  the D3D11 cbuffer layout stays consistent; `tests/test_cbuffer_layout.cpp` covers them.
- Older schema-2 files without these keys still load (defaults applied). The schema number stays 2.

### 4. Hue-aware mode: deliberately deferred

The design dropped hue remapping. Items 1-3 give "the palette always wins" accuracy on coloured
scenes, which is what was asked for. A hue-aware mode is not part of v1.

### 5. Up to 16 palettes per set

- `PL_MAX_PALETTES` is 16 (Director, set file, dock, filter, tests). Every fixed-size array and loop
  that assumed 8 was checked.
- Hotkeys stay at "Palette 1-8 go live" for the first 8 palettes; palettes 9-16 are reachable from the
  dock (pads) and the Next/Prev controls.
- The shader still uploads only the two palettes being crossfaded (A/B), 8 colours each.

### 6. Techno Apocalypse preset changes

- The 7 original palettes are unchanged (names, order, colours). 9 palettes are appended (16 total):
  six duotones (Blacklight, Acid Rain, Thermal, Cold Steel, Nuclear Dawn, Bruise) and three gradient
  maps with saturated, non-white tops (Sodium Streetlight, Chernobyl Glow, Ultraviolet Riot).
- Keep brightness 0 (was 0.15), breathing 0 (was 0.25). Coverage 1.0, input range 0-1, brightness
  perceived. Everything else as before (every 8 phrases, shuffle, 1-beat crossfade, breakdown
  Spectrum Fallout, contrast 1.2, random off 12% for 1-2 bars with a 3-phrase cooldown, off on drop for
  1 bar, fallback 132 BPM, seed 2077).
- **Source of truth sync needed.** The bundled `data/sets/techno-apocalypse.json` is intentionally no
  longer identical to the TechnoCore copy. Kalessin must sync
  `/mnt/storage/media/Visuals/palette-director/sets/Techno-Apocalypse.json` from the repo file. The
  repo file still round-trips byte for byte through the canonical writer.

### 7. UI moved to the dock

- The Palette Director dock is the main UI: an always-visible live section (instance picker, status
  line, beat lights, palette pads in a grid of up to 16, controls including PANIC, 64-beat timeline and
  a "now" sentence) above a collapsible tab widget (Set, Palettes, Look, Changes, Director, Log).
- Dock edits are applied on the UI thread through the filter's settings (`obs_source_update`), the
  same path the properties window uses; the filter then pushes them into the Director. No new
  cross-thread paths.
- The filter properties window is now a short fallback: header, Set (selector + Load file...), Audio
  source, Coverage, an "Open Palette Director panel" button, and a collapsed "Advanced (also in the
  panel)" group with everything else, so nothing is lost if the dock is unavailable.

### 8. Test hooks are build-gated

The sandbox test hooks (`PD_DOCK_TEST_CYCLE`, `PD_DOCK_SNAPSHOT_DIR`, `PD_UI_AUTO`) are compiled only
with the CMake option `PD_TEST_HOOKS=ON` (default OFF). Release builds do not contain them (verified
by a string search of the release DLL).

## v1.0.1

Changes after the v1.0.0 release, from Ian's first sessions on the VJ laptop.

### 9. PANIC is a toggle (deviation from the spec's one-way latch)

The spec made PANIC a one-way latch: lock on, director paused, palette held, rotation reset, and only
Pause/Resume got you out. On the laptop Ian could not find the way out ("the panic lock is stuck on"),
and a Force on pressed while panicked stayed on even after Resume.

- PANIC (dock button, hotkey F17) is now a toggle. The first press does what it did before. The second
  press releases it: the director resumes, Force on and Force off are cleared, and the log says
  "PANIC released: director resumed".
- Resume (F15 or the dock's Resume) while panicked also releases PANIC fully, including Force on.
- Panic is its own state (`Scheduler::panic_`, `DirectorState::panic`), separate from a plain Pause.
  While it is latched the lock is held on whatever Force on/off say.
- Dock: the PANIC button is checkable and filled red while active ("PANIC active - press to release"),
  the state chip reads PANIC, and the now line says "PANIC: lock held on, director paused (press PANIC
  or Resume to release)". Force on shows its checked state, and the now line says when Force on is
  holding the lock.
- A drop that happens while panicked does not change the palette (the director is paused); after the
  release the held palette stays until the next automatic change.

### 10. Audio sensitivity: adaptive level reference, input gain, kick sensitivity, meter

Ian: "the audio needs to be more sensitive or have a control to adjust". Root cause: the kick test
compared each beat's bass peak with an all-time maximum that never decayed, so after one loud moment
(a +5.5 dBFS synthetic track before switching to Desktop Audio at about -10 dBFS) normal kicks never
cleared the bar and the Director sat in BREAKDOWN with no drop.

- **Adaptive reference.** The reference has an instant attack and a 10 s release. It never falls below
  10% of a 60 s follower of itself (a long no-bass breakdown cannot drag it down to the pad's residue)
  nor below an absolute floor of 1e-7 bass energy (about -70 dB), so silence and hiss never count as
  kicks. A kick must also rise 25% above the beat's own minimum (steady drones do not count). The
  reference resets when the audio source changes. 10 s was picked with `tests/test_audio_level.cpp`:
  6 s let kicks 8 dB softer count as kicks within 4 bars (too eager in a breakdown); 12 s took 17 bars
  to recover from a section 15 dB louder; 10 s recovers in about 15 bars and keeps 8 dB-softer
  material out of the kick count at the default sensitivity.
- **Input gain** (`director.input_gain_db`, -12..+24 dB, default 0): applied to the mono downmix before
  analysis, so the silence detector and the meter see the gained signal. Because the kick test is now
  relative to the adaptive reference, gain does not change section detection on a healthy feed; it
  matters at the silence floor (a very quiet line feed is treated as silence until it is boosted) and
  for the meter.
- **Kick sensitivity** (`director.kick_sensitivity`, 0-100%, default 60%): kick threshold =
  (0.85 - 0.75 x sensitivity) x reference, so 0% = 0.85, 60% = 0.40 (= v1.0.0), 100% = 0.10. Higher is
  more sensitive.
- Both keys are optional on load (v1.0.0 files behave as before) and live in the Director group of the
  properties and the dock's Director tab, each with a one-line help text saying what to change when
  drops are not detected.
- **Dock meter**: input level in dBFS after gain (CLIP above -1 dBFS, held 1.5 s), a bass bar with the
  kick threshold marked, and a kick light that flashes on each detected kick. Published in
  `DirectorState` and copied to the dock under the existing view mutex; nothing new on the render thread.

### 11. Drop colour cycle (new lock behaviour, requested by Ian)

When a DROP follows a BREAKDOWN (the same event that does "drop after breakdown"), the Director may hard-cut
through the palettes for a short time, then settle.

- Set-file keys under `director.drop_cycle`: `enabled` (bool), `chance` 0-1 (default 0.5), `bars` 1-4
  (default 1), `rate` "1/4" | "1/8" | "1/16" (default "1/8" = 2 cuts per beat). The object is optional;
  missing = disabled, so older files behave exactly as before.
- The seeded RNG is rolled only when the behaviour is enabled, so with it disabled every event log is
  identical to v1.0.0's (checked by a digest A/B against the commit before it, and by a unit test). The
  roll is logged, e.g. `drop cycle (0.31 < 0.50): 1 bar at 1/8, 8 cuts`.
- While cycling: hard cuts (pfade 1, crossfade_beats ignored), stepping through seeded shuffles of the
  normal pool (the breakdown palette is excluded, never the same palette twice in a row). The steps are
  driven by the beat tracker's fractional beat on every analysis hop, not only on whole beats. Rotation
  offset resets per cut, as go_live does. Phrase-triggered changes and rotation wait until it ends.
- At the end it lands on the palette the drop would have picked anyway, with the normal crossfade, and
  logs `drop cycle over -> <name>`.
- The lock is held on during the cycle. If off-on-drop is enabled and the cycle fires, the cycle replaces
  the raw bars for that drop (logged); if it does not fire, off-on-drop behaves as before.
- PANIC, Pause, Force off and manual Next / Prev / Go live cancel a running cycle at once (a manual pick
  wins). It is a lock behaviour, not a palette-change trigger, so it also runs in Manual-only mode.
- Dock: Director tab controls with help text; a "DROP CYCLE" status chip while it runs; the live pad
  flashes with each cut; the now line says so. The properties' Advanced group has the same controls.
- Techno Apocalypse: enabled, 50%, 1 bar, 1/8.

### 12. Preset ownership and data-driven preset tests

The Techno Apocalypse palettes are being redesigned by kalessin on TechnoCore. In v1.0.1 only settings
keys were added to `data/sets/techno-apocalypse.json` (`input_gain_db`, `kick_sensitivity`, `drop_cycle`);
the palettes array is unchanged. The preset tests are data-driven so the palettes array can be swapped
in a follow-up commit without touching code:

- `tests/test_set_file.cpp` "bundled Techno Apocalypse loads with every palette, mode and setting" checks
  palette structure only (1-16 palettes, unique non-empty names, valid mode, 2-8 colours, the breakdown
  palette resolves by name) and pins the settings values (colour, palette changes, director, v1.0.1 keys).
  It also pins the breakdown palette NAME "Spectrum Fallout"; if the new palettes rename it, update that
  one line (or the set's `breakdown_palette`).
- "load -> save round-trips the bundled preset byte for byte" compares against the file itself; there is
  no hard-coded hash.
- After merge kalessin must re-sync
  `/mnt/storage/media/Visuals/palette-director/sets/Techno-Apocalypse.json` from the repo file.
