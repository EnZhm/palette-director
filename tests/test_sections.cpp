#include "doctest.h"

#include <cmath>
#include <string>

#include "director.h"
#include "test_helpers.h"

using namespace pdtest;

// Phrase boundaries land on downbeats (beat 32 = 8 bars = a downbeat), and the beat grid
// tracks the actual audio across the whole file.
TEST_CASE("beat grid tracks audio; phrase boundaries on downbeats")
{
	pd::SetFile s = pdtest::make_set(1);
	s.director.phrase_beats = 32;
	s.director.fallback_bpm = 128.0f;
	pd::Director d(s);
	const WavMono w = pdtest::gen_kick(128.0, 16); // 64 beats
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "phrase-kick.wav");

	const pd::DirectorState st = d.snapshot();
	CHECK(st.beat >= 63);
	CHECK(st.beat <= 65);
	CHECK(st.phrase == 2); // boundaries at beat 32 and 64, both downbeats (32 % 4 == 0)
}

TEST_CASE("breakdown detected within 2 bars; DROP on the return downbeat")
{
	pd::SetFile s = pdtest::make_set(3);
	pd::Director d(s);
	// groove 8 bars -> breakdown 4 bars -> drop 4 bars; breakdown starts at beat 32,
	// drop returns on the downbeat at beat 48.
	const WavMono w = pdtest::gen_breakdown(128.0, 8, 4, 4);
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "sections.wav");

	const auto evs = d.events();
	const int64_t bd = pdtest::section_beat(evs, pd::Section::Breakdown);
	const int64_t dp = pdtest::section_beat(evs, pd::Section::Drop);

	CHECK(bd > 32);
	CHECK(bd <= 40); // within 2 bars (8 beats) of the breakdown start
	CHECK(dp >= 47);
	CHECK(dp <= 49); // on the return downbeat (beat 48)
	CHECK(dp > bd);
}
