#include "beat_tracker.h"

#include <algorithm>
#include <cmath>

namespace pd {

BeatTracker::BeatTracker(uint32_t sample_rate, std::size_t hop, double fallback_bpm)
	: sample_rate_(sample_rate),
	  hop_(hop ? hop : 256),
	  env_rate_(static_cast<double>(sample_rate_) / static_cast<double>(hop_)),
	  fallback_bpm_(fallback_bpm),
	  effective_bpm_(fallback_bpm)
{
	min_buf_ = static_cast<std::size_t>(env_rate_ * 3.0); // ~3 s before estimating
	max_buf_ = static_cast<std::size_t>(env_rate_ * 8.0); // ~8 s window
	lag_min_ = static_cast<std::size_t>(env_rate_ * 60.0 / 180.0);
	lag_max_ = static_cast<std::size_t>(env_rate_ * 60.0 / 90.0);
}

void BeatTracker::reset()
{
	beats_ = 0.0;
	estimated_bpm_ = 0.0;
	confidence_ = 0.0;
	effective_bpm_ = fallback_bpm_;
	have_lock_ = false;
	locked_bpm_ = 0.0;
	onset_buf_.clear();
	onset_peak_ = 0.0;
}

double BeatTracker::autocorr(std::size_t lag) const
{
	const std::size_t n = onset_buf_.size();
	double s = 0.0;
	for (std::size_t i = lag; i < n; ++i)
		s += onset_buf_[i] * onset_buf_[i - lag];
	return s;
}

void BeatTracker::process(double onset, bool silent, double dt)
{
	onset_buf_.push_back(onset);
	if (onset_buf_.size() > max_buf_)
		onset_buf_.pop_front();

	// BPM + confidence from autocorrelation of the onset envelope.
	estimated_bpm_ = 0.0;
	confidence_ = 0.0;
	const std::size_t n = onset_buf_.size();
	if (n >= min_buf_ && n > lag_max_ + 1) {
		const double e0 = autocorr(0);
		if (e0 > 1e-12) {
			std::size_t best_lag = 0;
			double best_val = -1.0;
			for (std::size_t lag = lag_min_; lag <= lag_max_ && lag < n; ++lag) {
				const double s = autocorr(lag);
				if (s > best_val) {
					best_val = s;
					best_lag = lag;
				}
			}
			if (best_lag > 0 && best_lag < n - 1) {
				const double y0 = autocorr(best_lag - 1);
				const double y1 = best_val;
				const double y2 = autocorr(best_lag + 1);
				const double denom = y0 - 2.0 * y1 + y2;
				const double delta = (denom != 0.0) ? 0.5 * (y0 - y2) / denom : 0.0;
				const double lagf = static_cast<double>(best_lag) + delta;
				if (lagf > 0.0)
					estimated_bpm_ = 60.0 * env_rate_ / lagf;
				confidence_ = best_val / e0;
			}
		}
	}

	if (confidence_ >= conf_threshold_) {
		have_lock_ = true;
		locked_bpm_ = estimated_bpm_;
	}

	// Effective tempo: fallback on silence / no lock, hold the last lock through a
	// breakdown, use the estimate once confident.
	if (silent) {
		effective_bpm_ = fallback_bpm_;
	} else if (confidence_ >= conf_threshold_) {
		effective_bpm_ = estimated_bpm_;
	} else if (have_lock_) {
		effective_bpm_ = locked_bpm_;
	} else {
		effective_bpm_ = fallback_bpm_;
	}

	beats_ += dt * effective_bpm_ / 60.0;

	// Soft phase-lock: a strong onset near a beat pulls the grid halfway toward it.
	onset_peak_ = std::max(onset_peak_ * 0.999, onset);
	if (onset > 0.5 * onset_peak_ && onset_peak_ > 1e-12) {
		const double frac = beats_ - std::floor(beats_);
		const double err = (frac < 0.5) ? frac : (frac - 1.0);
		if (std::fabs(err) < 0.25)
			beats_ -= 0.5 * err;
	}
}

} // namespace pd
