#pragma once
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "director.h"
#include "gen_fixtures.h"

namespace pdtest {

// Build a set with n named palettes (4 colours each), defaults matching the mock DEF_DIR.
inline pd::SetFile make_set(int n_palettes, const std::string &breakdown_name = "")
{
	pd::SetFile s;
	s.name = "Test Set";
	for (int i = 0; i < n_palettes; ++i) {
		pd::PaletteSpec p;
		p.name = "Pal " + std::to_string(i);
		p.colours = {{0.0f, 0.0f, 0.0f}, {0.3f, 0.3f, 0.3f}, {0.7f, 0.7f, 0.7f}, {1.0f, 0.0f, 0.0f}};
		s.palettes.push_back(p);
	}
	s.palette_changes.breakdown_palette = breakdown_name;
	return s;
}

// Feed a mono float buffer in fixed-size blocks.
inline void feed(pd::Director &d, const std::vector<float> &mono, uint32_t sr, std::size_t block = 1024)
{
	for (std::size_t i = 0; i < mono.size(); i += block) {
		const std::size_t n = std::min(block, mono.size() - i);
		d.push_audio(mono.data() + i, n, sr);
	}
}

// Generate -> write WAV -> read back -> feed (full WAV round-trip, spec 4.3).
inline void feed_roundtrip(pd::Director &d, const WavMono &w, const std::string &dir, const std::string &name,
			   std::size_t block = 1024)
{
	const std::string path = dir + "/" + name;
	REQUIRE(w.write(path));
	WavMono back;
	REQUIRE(WavMono::read(path, back));
	REQUIRE(back.sample_rate == w.sample_rate);
	REQUIRE(back.samples.size() == w.samples.size());
	feed(d, back.to_float(), back.sample_rate, block);
}

inline std::string fixture_dir()
{
	const std::filesystem::path p = std::filesystem::temp_directory_path() / "pd_director_tests";
	std::filesystem::create_directories(p);
	return p.string();
}

inline std::vector<pd::Event> palette_changes(const std::vector<pd::Event> &evs)
{
	std::vector<pd::Event> out;
	for (const auto &e : evs)
		if (e.type == pd::Event::Type::PaletteChange)
			out.push_back(e);
	return out;
}

inline std::vector<pd::Event> section_changes(const std::vector<pd::Event> &evs)
{
	std::vector<pd::Event> out;
	for (const auto &e : evs)
		if (e.type == pd::Event::Type::SectionChange)
			out.push_back(e);
	return out;
}

inline std::vector<pd::Event> lock_offs(const std::vector<pd::Event> &evs)
{
	std::vector<pd::Event> out;
	for (const auto &e : evs)
		if (e.type == pd::Event::Type::LockOff)
			out.push_back(e);
	return out;
}

inline std::vector<pd::Event> lock_ons(const std::vector<pd::Event> &evs)
{
	std::vector<pd::Event> out;
	for (const auto &e : evs)
		if (e.type == pd::Event::Type::LockOn)
			out.push_back(e);
	return out;
}

// Find the beat of the first event with the given section.
inline int64_t section_beat(const std::vector<pd::Event> &evs, pd::Section sec)
{
	for (const auto &e : evs)
		if (e.type == pd::Event::Type::SectionChange && e.section == sec)
			return static_cast<int64_t>(e.beat);
	return -1;
}

// Deterministic serialization of an event log (for the determinism test).
inline std::string summarize(const std::vector<pd::Event> &evs)
{
	std::ostringstream os;
	for (const auto &e : evs) {
		os << static_cast<int>(e.type) << '|' << e.beat << '|' << e.phrase << '|' << static_cast<int>(e.section)
		   << '|' << e.palette << '|' << e.prev_palette << '|' << e.detail << ';';
	}
	return os.str();
}

} // namespace pdtest
