#include "doctest.h"

#include <vector>

#include "director.h"
#include "test_helpers.h"

using namespace pdtest;

// P4 regression (found in the OBS sandbox): when the feed came back after silence the
// section tracker still held the last pre-silence kick beat, so the first beats after the
// gap read as ">= 2 bars without bass" -> a false BREAKDOWN, then a false DROP.
TEST_CASE("feed resuming after silence does not fake a breakdown/drop")
{
	pd::SetFile s = pdtest::make_set(4, "Pal 3");
	s.director.random_off = false;
	// resume at several points inside a beat (e.g. in a kick's decay tail), as a live feed does
	for (int k = 0; k < 8; ++k) {
		pd::Director d(s);
		feed(d, gen_kick(128.0, 16).to_float(), 44100);
		feed(d, gen_silence(20.0 + 0.06 * k).to_float(), 44100);
		const std::size_t before = d.events().size();
		std::vector<float> resume = gen_kick(128.0, 16).to_float();
		const std::size_t skip = static_cast<std::size_t>((0.15 + 0.1 * k) * 60.0 / 128.0 * 44100);
		resume.erase(resume.begin(), resume.begin() + static_cast<std::ptrdiff_t>(skip));
		feed(d, resume, 44100);
		std::vector<pd::Event> after(d.events().begin() + static_cast<std::ptrdiff_t>(before),
					     d.events().end());
		CHECK(pdtest::section_changes(after).empty());
		CHECK(pdtest::palette_changes(after).empty());
	}
}

TEST_CASE("update_set keeps the beat clock and can force the live palette")
{
	pd::SetFile s = pdtest::make_set(3);
	s.director.random_off = false;
	pd::Director d(s);
	feed(d, gen_kick(128.0, 8).to_float(), 44100);
	const uint64_t beat = d.snapshot().beat;
	CHECK(beat >= 30);
	s.colour.strength = 0.5f;
	d.update_set(s, 2);
	feed(d, std::vector<float>(4410, 0.0f), 44100);
	CHECK(d.snapshot().beat >= beat);
	CHECK(d.snapshot().live_palette == 2);
	CHECK(d.snapshot().prev_palette == -1);
	// shrinking the palette list clamps the live palette
	s.palettes.resize(2);
	d.update_set(s);
	feed(d, std::vector<float>(4410, 0.0f), 44100);
	CHECK(d.snapshot().live_palette == 1);
}

TEST_CASE("drain_events streams the decision log")
{
	pd::SetFile s = pdtest::make_set(3);
	s.director.random_off = false;
	pd::Director d(s);
	std::vector<pd::Event> out;
	d.drain_events(out);
	CHECK(out.empty());
	d.set_keep_history(false);
	d.push_command(pd::Command::GoLive, 2);
	feed(d, std::vector<float>(4410, 0.0f), 44100);
	d.drain_events(out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].detail == "palette -> Pal 2 (manual, 2-beat crossfade)");
	CHECK(d.events().empty());
	CHECK(d.snapshot().next_palette == 0);
}
