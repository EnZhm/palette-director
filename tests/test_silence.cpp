#include "doctest.h"

#include "director.h"
#include "test_helpers.h"

using namespace pdtest;

TEST_CASE("silence -> fallback BPM, no random events, manual still works")
{
	pd::SetFile s = pdtest::make_set(3);
	s.director.fallback_bpm = 128.0f;
	s.director.random_off = true;
	s.director.random_chance = 1.0f; // would fire if not silent
	s.palette_changes.trigger = pd::ChangeTrigger::Random;
	s.palette_changes.chance = 1.0f; // would change if not silent
	pd::Director d(s);
	const WavMono w = pdtest::gen_silence(10.0);
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "silence.wav");

	const pd::DirectorState st = d.snapshot();
	CHECK(st.bpm == doctest::Approx(128.0).epsilon(0.001)); // fallback
	CHECK(st.confidence < 0.3f);                            // no lock
	CHECK(st.lock_on == true);                              // held on
	CHECK(st.mix_target == 1.0f);

	CHECK(pdtest::lock_offs(d.events()).empty());       // no random off
	CHECK(pdtest::palette_changes(d.events()).empty()); // no automatic changes

	// manual go-live still works in silence
	d.push_command(pd::Command::GoLive, 2);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100);
	CHECK(d.snapshot().live_palette == 2);
}
