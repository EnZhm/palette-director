#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>

namespace pd {

// BPM estimation (autocorrelation of the onset envelope, 90-180 BPM) plus a soft
// phase-lock that keeps the beat grid aligned to onsets without jumps.
class BeatTracker {
public:
	BeatTracker(uint32_t sample_rate, std::size_t hop, double fallback_bpm);

	// Feed one onset sample + silence flag; advances the beat grid by dt seconds.
	void process(double onset, bool silent, double dt);

	double bpm() const { return effective_bpm_; } // tempo the grid runs at
	double estimated_bpm() const { return estimated_bpm_; }
	double confidence() const { return confidence_; }
	double beats() const { return beats_; }
	void reset();

private:
	double autocorr(std::size_t lag) const;

	uint32_t sample_rate_;
	std::size_t hop_;
	double env_rate_;
	double fallback_bpm_;

	double beats_ = 0.0;
	double estimated_bpm_ = 0.0;
	double confidence_ = 0.0;
	double effective_bpm_;
	bool have_lock_ = false;
	double locked_bpm_ = 0.0;

	std::deque<double> onset_buf_;
	std::size_t min_buf_ = 0;
	std::size_t max_buf_ = 0;
	std::size_t lag_min_ = 0;
	std::size_t lag_max_ = 0;
	double conf_threshold_ = 0.30;

	double onset_peak_ = 0.0;
};

} // namespace pd
