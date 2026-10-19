#pragma once
#include <cstddef>
#include <cstdint>

namespace pd {

// Three-band energy split (biquad filters) plus bass onset detection.
// Process mono float samples in fixed-size hops; no libobs.
struct BandEnergy {
	double bass = 0.0;  // < 150 Hz
	double mid = 0.0;   // 150 Hz - 2 kHz
	double high = 0.0;  // > 2 kHz
	double total = 0.0; // bass + mid + high (~ full band)
};

struct AnalysisResult {
	BandEnergy energy;
	double bass_fast = 0.0; // fast envelope of bass energy (kick transients)
	double bass_slow = 0.0; // slow envelope (beat-level bass presence)
	double onset = 0.0;     // bass flux (positive energy rise), drives BPM + phase lock
};

class Analysis {
public:
	Analysis(uint32_t sample_rate, std::size_t hop = 256);
	Analysis(const Analysis &) = default;
	Analysis &operator=(const Analysis &) = default;

	// Feed one hop of mono samples; returns the per-hop analysis.
	AnalysisResult process(const float *mono, std::size_t n);

	void reset();

private:
	uint32_t sample_rate_;
	std::size_t hop_;
	double fast_coeff_ = 0.0;
	double slow_coeff_ = 0.0;

	struct Biquad {
		double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
		double z1 = 0, z2 = 0;
		void reset() { z1 = z2 = 0; }
		double process(double x)
		{
			const double y = b0 * x + z1;
			z1 = b1 * x - a1 * y + z2;
			z2 = b2 * x - a2 * y;
			return y;
		}
	};

	Biquad lp150_;
	Biquad lp2000_;
	Biquad hp2000_;

	double bass_fast_ = 0.0;
	double bass_slow_ = 0.0;
	double prev_bass_fast_ = 0.0;
};

} // namespace pd
