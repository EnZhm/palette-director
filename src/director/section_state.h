#pragma once
#include <cstdint>

#include "director.h"

namespace pd {

// GROOVE / BREAKDOWN / DROP from bass presence (design 2 "Section state"):
//   BREAKDOWN: bass absent for >= 2 bars (8 beats)
//   DROP:      bass returns on a downbeat; lasts one phrase
//   GROOVE:    otherwise
class SectionState {
public:
	SectionState() = default;

	// Call once per beat. `bass_present` = a kick/bass hit occurred this beat.
	Section update(uint64_t beat, bool bass_present, int phrase_beats);

	Section current() const { return cur_; }
	void reset()
	{
		cur_ = Section::Groove;
		last_bass_beat_ = 0;
		drop_start_ = -1;
	}

private:
	Section cur_ = Section::Groove;
	uint64_t last_bass_beat_ = 0;
	int64_t drop_start_ = -1;
};

} // namespace pd
