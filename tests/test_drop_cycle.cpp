#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "director.h"
#include "test_helpers.h"

using namespace pdtest;

// v1.0.1 drop colour cycle: on a DROP that follows a BREAKDOWN, maybe hard-cut through the normal palettes
// for N bars at 1/4, 1/8 or 1/16, then land on the palette the drop would have picked anyway.

namespace {
constexpr int kBd = 5; // breakdown palette index in the test set ("Pal 5")

pd::SetFile cycle_set(bool on, float chance = 1.0f, int bars = 1, int spb = 2, uint32_t seed = 7)
{
	pd::SetFile s = pdtest::make_set(6, "Pal 5");
	s.director.random_off = false;
	s.director.seed = seed;
	s.director.drop_cycle = on;
	s.director.drop_cycle_chance = chance;
	s.director.drop_cycle_bars = bars;
	s.director.drop_cycle_steps_per_beat = spb;
	return s;
}

// groove 8 bars -> breakdown 4 bars -> drop 8 bars; the drop lands on beat 48
WavMono track()
{
	return pdtest::gen_breakdown(128.0, 8, 4, 8);
}

std::vector<pd::Event> cycle_cuts(const std::vector<pd::Event> &evs)
{
	std::vector<pd::Event> out;
	for (const auto &e : evs)
		if (e.type == pd::Event::Type::PaletteChange && e.detail.find("drop cycle") != std::string::npos)
			out.push_back(e);
	return out;
}

bool has(const std::vector<pd::Event> &evs, const std::string &needle)
{
	for (const auto &e : evs)
		if (e.detail.find(needle) != std::string::npos)
			return true;
	return false;
}

// the palette a drop-after-breakdown picks with the cycle disabled (same seed, same audio)
int drop_pick(uint32_t seed)
{
	pd::Director d(cycle_set(false, 1.0f, 1, 2, seed));
	pdtest::feed(d, track().to_float(), 44100);
	for (const auto &e : pdtest::palette_changes(d.events()))
		if (e.detail.find("drop after breakdown") != std::string::npos)
			return e.palette;
	return -1;
}
} // namespace

TEST_CASE("v1.0.1 drop cycle: exact cut count per rate and bars at 128 BPM, lands on the drop pick")
{
	struct Case {
		int bars, spb, cuts;
	};
	for (const Case c : {Case{1, 2, 8}, Case{1, 1, 4}, Case{1, 4, 16}, Case{2, 2, 16}, Case{4, 1, 16}}) {
		INFO("bars " << c.bars << " steps/beat " << c.spb);
		pd::SetFile s = cycle_set(true, 1.0f, c.bars, c.spb);
		pd::Director d(s);
		const auto w = pdtest::gen_breakdown(128.0, 8, 4, 8 + 4 * c.bars);
		pdtest::feed(d, w.to_float(), 44100, 256);
		const auto evs = d.events();
		const auto cuts = cycle_cuts(evs);
		CHECK(static_cast<int>(cuts.size()) == c.cuts);
		CHECK(has(evs, "drop cycle (")); // the roll is logged
		const int64_t dp = pdtest::section_beat(evs, pd::Section::Drop);
		REQUIRE(dp >= 47);
		// sub-beat steps: cut k lands in beat floor(k / spb) of the cycle
		for (std::size_t k = 0; k < cuts.size(); ++k)
			CHECK(static_cast<int64_t>(cuts[k].beat) == dp + static_cast<int64_t>(k) / c.spb);
		// never the breakdown palette, never the same palette twice in a row, all hard cuts
		for (std::size_t k = 0; k < cuts.size(); ++k) {
			CHECK(cuts[k].palette != kBd);
			CHECK(cuts[k].palette != cuts[k].prev_palette);
			CHECK(cuts[k].detail.find(", cut)") != std::string::npos);
		}
		// lands on the palette the drop would have chosen, with the normal crossfade, after N bars
		std::size_t over = evs.size();
		for (std::size_t i = 0; i < evs.size(); ++i)
			if (evs[i].detail.rfind("drop cycle over -> ", 0) == 0)
				over = i;
		REQUIRE(over + 1 < evs.size());
		const pd::Event &land = evs[over + 1];
		CHECK(land.type == pd::Event::Type::PaletteChange);
		CHECK(land.palette == drop_pick(7));
		CHECK(land.detail.find("drop after breakdown, 2-beat crossfade") != std::string::npos);
		CHECK(static_cast<int64_t>(land.beat) == dp + 4 * c.bars);
		CHECK_FALSE(d.snapshot().drop_cycle);
	}
}

TEST_CASE("v1.0.1 drop cycle: cut timing is sub-beat (fractional beat clock), ~0.23 s apart at 1/8")
{
	pd::Director d(cycle_set(true));
	const std::vector<float> m = track().to_float();
	const uint32_t sr = 44100;
	std::vector<double> times;
	std::size_t n_cuts = 0;
	for (std::size_t i = 0; i < m.size(); i += 256) {
		d.push_audio(m.data() + i, std::min<std::size_t>(256, m.size() - i), sr);
		const std::size_t c = cycle_cuts(d.events()).size();
		while (n_cuts < c) {
			times.push_back(static_cast<double>(i + 256) / sr);
			n_cuts++;
		}
	}
	REQUIRE(times.size() == 8u);
	for (std::size_t k = 1; k < times.size(); ++k) {
		INFO("cut " << k);
		CHECK(times[k] - times[k - 1] == doctest::Approx(60.0 / 128.0 / 2.0).epsilon(0.12)); // 0.234 s
	}
}

TEST_CASE("v1.0.1 drop cycle: fires or not per seed and chance; lock held on during the cycle")
{
	int fired = 0, skipped = 0;
	for (uint32_t seed = 1; seed <= 12; ++seed) {
		pd::Director d(cycle_set(true, 0.5f, 1, 2, seed));
		pdtest::feed(d, track().to_float(), 44100);
		const bool f = !cycle_cuts(d.events()).empty();
		CHECK(f == !has(d.events(), "no cycle"));
		if (!f) {
			// no cycle: the drop goes straight to its pick
			bool direct = false;
			for (const auto &e : pdtest::palette_changes(d.events()))
				direct = direct || e.detail.find("drop after breakdown") != std::string::npos;
			CHECK(direct);
		}
		(f ? fired : skipped)++;
	}
	CHECK(fired > 0);
	CHECK(skipped > 0);
	// same seed -> same decision
	pd::Director a(cycle_set(true, 0.5f, 1, 2, 3)), b(cycle_set(true, 0.5f, 1, 2, 3));
	pdtest::feed(a, track().to_float(), 44100);
	pdtest::feed(b, track().to_float(), 44100);
	CHECK(pdtest::summarize(a.events()) == pdtest::summarize(b.events()));
	// chance 0 never fires, chance 1 always does
	pd::Director z(cycle_set(true, 0.0f));
	pdtest::feed(z, track().to_float(), 44100);
	CHECK(cycle_cuts(z.events()).empty());

	// lock held on: random off at chance 1 + off on drop cannot take the lock away during the cycle
	pd::SetFile s = cycle_set(true);
	s.director.off_on_drop = true;
	s.director.drop_raw_bars = 2;
	pd::Director d(s);
	const std::vector<float> m = track().to_float();
	bool seen_cycle = false;
	for (std::size_t i = 0; i < m.size(); i += 512) {
		d.push_audio(m.data() + i, std::min<std::size_t>(512, m.size() - i), 44100);
		if (d.snapshot().drop_cycle) {
			seen_cycle = true;
			CHECK(d.snapshot().mix_target == 1.0f);
		}
	}
	CHECK(seen_cycle);
	CHECK(has(d.events(), "drop cycle replaces the raw bars"));
	CHECK_FALSE(has(d.events(), "raw visuals"));
}

TEST_CASE("v1.0.1 drop cycle: off on drop behaves as before when the cycle does not fire")
{
	pd::SetFile s = cycle_set(true, 0.0f);
	s.director.off_on_drop = true;
	pd::Director d(s);
	pdtest::feed(d, track().to_float(), 44100);
	CHECK(has(d.events(), "drop! raw visuals for 2 bars"));
	CHECK(cycle_cuts(d.events()).empty());
}

TEST_CASE("v1.0.1 drop cycle: cancelled by PANIC, pause, force off and manual picks (manual wins)")
{
	const std::vector<float> m = track().to_float();
	struct Case {
		pd::Command cmd;
		int idx;
		const char *why;
	};
	for (const Case c :
	     {Case{pd::Command::Panic, -1, "PANIC"}, Case{pd::Command::Pause, -1, "pause"},
	      Case{pd::Command::ForceOff, -1, "force off"}, Case{pd::Command::GoLive, 2, "manual pick wins"},
	      Case{pd::Command::Next, -1, "manual pick wins"}}) {
		INFO(c.why);
		pd::Director d(cycle_set(true));
		std::size_t i = 0;
		for (; i < m.size() && !d.snapshot().drop_cycle; i += 256)
			d.push_audio(m.data() + i, std::min<std::size_t>(256, m.size() - i), 44100);
		REQUIRE(d.snapshot().drop_cycle);
		d.push_command(c.cmd, c.idx);
		d.push_audio(m.data() + i, 256, 44100);
		i += 256;
		CHECK_FALSE(d.snapshot().drop_cycle);
		CHECK(has(d.events(), std::string("drop cycle cancelled (") + c.why + ")"));
		const std::size_t cuts = cycle_cuts(d.events()).size();
		const int live = d.snapshot().live_palette;
		if (c.cmd == pd::Command::GoLive)
			CHECK(live == 2);
		for (; i + 256 <= m.size(); i += 256)
			d.push_audio(m.data() + i, 256, 44100);
		CHECK(cycle_cuts(d.events()).size() == cuts); // no more cuts
		CHECK_FALSE(has(d.events(), "drop cycle over"));
	}
}

TEST_CASE("v1.0.1 drop cycle: manual-only trigger still cycles (lock behaviour), not on a plain drop")
{
	pd::SetFile s = cycle_set(true);
	s.palette_changes.trigger = pd::ChangeTrigger::Manual;
	pd::Director d(s);
	pdtest::feed(d, track().to_float(), 44100);
	CHECK(cycle_cuts(d.events()).size() == 8u);
	// a drop with no breakdown before it (breakdown palette unset -> no bd_active_) never cycles
	pd::SetFile s2 = cycle_set(true);
	s2.palette_changes.breakdown_palette.clear();
	pd::Director d2(s2);
	pdtest::feed(d2, track().to_float(), 44100);
	CHECK(cycle_cuts(d2.events()).empty());
	CHECK_FALSE(has(d2.events(), "drop cycle ("));
}

TEST_CASE("v1.0.1 drop cycle disabled: identical event log to v1.0.0 (no RNG roll)")
{
	// The same configuration with the behaviour off must replay exactly; and since the RNG is only
	// rolled when enabled, every later decision (random off, shuffle) is unchanged too.
	auto run = [](bool cycle_fields_set) {
		pd::SetFile s = pdtest::make_set(4, "Pal 3");
		s.palette_changes.trigger = pd::ChangeTrigger::Random;
		s.palette_changes.chance = 0.5f;
		s.palette_changes.order = pd::ChangeOrder::Shuffle;
		s.director.random_off = true;
		s.director.random_chance = 0.3f;
		s.director.seed = 12345;
		if (cycle_fields_set) { // disabled, but with non-default parameters
			s.director.drop_cycle = false;
			s.director.drop_cycle_chance = 1.0f;
			s.director.drop_cycle_bars = 4;
			s.director.drop_cycle_steps_per_beat = 4;
		}
		pd::Director d(s);
		pdtest::feed(d, pdtest::gen_breakdown(128.0, 8, 4, 16).to_float(), 44100);
		return pdtest::summarize(d.events());
	};
	const std::string a = run(false), b = run(true);
	CHECK(a == b);
	CHECK(a.find("drop cycle") == std::string::npos);
}
