#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "command_queue.h"
#include "palette.h"

namespace pd {

// ---- Sections / scheduling enums (spec 4.2) ----
enum class Section { Groove, Breakdown, Drop };
enum class ChangeTrigger { Drop, Phrases, Random, Manual };
enum class ChangeOrder { Sequence, Shuffle };

// ---- Config structs (spec 4.2) ----
struct ColourSettings {
	float strength = 1.0f;
	float contrast = 1.1f;
	float keep_brightness = 0.0f;
};

struct PaletteChanges {
	ChangeTrigger trigger = ChangeTrigger::Drop;
	int every_phrases = 4; // Phrases trigger
	float chance = 0.25f;  // Random trigger
	ChangeOrder order = ChangeOrder::Sequence;
	float crossfade_beats = 2.0f;  // 0 = cut
	std::string breakdown_palette; // "" = none; resolved BY NAME
};

struct DirectorConfig {
	int phrase_beats = 32;
	float fade_beats = 1.0f;
	float fallback_bpm = 128.0f;
	bool random_off = true;
	float random_chance = 0.15f;
	int random_bars_min = 1;
	int random_bars_max = 4;
	int cooldown_phrases = 2;
	bool off_on_breakdown = false;
	bool off_on_drop = false;
	int drop_raw_bars = 2;
	float breathing = 0.0f;
	bool rotation = false;
	uint32_t seed = 7;
	// Added beyond spec 4.2 (present in the set schema 5.1 and properties 3.4):
	bool enabled = true;
	// v1.0.1 audio sensitivity (set-file keys optional on load; defaults = v1.0.0 behaviour)
	float input_gain_db = 0.0f;    // -12..+24 dB, applied to the mono downmix before analysis
	float kick_sensitivity = 0.6f; // 0..1 (shown 0-100%); see kick_threshold_for()
	// v1.0.1 drop colour cycle (lock behaviour): on a drop after a breakdown, maybe hard-cut through
	// the normal palettes for a few bars, then land on the palette the drop would have picked.
	bool drop_cycle = false;           // set-file key missing = disabled (old files unchanged)
	float drop_cycle_chance = 0.5f;    // 0..1
	int drop_cycle_bars = 1;           // 1..4
	int drop_cycle_steps_per_beat = 2; // 1 ("1/4"), 2 ("1/8"), 4 ("1/16")
};

// v1.0.1: kick threshold as a fraction of the adaptive bass reference.
// sensitivity 0 -> 0.85, 0.6 (default) -> 0.40 (= v1.0.0's fixed 0.4), 1 -> 0.10. Higher = more sensitive.
inline float kick_threshold_for(float sensitivity)
{
	const float s = sensitivity < 0.0f ? 0.0f : (sensitivity > 1.0f ? 1.0f : sensitivity);
	return 0.85f - 0.75f * s;
}
// v1.0.1: input gain in dB (clamped to -12..+24) -> linear amplitude factor.
float input_gain_linear(float db);

struct SetFile {
	std::string name;
	std::string synesthesia_playlist;
	std::vector<PaletteSpec> palettes; // 1..16
	ColourSettings colour;
	PaletteChanges palette_changes;
	DirectorConfig director;
};

// ---- Lock-free snapshot for the render thread (spec 4.2) ----
struct DirectorState {
	float bpm = 0.0f;
	float confidence = 0.0f;
	float bass = 0.0f;
	float mix_target = 1.0f; // scheduler target in [0,1]; the render thread fades toward it
	int rotation_offset = 0;
	Section section = Section::Groove;
	uint64_t beat = 0;
	uint64_t phrase = 0;
	int live_palette = 0;  // index into SetFile::palettes
	int prev_palette = -1; // outgoing palette, -1 if none
	float pfade = 1.0f;    // 0..1 crossfade progress
	int cooldown_phrases_left = 0;
	bool lock_on = true;
	// Added for the filter + dock (P4/P5); not in spec 4.2 but derived from the same state.
	float beat_frac = 0.0f; // fractional part of the beat clock (beat lights)
	int next_palette = -1;  // pickNextPreview: next palette the scheduler would pick, -1 if none
	bool paused = false;    // director paused (Pause / Panic)
	bool panic = false;     // PANIC latched (v1.0.1 toggle); implies paused + lock held on
	bool forced_on = false; // Force on toggle
	bool force_off = false; // Force off (hold)
	bool enabled = true;    // DirectorConfig::enabled
	bool silent = true;     // feed is silent (fallback BPM in use)
	// v1.0.1 audio meter (dock): all after the input gain
	float input_db = -120.0f;    // peak level, dBFS (fast attack, ~300 ms fall)
	bool clip = false;           // peak above -1 dBFS within the last ~1.5 s
	float bass_level = 0.0f;     // bass envelope / adaptive reference (0..1)
	float kick_threshold = 0.4f; // the kick threshold on the same scale (marker on the bass bar)
	uint32_t kicks = 0;          // detected kick count (the dock flashes its kick light on change)
	bool drop_cycle = false;     // v1.0.1: a drop colour cycle is running (lock held on)
};

// ---- Director decision log (feeds the dock log in P5 and the offline tests) ----
struct Event {
	enum class Type { SectionChange, PaletteChange, LockOff, LockOn, Command };
	Type type = Type::Command;
	uint64_t beat = 0;
	uint64_t phrase = 0;
	Section section = Section::Groove; // SectionChange
	int palette = -1;                  // PaletteChange: new live index
	int prev_palette = -1;             // PaletteChange: outgoing index
	std::string detail;                // human-readable reason
};

class Scheduler; // forward declaration (implementation detail, lives in scheduler.h/.cpp)

// The Director library core. Pure C++17, no libobs/Qt.
//
// Threading contract (used by P4): push_audio runs on the audio/WAV thread and drains
// the command queue; snapshot() is safe on the render thread (it reads a plain struct
// copy); push_command is safe from the UI thread (SPSC queue).
class Director {
public:
	explicit Director(const SetFile &);
	~Director();
	Director(Director &&) noexcept;
	Director &operator=(Director &&) noexcept;
	Director(const Director &) = delete;
	Director &operator=(const Director &) = delete;

	void push_audio(const float *mono, std::size_t frames, uint32_t sample_rate);
	DirectorState snapshot() const;
	void push_command(Command cmd, int palette_index = -1);
	void apply_set(const SetFile &);
	const std::vector<Event> &events() const;

	// P4 additions (filter host). update_set re-configures a running Director without
	// resetting the beat clock / analysis; live_palette >= 0 also sets the live palette
	// (used when the filter restores a saved live palette or the palette list changes).
	void update_set(const SetFile &, int live_palette = -1);
	// Moves the events produced since the last call into `out` (appended).
	void drain_events(std::vector<Event> &out);
	// When false, events() keeps no history (long-running host; use drain_events).
	void set_keep_history(bool keep);
	// v1.0.1: forget the adaptive bass reference + meters (call when the audio source changes).
	void reset_audio_reference();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace pd
