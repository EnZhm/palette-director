#include "doctest.h"

#include "director.h"
#include "test_helpers.h"

TEST_CASE("force off (hold) and release")
{
	pd::SetFile s = pdtest::make_set(2);
	s.director.random_off = false;
	pd::Director d(s);

	d.push_command(pd::Command::ForceOff);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100); // drain
	CHECK(d.snapshot().mix_target == 0.0f);
	CHECK(d.snapshot().lock_on == false);

	d.push_command(pd::Command::ReleaseForce);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100);
	CHECK(d.snapshot().mix_target == 1.0f);
	CHECK(d.snapshot().lock_on == true);
}

TEST_CASE("force on holds the lock on")
{
	pd::SetFile s = pdtest::make_set(2);
	s.director.random_off = true;
	s.director.random_chance = 1.0f;
	pd::Director d(s);

	d.push_command(pd::Command::ForceOn);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100);
	CHECK(d.snapshot().mix_target == 1.0f);
	// even with random off at chance 1.0, forced on keeps the lock on
	pdtest::feed_roundtrip(d, pdtest::gen_kick(128.0, 16), pdtest::fixture_dir(), "force-on.wav");
	CHECK(d.snapshot().mix_target == 1.0f);
	CHECK(d.snapshot().lock_on == true);
}

TEST_CASE("panic: lock on, palette held, rotation reset")
{
	pd::SetFile s = pdtest::make_set(3);
	s.director.rotation = true;
	s.director.random_off = false;
	pd::Director d(s);

	// advance one phrase so rotation increments
	pdtest::feed_roundtrip(d, pdtest::gen_kick(128.0, 8), pdtest::fixture_dir(), "panic-a.wav");
	CHECK(d.snapshot().rotation_offset == 1);

	const int before = d.snapshot().live_palette;
	d.push_command(pd::Command::Panic);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100);
	CHECK(d.snapshot().rotation_offset == 0);
	CHECK(d.snapshot().lock_on == true);
	CHECK(d.snapshot().mix_target == 1.0f);
	CHECK(d.snapshot().live_palette == before);

	// director paused -> no further automatic changes
	const std::size_t n_ev = d.events().size();
	pdtest::feed_roundtrip(d, pdtest::gen_kick(128.0, 16), pdtest::fixture_dir(), "panic-b.wav");
	CHECK(d.events().size() == n_ev); // no new decisions
}

TEST_CASE("pause holds state; resume continues")
{
	pd::SetFile s = pdtest::make_set(3);
	s.palette_changes.trigger = pd::ChangeTrigger::Phrases;
	s.palette_changes.every_phrases = 1;
	s.palette_changes.order = pd::ChangeOrder::Sequence;
	s.director.phrase_beats = 32;
	s.director.random_off = false;
	pd::Director d(s);

	pdtest::feed_roundtrip(d, pdtest::gen_kick(128.0, 16), pdtest::fixture_dir(), "pause-a.wav");
	const std::size_t n1 = pdtest::palette_changes(d.events()).size();
	CHECK(n1 >= 1); // phrase boundaries at beat 32 (and 64) fire

	d.push_command(pd::Command::Pause);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100);
	pdtest::feed_roundtrip(d, pdtest::gen_kick(128.0, 16), pdtest::fixture_dir(), "pause-b.wav");
	CHECK(pdtest::palette_changes(d.events()).size() == n1); // no change while paused

	d.push_command(pd::Command::Resume);
	pdtest::feed(d, std::vector<float>(4410, 0.0f), 44100);
	pdtest::feed_roundtrip(d, pdtest::gen_kick(128.0, 16), pdtest::fixture_dir(), "pause-c.wav");
	CHECK(pdtest::palette_changes(d.events()).size() > n1); // resumes changing
}
