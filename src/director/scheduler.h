#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "director.h"
#include "rng.h"

namespace pd {

// Resolved scheduling configuration handed from the Director (breakdown palette
// already resolved to an index; names + colour counts copied for the log / rotation).
struct SchedulerConfig {
	int palette_count = 1;
	int breakdown_index = -1; // -1 = none
	std::vector<std::string> names;
	std::vector<int> colour_counts;
	bool enabled = true;
	ChangeTrigger trigger = ChangeTrigger::Drop;
	int every_phrases = 4;
	float chance = 0.25f;
	ChangeOrder order = ChangeOrder::Sequence;
	float crossfade_beats = 2.0f;
	bool random_off = true;
	float random_chance = 0.15f;
	int random_bars_min = 1;
	int random_bars_max = 4;
	int cooldown_phrases = 2;
	bool off_on_breakdown = false;
	bool off_on_drop = false;
	int drop_raw_bars = 2;
	bool rotation = false;
	uint32_t seed = 7;
};

// Palette-change scheduler + lock behaviours -> mix target + pfade (spec 4.2).
// Mirrors the mock v2 semantics of pickNext / goLive / onSectionChange / phraseBoundary.
class Scheduler {
public:
	explicit Scheduler(const SchedulerConfig &cfg) : rng_(cfg.seed) { reset(cfg); }

	void reset(const SchedulerConfig &cfg);
	// Live re-configuration (settings edited while running): keeps the runtime state
	// where it is still valid. live >= 0 forces the live palette (no crossfade).
	void reconfigure(const SchedulerConfig &cfg, int live);
	// Mock pickNextPreview: the palette the next automatic change would pick, or -1.
	int peek_next() const;
	bool forced_on() const { return forced_on_; }
	bool force_off() const { return force_off_; }

	std::vector<Event> on_section_change(Section sec, uint64_t beat);
	std::vector<Event> on_phrase(uint64_t phrase, uint64_t beat);
	std::vector<Event> on_beat(uint64_t beat);
	std::vector<Event> command(Command cmd, int palette_index, uint64_t beat);
	void advance_pfade(double dt, double bpm);

	double mix_target(uint64_t beat) const;
	bool lock_on(uint64_t beat) const { return mix_target(beat) >= 0.5f; }
	bool paused() const { return paused_; }
	bool panic() const { return panic_; }

	int live_palette() const { return live_; }
	int prev_palette() const { return prev_; }
	float pfade() const { return pfade_; }
	int rotation_offset() const { return rot_; }
	int cooldown_left() const { return cooldown_; }

private:
	std::vector<int> normal_idx() const;
	int pick_next(int dir);
	std::vector<Event> go_live(int i, const std::string &why, uint64_t beat);
	void release_panic();

	SchedulerConfig cfg_;
	Mulberry32 rng_;

	int live_ = 0;
	int prev_ = -1;
	float pfade_ = 1.0f;
	int rot_ = 0;
	bool bd_active_ = false;
	int last_normal_ = -1;
	std::vector<int> order_;         // remaining palettes of the current shuffle pass (consumed from the front)
	std::vector<int> shuffle_cycle_; // canonical seeded permutation, replayed each full pass
	int cooldown_ = 0;
	int64_t off_until_ = -1;
	std::string off_reason_;
	bool paused_ = false;
	bool panic_ = false; // PANIC latch (v1.0.1: a toggle; PANIC again or Resume releases it)
	bool force_off_ = false;
	bool forced_on_ = false;
};

} // namespace pd
