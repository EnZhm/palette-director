#include "doctest.h"

#include "director.h"
#include "test_helpers.h"

TEST_CASE("same seed -> identical event log")
{
	const std::string dir = pdtest::fixture_dir();

	auto run = [&]() {
		pd::SetFile s = pdtest::make_set(4, "Pal 3");
		s.palette_changes.trigger = pd::ChangeTrigger::Random;
		s.palette_changes.chance = 0.5f;
		s.palette_changes.order = pd::ChangeOrder::Shuffle;
		s.director.random_off = true;
		s.director.random_chance = 0.3f;
		s.director.seed = 12345;
		pd::Director d(s);
		pdtest::feed_roundtrip(d, pdtest::gen_breakdown(128.0, 8, 4, 4), dir, "det.wav");
		return d;
	};

	pd::Director a = run();
	pd::Director b = run();
	REQUIRE(a.events().size() == b.events().size());
	CHECK(pdtest::summarize(a.events()) == pdtest::summarize(b.events()));
}
