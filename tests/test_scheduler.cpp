#include "doctest.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "director.h"
#include "test_helpers.h"

using namespace pdtest;

TEST_CASE("random off at chance 1.0 fires on the next phrase and respects cooldown")
{
	pd::SetFile s = pdtest::make_set(1);
	s.director.phrase_beats = 32;
	s.director.random_off = true;
	s.director.random_chance = 1.0f;
	s.director.random_bars_min = 1;
	s.director.random_bars_max = 1; // deterministic 1-bar off
	s.director.cooldown_phrases = 2;
	pd::Director d(s);
	const WavMono w = pdtest::gen_kick(128.0, 40); // 160 beats -> phrase boundaries at 32,64,96,128
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "random-off.wav");

	const auto offs = pdtest::lock_offs(d.events());
	const auto ons = pdtest::lock_ons(d.events());

	std::vector<uint64_t> off_beats;
	for (const auto &e : offs)
		off_beats.push_back(e.beat);
	std::vector<uint64_t> on_beats;
	for (const auto &e : ons)
		on_beats.push_back(e.beat);

	// First off at the very next phrase boundary (beat 32), then a 1-bar (4-beat) off,
	// then the next off after the 2-phrase cooldown (beat 128).
	CHECK(off_beats == std::vector<uint64_t>({32, 128}));
	CHECK(on_beats == std::vector<uint64_t>({36, 132}));

	// lock held on throughout otherwise
	CHECK(d.snapshot().lock_on == true);
}

TEST_CASE("shuffle plays every normal palette once before any repeat; breakdown excluded")
{
	constexpr int kNormal = 3; // palettes 0,1,2; breakdown = index 3
	pd::SetFile s = pdtest::make_set(4, "Pal 3");
	s.palette_changes.order = pd::ChangeOrder::Shuffle;
	s.palette_changes.trigger = pd::ChangeTrigger::Phrases;
	s.palette_changes.every_phrases = 1;
	s.director.phrase_beats = 32;
	s.director.random_off = false;
	s.director.seed = 7;
	pd::Director d(s);
	// 64 bars = 256 beats -> phrase boundaries at 32..224 = 7 palette changes (2+ full
	// rotations of 3), enough to catch a repeat within any window of N normal palettes.
	const WavMono w = pdtest::gen_kick(128.0, 64);
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "shuffle.wav");

	const auto pcs = pdtest::palette_changes(d.events());
	REQUIRE(pcs.size() >= 2 * kNormal);

	std::vector<int> seq;
	for (const auto &e : pcs)
		seq.push_back(e.palette);

	// breakdown palette (index 3) never enters the rotation
	for (int v : seq)
		CHECK(v != 3);

	// no repeat within any window of N normal palettes (spec 4.2 + design doc test 7):
	// every N consecutive changes must be a full permutation of the normal pool.
	for (std::size_t i = 0; i + kNormal <= seq.size(); ++i) {
		std::vector<int> window(seq.begin() + static_cast<std::ptrdiff_t>(i),
					seq.begin() + static_cast<std::ptrdiff_t>(i + kNormal));
		std::sort(window.begin(), window.end());
		CAPTURE(i);
		for (int p = 0; p < kNormal; ++p)
			CHECK(window[static_cast<std::size_t>(p)] == p);
	}
}

TEST_CASE("breakdown palette goes live on BREAKDOWN; next normal on DROP")
{
	pd::SetFile s = pdtest::make_set(4, "Pal 3"); // breakdown = index 3
	s.palette_changes.trigger = pd::ChangeTrigger::Drop;
	s.palette_changes.order = pd::ChangeOrder::Sequence;
	s.palette_changes.crossfade_beats = 2.0f;
	s.director.off_on_breakdown = false;
	s.director.random_off = false;
	pd::Director d(s);
	const WavMono w = pdtest::gen_breakdown(128.0, 8, 4, 4); // breakdown@32, drop@48
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "bd-palette.wav");

	const auto evs = d.events();
	const int64_t bd = pdtest::section_beat(evs, pd::Section::Breakdown);
	const int64_t dp = pdtest::section_beat(evs, pd::Section::Drop);

	const auto pcs = pdtest::palette_changes(evs);
	bool saw_breakdown_live = false;
	bool saw_next_normal = false;
	for (const auto &e : pcs) {
		if (e.palette == 3 && std::llabs(static_cast<int64_t>(e.beat) - bd) <= 2)
			saw_breakdown_live = true;
		if (e.palette == 1 && std::llabs(static_cast<int64_t>(e.beat) - dp) <= 2)
			saw_next_normal = true;
	}
	CHECK(saw_breakdown_live); // breakdown palette live within 2 beats of BREAKDOWN
	CHECK(saw_next_normal);    // next normal palette (1, sequence from 0) on the DROP

	// crossfade finishes within the configured beats
	CHECK(d.snapshot().pfade == doctest::Approx(1.0f).epsilon(0.001f));
	CHECK(d.snapshot().live_palette == 1);
}

TEST_CASE("trigger: Every N phrases changes in list order")
{
	pd::SetFile s = pdtest::make_set(3);
	s.palette_changes.trigger = pd::ChangeTrigger::Phrases;
	s.palette_changes.every_phrases = 2;
	s.palette_changes.order = pd::ChangeOrder::Sequence;
	s.director.phrase_beats = 32;
	s.director.random_off = false;
	pd::Director d(s);
	const WavMono w = pdtest::gen_kick(128.0, 40);
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "every-n.wav");

	const auto pcs = pdtest::palette_changes(d.events());
	REQUIRE(pcs.size() == 2);
	CHECK(pcs[0].beat == 64);
	CHECK(pcs[0].palette == 1);
	CHECK(pcs[1].beat == 128);
	CHECK(pcs[1].palette == 2);
}

TEST_CASE("trigger: Random per phrase (chance 1.0) changes every phrase")
{
	pd::SetFile s = pdtest::make_set(3);
	s.palette_changes.trigger = pd::ChangeTrigger::Random;
	s.palette_changes.chance = 1.0f;
	s.palette_changes.order = pd::ChangeOrder::Sequence;
	s.director.phrase_beats = 32;
	s.director.random_off = false;
	pd::Director d(s);
	const WavMono w = pdtest::gen_kick(128.0, 40);
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "random-trig.wav");

	const auto pcs = pdtest::palette_changes(d.events());
	REQUIRE(pcs.size() == 5); // boundaries at 32, 64, 96, 128, 160
	const int expect[] = {1, 2, 0, 1, 2};
	for (std::size_t i = 0; i < pcs.size(); ++i) {
		CAPTURE(i);
		CHECK(pcs[i].beat == 32 * (i + 1));
		CHECK(pcs[i].palette == expect[i]);
	}
}

TEST_CASE("trigger: Drop changes only on drops")
{
	pd::SetFile s = pdtest::make_set(3);
	s.palette_changes.trigger = pd::ChangeTrigger::Drop;
	s.palette_changes.order = pd::ChangeOrder::Sequence;
	s.director.random_off = false;
	pd::Director d(s);
	const WavMono w = pdtest::gen_breakdown(128.0, 8, 4, 4);
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "drop-trig.wav");

	const auto pcs = pdtest::palette_changes(d.events());
	REQUIRE(pcs.size() == 1);
	CHECK(pcs[0].palette == 1);
	CHECK(pcs[0].beat >= 47);
	CHECK(pcs[0].beat <= 49);
}

TEST_CASE("trigger: Manual makes no automatic changes")
{
	pd::SetFile s = pdtest::make_set(3);
	s.palette_changes.trigger = pd::ChangeTrigger::Manual;
	s.director.random_off = false;
	pd::Director d(s);
	const WavMono w = pdtest::gen_kick(128.0, 40);
	pdtest::feed_roundtrip(d, w, pdtest::fixture_dir(), "manual.wav");

	CHECK(pdtest::palette_changes(d.events()).empty());

	d.push_command(pd::Command::GoLive, 2);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100); // drain
	CHECK(d.snapshot().live_palette == 2);
}

TEST_CASE("16-palette set: shuffle plays all 15 normal palettes before any repeat")
{
	constexpr int kNormal = 15; // palettes 0..14; breakdown = index 15
	pd::SetFile s = pdtest::make_set(16, "Pal 15");
	s.palette_changes.order = pd::ChangeOrder::Shuffle;
	s.palette_changes.trigger = pd::ChangeTrigger::Phrases;
	s.palette_changes.every_phrases = 1;
	s.director.phrase_beats = 16;
	s.director.random_off = false;
	s.director.seed = 2077;
	pd::Director d(s);
	// 136 bars = 544 beats -> 33 phrase boundaries (16-beat phrases) = 2+ full passes of 15
	const WavMono w = pdtest::gen_kick(128.0, 136);
	pdtest::feed(d, w.to_float(), w.sample_rate);

	const auto pcs = pdtest::palette_changes(d.events());
	REQUIRE(pcs.size() >= 2 * kNormal);
	std::vector<int> seq;
	for (const auto &e : pcs)
		seq.push_back(e.palette);
	for (int v : seq)
		CHECK(v != 15);
	for (std::size_t i = 0; i + kNormal <= seq.size(); ++i) {
		std::vector<int> window(seq.begin() + static_cast<std::ptrdiff_t>(i),
					seq.begin() + static_cast<std::ptrdiff_t>(i + kNormal));
		std::sort(window.begin(), window.end());
		CAPTURE(i);
		for (int p = 0; p < kNormal; ++p)
			CHECK(window[static_cast<std::size_t>(p)] == p);
	}
}
