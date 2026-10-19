#include "gen_fixtures.h"

#include <cmath>

namespace pdtest {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Add a kick (decaying 55 Hz cosine burst) starting at `start` sample.
void add_kick(std::vector<int16_t> &s, uint32_t sr, std::size_t start)
{
	const std::size_t n = static_cast<std::size_t>(0.25 * sr);
	for (std::size_t i = 0; i < n && start + i < s.size(); ++i) {
		const double t = static_cast<double>(i) / static_cast<double>(sr);
		const double env = std::exp(-t / 0.08);
		const double v = 0.8 * env * std::cos(2.0 * kPi * 55.0 * t);
		s[start + i] = static_cast<int16_t>(std::lround(std::max(-1.0, std::min(1.0, v)) * 32767.0));
	}
	// click transient to sharpen the onset
	for (std::size_t i = 0; i < 2 && start + i < s.size(); ++i) {
		s[start + i] = 32767;
	}
}

void add_pad(std::vector<int16_t> &s, uint32_t sr, std::size_t start, std::size_t len)
{
	for (std::size_t i = 0; i < len && start + i < s.size(); ++i) {
		const double t = static_cast<double>(i) / static_cast<double>(sr);
		const double v = 0.04 * std::sin(2.0 * kPi * 440.0 * t);
		s[start + i] = static_cast<int16_t>(std::lround(v * 32767.0));
	}
}
} // namespace

WavMono gen_kick(double bpm, int bars, uint32_t sample_rate)
{
	WavMono w;
	w.sample_rate = sample_rate;
	const double spb = 60.0 / bpm * sample_rate;
	const int beats = bars * 4;
	const std::size_t total =
		static_cast<std::size_t>(std::ceil(beats * spb)) + static_cast<std::size_t>(0.3 * sample_rate);
	w.samples.assign(total, 0);
	for (int b = 0; b < beats; ++b) {
		add_kick(w.samples, sample_rate, static_cast<std::size_t>(std::llround(b * spb)));
	}
	return w;
}

WavMono gen_breakdown(double bpm, int groove_bars, int breakdown_bars, int drop_bars, uint32_t sample_rate)
{
	WavMono w;
	w.sample_rate = sample_rate;
	const double spb = 60.0 / bpm * sample_rate;
	const int total_beats = (groove_bars + breakdown_bars + drop_bars) * 4;
	const std::size_t total =
		static_cast<std::size_t>(std::ceil(total_beats * spb)) + static_cast<std::size_t>(0.3 * sample_rate);
	w.samples.assign(total, 0);

	const int groove_beats = groove_bars * 4;
	const int breakdown_beats = breakdown_bars * 4;
	const int drop_start_beat = groove_beats + breakdown_beats;

	for (int b = 0; b < groove_beats; ++b) {
		add_kick(w.samples, sample_rate, static_cast<std::size_t>(std::llround(b * spb)));
	}
	add_pad(w.samples, sample_rate, static_cast<std::size_t>(std::llround(groove_beats * spb)),
		static_cast<std::size_t>(breakdown_beats * spb));
	for (int b = drop_start_beat; b < total_beats; ++b) {
		add_kick(w.samples, sample_rate, static_cast<std::size_t>(std::llround(b * spb)));
	}
	return w;
}

WavMono gen_silence(double seconds, uint32_t sample_rate)
{
	WavMono w;
	w.sample_rate = sample_rate;
	w.samples.assign(static_cast<std::size_t>(seconds * sample_rate), 0);
	return w;
}

} // namespace pdtest
