#include "doctest.h"

#include <cmath>
#include <vector>

#include "director.h"
#include "test_helpers.h"

using namespace pdtest;

// P4 regression (found in the OBS sandbox): during a breakdown the onset envelope is only
// numerical residue of the pad; its periodic pattern used to win the autocorrelation
// (181 BPM at conf 0.96), so the beat grid ran fast and the drop landed off the phrase grid.
TEST_CASE("breakdown with no bass holds the locked BPM and the drop lands on the beat grid")
{
	pd::SetFile s = pdtest::make_set(4, "Pal 3");
	s.director.random_off = false;
	pd::Director d(s);
	for (uint32_t sr : {44100u, 48000u}) {
		pd::Director dd(s);
		const WavMono w = pdtest::gen_breakdown(128.0, 32, 8, 16, sr);
		const std::vector<float> mono = w.to_float();
		const std::size_t bd_start = static_cast<std::size_t>(32 * 4 * 60.0 / 128.0 * sr);
		const std::size_t bd_end = static_cast<std::size_t>(40 * 4 * 60.0 / 128.0 * sr);
		float worst = 0.0f;
		for (std::size_t i = 0; i < mono.size(); i += 1024) {
			const std::size_t n = std::min<std::size_t>(1024, mono.size() - i);
			dd.push_audio(mono.data() + i, n, sr);
			if (i > bd_start && i < bd_end)
				worst = std::max(worst, std::fabs(dd.snapshot().bpm - 128.0f));
		}
		CHECK(worst <= 1.0f);
		const int64_t drop = pdtest::section_beat(dd.events(), pd::Section::Drop);
		CHECK(drop >= 159);
		CHECK(drop <= 161);
	}
}
