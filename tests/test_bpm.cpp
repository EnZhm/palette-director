#include "doctest.h"

#include <cmath>
#include <string>

#include "director.h"
#include "test_helpers.h"

using namespace pdtest;

TEST_CASE("BPM estimate within +/-1 at 124/128/132/140")
{
	const double targets[] = {124.0, 128.0, 132.0, 140.0};
	const std::string dir = pdtest::fixture_dir();
	for (double bpm : targets) {
		CAPTURE(bpm);
		pd::SetFile s = pdtest::make_set(1);
		s.director.fallback_bpm = static_cast<float>(bpm);
		pd::Director d(s);
		const WavMono w = pdtest::gen_kick(bpm, 16);
		pdtest::feed_roundtrip(d, w, dir, "kick-" + std::to_string(static_cast<int>(bpm)) + ".wav");

		const pd::DirectorState st = d.snapshot();
		CAPTURE(st.bpm);
		CAPTURE(st.confidence);
		CHECK(std::abs(st.bpm - bpm) <= 1.0); // acceptance: BPM within +/-1
		CHECK(st.confidence > 0.3f);          // locked
	}
}
