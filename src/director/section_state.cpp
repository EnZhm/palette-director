#include "section_state.h"

namespace pd {

namespace {
constexpr uint64_t kBreakdownBeats = 8; // 2 bars of 4
}

Section SectionState::update(uint64_t beat, bool bass_present, int phrase_beats)
{
	switch (cur_) {
	case Section::Groove:
		if (bass_present) {
			last_bass_beat_ = beat;
		} else if (beat - last_bass_beat_ >= kBreakdownBeats) {
			cur_ = Section::Breakdown;
		}
		break;
	case Section::Breakdown:
		if (bass_present)
			last_bass_beat_ = beat;
		if (bass_present && (beat % 4 == 0)) {
			cur_ = Section::Drop;
			drop_start_ = static_cast<int64_t>(beat);
		}
		break;
	case Section::Drop:
		if (bass_present)
			last_bass_beat_ = beat;
		if (drop_start_ >= 0 &&
		    beat - static_cast<uint64_t>(drop_start_) >= static_cast<uint64_t>(phrase_beats)) {
			cur_ = Section::Groove;
			drop_start_ = -1;
		}
		break;
	}
	return cur_;
}

} // namespace pd
