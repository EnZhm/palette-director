#include "analysis.h"

#include <cmath>

namespace pd {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kQ = 0.7071067811865476; // Butterworth

void lowpass(double f0, double fs, double &b0, double &b1, double &b2, double &a1, double &a2)
{
	const double w = 2.0 * kPi * f0 / fs;
	const double alpha = std::sin(w) / (2.0 * kQ);
	const double cw = std::cos(w);
	const double a0 = 1.0 + alpha;
	b0 = (1.0 - cw) / 2.0 / a0;
	b1 = (1.0 - cw) / a0;
	b2 = (1.0 - cw) / 2.0 / a0;
	a1 = (-2.0 * cw) / a0;
	a2 = (1.0 - alpha) / a0;
}

void highpass(double f0, double fs, double &b0, double &b1, double &b2, double &a1, double &a2)
{
	const double w = 2.0 * kPi * f0 / fs;
	const double alpha = std::sin(w) / (2.0 * kQ);
	const double cw = std::cos(w);
	const double a0 = 1.0 + alpha;
	b0 = (1.0 + cw) / 2.0 / a0;
	b1 = -(1.0 + cw) / a0;
	b2 = (1.0 + cw) / 2.0 / a0;
	a1 = (-2.0 * cw) / a0;
	a2 = (1.0 - alpha) / a0;
}

} // namespace

Analysis::Analysis(uint32_t sample_rate, std::size_t hop) : sample_rate_(sample_rate), hop_(hop ? hop : 256)
{
	const double fs = static_cast<double>(sample_rate_);
	lowpass(150.0, fs, lp150_.b0, lp150_.b1, lp150_.b2, lp150_.a1, lp150_.a2);
	lowpass(2000.0, fs, lp2000_.b0, lp2000_.b1, lp2000_.b2, lp2000_.a1, lp2000_.a2);
	highpass(2000.0, fs, hp2000_.b0, hp2000_.b1, hp2000_.b2, hp2000_.a1, hp2000_.a2);
	const double hop_s = static_cast<double>(hop_) / static_cast<double>(sample_rate_);
	fast_coeff_ = 1.0 - std::exp(-hop_s / 0.020); // ~20 ms
	slow_coeff_ = 1.0 - std::exp(-hop_s / 0.300); // ~300 ms
}

void Analysis::reset()
{
	lp150_.reset();
	lp2000_.reset();
	hp2000_.reset();
	bass_fast_ = bass_slow_ = prev_bass_fast_ = 0.0;
}

AnalysisResult Analysis::process(const float *mono, std::size_t n)
{
	double sb = 0.0, sm = 0.0, sh = 0.0;
	for (std::size_t i = 0; i < n; ++i) {
		const double x = mono[i];
		const double b = lp150_.process(x);
		const double l2 = lp2000_.process(x);
		const double h = hp2000_.process(x);
		const double m = l2 - b; // 150 Hz .. 2 kHz
		sb += b * b;
		sm += m * m;
		sh += h * h;
	}
	const double inv = 1.0 / static_cast<double>(n);
	AnalysisResult r;
	r.energy.bass = sb * inv;
	r.energy.mid = sm * inv;
	r.energy.high = sh * inv;
	r.energy.total = r.energy.bass + r.energy.mid + r.energy.high;

	bass_fast_ += (r.energy.bass - bass_fast_) * fast_coeff_;
	bass_slow_ += (r.energy.bass - bass_slow_) * slow_coeff_;
	r.bass_fast = bass_fast_;
	r.bass_slow = bass_slow_;
	// Onset = rise of the fast bass envelope (smoothes the 110 Hz carrier ripple of a
	// 55 Hz kick), one clean bump per kick.
	r.onset = (bass_fast_ > prev_bass_fast_) ? (bass_fast_ - prev_bass_fast_) : 0.0;
	prev_bass_fast_ = bass_fast_;
	return r;
}

} // namespace pd
