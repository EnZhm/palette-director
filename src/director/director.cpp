#include "director.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "analysis.h"
#include "beat_tracker.h"
#include "section_state.h"
#include "scheduler.h"

namespace pd {

namespace {
constexpr std::size_t kHop = 256;
constexpr double kSilenceThreshold = 1e-6; // full-band mean-square energy
constexpr double kSilenceSmooth = 0.05;    // one-pole coeff for the silence detector

// v1.0.1 adaptive bass reference (replaces v1.0.0's all-time peak, which never decayed: one
// loud moment and normal kicks never cleared 0.4 x peak again).
//  - ref_: instant attack, release time constant kRefRelease.
//  - lt_ : a slow follower of ref_ (kLtRelease); the reference never drops below kLtFloor x lt_, so a
//          long breakdown cannot drive it down to the level of pad / residue noise.
//  - kAbsFloor: absolute floor (bass energy, ~ -70 dB) so silence or noise never looks like a kick,
//          even at +24 dB input gain.
//  - kContrast: a kick must also rise above the beat's own minimum (rejects steady drones).
constexpr double kRefRelease = 10.0; // s (picked with tests/test_audio_level.cpp: 10-30 s window)
constexpr double kLtRelease = 60.0;  // s
constexpr double kLtFloor = 0.1;
constexpr double kAbsFloor = 1e-7;
constexpr double kContrast = 1.25;
constexpr double kMeterFall = 0.3; // s, peak meter fall time
constexpr double kClipHold = 1.5;  // s

const char *section_name(Section s)
{
	switch (s) {
	case Section::Groove:
		return "GROOVE";
	case Section::Breakdown:
		return "BREAKDOWN";
	case Section::Drop:
		return "DROP";
	}
	return "?";
}

int resolve_breakdown(const SetFile &s)
{
	const std::string &name = s.palette_changes.breakdown_palette;
	if (name.empty())
		return -1;
	for (std::size_t i = 0; i < s.palettes.size(); ++i) {
		if (s.palettes[i].name == name)
			return static_cast<int>(i);
	}
	return -1;
}

SchedulerConfig make_sched_cfg(const SetFile &s)
{
	SchedulerConfig c;
	c.palette_count = static_cast<int>(s.palettes.size());
	c.breakdown_index = resolve_breakdown(s);
	for (const auto &p : s.palettes) {
		c.names.push_back(p.name);
		c.colour_counts.push_back(static_cast<int>(p.colours.size()));
	}
	c.enabled = s.director.enabled;
	c.trigger = s.palette_changes.trigger;
	c.every_phrases = s.palette_changes.every_phrases;
	c.chance = s.palette_changes.chance;
	c.order = s.palette_changes.order;
	c.crossfade_beats = s.palette_changes.crossfade_beats;
	c.random_off = s.director.random_off;
	c.random_chance = s.director.random_chance;
	c.random_bars_min = s.director.random_bars_min;
	c.random_bars_max = s.director.random_bars_max;
	c.cooldown_phrases = s.director.cooldown_phrases;
	c.off_on_breakdown = s.director.off_on_breakdown;
	c.off_on_drop = s.director.off_on_drop;
	c.drop_raw_bars = s.director.drop_raw_bars;
	c.rotation = s.director.rotation;
	c.seed = s.director.seed;
	c.drop_cycle = s.director.drop_cycle;
	c.drop_cycle_chance = s.director.drop_cycle_chance;
	c.drop_cycle_bars = std::max(1, std::min(4, s.director.drop_cycle_bars));
	const int spb = s.director.drop_cycle_steps_per_beat;
	c.drop_cycle_steps_per_beat = (spb == 1 || spb == 4) ? spb : 2;
	return c;
}
} // namespace

float input_gain_linear(float db)
{
	const float d = db < -12.0f ? -12.0f : (db > 24.0f ? 24.0f : db);
	return static_cast<float>(std::pow(10.0, static_cast<double>(d) / 20.0));
}

struct Director::Impl {
	explicit Impl(const SetFile &s)
		: scheduler_(make_sched_cfg(s)),
		  analysis_(44100, kHop),
		  tracker_(44100, kHop, s.director.fallback_bpm)
	{
		apply(s);
	}

	void apply(const SetFile &s)
	{
		set_ = s;
		breakdown_index_ = resolve_breakdown(s);
		phrase_beats_ = s.director.phrase_beats;
		if (phrase_beats_ < 1)
			phrase_beats_ = 32;

		scheduler_.reset(make_sched_cfg(s));
		section_.reset();
		log_.clear();
		out_.clear();
		pending_.clear();
		beat_bass_fast_max_ = 0.0;
		beat_bass_fast_min_ = -1.0;
		silence_smooth_ = 0.0;
		reset_reference();
		kicks_ = 0;
		gain_ = input_gain_linear(s.director.input_gain_db);
		kick_thr_ = kick_threshold_for(s.director.kick_sensitivity);
		last_phrase_ = 0;
		phrase_inited_ = false;
		cur_section_ = Section::Groove;
		sample_rate_ = 0;
		CommandMsg m;
		while (queue_.pop(m)) {
		}
	}

	void append_events(const std::vector<Event> &evs)
	{
		for (Event e : evs) {
			e.phrase = e.beat / static_cast<uint64_t>(phrase_beats_);
			if (drain_enabled_)
				out_.push_back(e);
			if (keep_history_)
				log_.push_back(std::move(e));
		}
	}

	void update(const SetFile &s, int live)
	{
		const bool fallback_changed = s.director.fallback_bpm != set_.director.fallback_bpm;
		set_ = s;
		breakdown_index_ = resolve_breakdown(s);
		phrase_beats_ = s.director.phrase_beats;
		if (phrase_beats_ < 1)
			phrase_beats_ = 32;
		scheduler_.reconfigure(make_sched_cfg(s), live);
		if (fallback_changed)
			tracker_.set_fallback_bpm(s.director.fallback_bpm);
		gain_ = input_gain_linear(s.director.input_gain_db);
		kick_thr_ = kick_threshold_for(s.director.kick_sensitivity);
	}

	void reset_reference()
	{
		ref_ = 0.0;
		lt_ = 0.0;
		bass_fast_ = 0.0;
		meter_peak_ = 0.0;
		clip_hold_ = 0.0;
	}

	double ref_eff() const { return std::max({ref_, kLtFloor * lt_, kAbsFloor}); }

	void drain_commands()
	{
		CommandMsg m;
		while (queue_.pop(m)) {
			const uint64_t beat = static_cast<uint64_t>(std::floor(tracker_.beats()));
			auto evs = scheduler_.command(m.cmd, m.palette_index, beat);
			append_events(evs);
		}
	}

	void on_beat(uint64_t b)
	{
		const double bass_fast_max = beat_bass_fast_max_;
		const double bass_fast_min = beat_bass_fast_min_ < 0.0 ? 0.0 : beat_bass_fast_min_;
		beat_bass_fast_max_ = 0.0;
		beat_bass_fast_min_ = -1.0;

		const bool silent = silence_smooth_ < kSilenceThreshold;
		const bool kick = !silent && bass_fast_max > kAbsFloor &&
				  bass_fast_max > static_cast<double>(kick_thr_) * ref_eff() &&
				  bass_fast_max > kContrast * bass_fast_min;
		if (kick)
			kicks_++;

		if (silent)
			section_.hold(b);
		if (!silent) {
			const Section sec = section_.update(b, kick, phrase_beats_);
			if (sec != cur_section_) {
				cur_section_ = sec;
				Event e;
				e.type = Event::Type::SectionChange;
				e.beat = b;
				e.section = sec;
				e.detail = std::string("section -> ") + section_name(sec);
				append_events({e});
				append_events(scheduler_.on_section_change(sec, b));
			}
		}

		const uint64_t ph = b / static_cast<uint64_t>(phrase_beats_);
		if (!phrase_inited_) {
			last_phrase_ = ph;
			phrase_inited_ = true;
		} else if (ph != last_phrase_) {
			last_phrase_ = ph;
			if (!silent)
				append_events(scheduler_.on_phrase(ph, b));
		}

		append_events(scheduler_.on_beat(b));
	}

	DirectorState snapshot_state() const
	{
		DirectorState st;
		const uint64_t beat = static_cast<uint64_t>(std::floor(tracker_.beats()));
		st.bpm = static_cast<float>(tracker_.bpm());
		st.confidence = static_cast<float>(tracker_.confidence());
		const double ref = ref_eff();
		st.bass = static_cast<float>(std::min(1.0, bass_fast_ / ref));
		st.mix_target = static_cast<float>(scheduler_.mix_target(beat));
		st.rotation_offset = scheduler_.rotation_offset();
		st.section = cur_section_;
		st.beat = beat;
		st.phrase = beat / static_cast<uint64_t>(phrase_beats_);
		st.live_palette = scheduler_.live_palette();
		st.prev_palette = scheduler_.prev_palette();
		st.pfade = scheduler_.pfade();
		st.cooldown_phrases_left = scheduler_.cooldown_left();
		st.lock_on = st.mix_target >= 0.5f;
		const double bf = tracker_.beats();
		st.beat_frac = static_cast<float>(bf - std::floor(bf));
		st.next_palette = scheduler_.peek_next();
		st.paused = scheduler_.paused();
		st.panic = scheduler_.panic();
		st.forced_on = scheduler_.forced_on();
		st.force_off = scheduler_.force_off();
		st.enabled = set_.director.enabled;
		st.silent = silence_smooth_ < kSilenceThreshold;
		st.input_db = meter_peak_ > 1e-6 ? static_cast<float>(20.0 * std::log10(meter_peak_)) : -120.0f;
		st.clip = clip_hold_ > 0.0;
		st.bass_level = st.bass;
		st.kick_threshold = kick_thr_;
		st.kicks = kicks_;
		st.drop_cycle = scheduler_.drop_cycle_active();
		return st;
	}

	SetFile set_;
	int breakdown_index_ = -1;
	uint32_t sample_rate_ = 0;

	Scheduler scheduler_;
	Analysis analysis_;
	BeatTracker tracker_;
	SectionState section_;
	CommandQueue queue_;
	std::vector<Event> log_;
	std::vector<Event> out_;
	bool keep_history_ = true;
	bool drain_enabled_ = false;

	std::vector<float> pending_;

	double beat_bass_fast_max_ = 0.0;
	double beat_bass_fast_min_ = -1.0;
	double silence_smooth_ = 0.0;
	double ref_ = 0.0; // adaptive bass reference (see kRefRelease)
	double lt_ = 0.0;  // slow follower of ref_
	double bass_fast_ = 0.0;
	double meter_peak_ = 0.0;
	double clip_hold_ = 0.0;
	uint32_t kicks_ = 0;
	float gain_ = 1.0f;
	float kick_thr_ = 0.4f;

	int phrase_beats_ = 32;
	uint64_t last_phrase_ = 0;
	bool phrase_inited_ = false;
	Section cur_section_ = Section::Groove;
};

Director::Director(const SetFile &s) : impl_(new Impl(s)) {}
Director::~Director() = default;
Director::Director(Director &&) noexcept = default;
Director &Director::operator=(Director &&) noexcept = default;

void Director::push_audio(const float *mono, std::size_t frames, uint32_t sample_rate)
{
	if (!impl_ || frames == 0)
		return;
	Impl &d = *impl_;

	d.drain_commands();

	if (sample_rate != d.sample_rate_) {
		d.sample_rate_ = sample_rate;
		d.analysis_ = Analysis(sample_rate, kHop);
		d.tracker_ = BeatTracker(sample_rate, kHop, d.set_.director.fallback_bpm);
		d.pending_.clear();
	}

	if (d.gain_ == 1.0f) {
		d.pending_.insert(d.pending_.end(), mono, mono + frames);
	} else {
		const std::size_t at = d.pending_.size();
		d.pending_.resize(at + frames);
		for (std::size_t i = 0; i < frames; ++i)
			d.pending_[at + i] = mono[i] * d.gain_;
	}

	const double dt = static_cast<double>(kHop) / static_cast<double>(sample_rate);
	const double ref_rel = std::exp(-dt / kRefRelease);
	const double lt_coeff = 1.0 - std::exp(-dt / kLtRelease);
	const double meter_rel = std::exp(-dt / kMeterFall);
	std::size_t pos = 0;
	while (pos + kHop <= d.pending_.size()) {
		const float *base = d.pending_.data() + pos;
		const AnalysisResult ar = d.analysis_.process(base, kHop);

		d.silence_smooth_ += (ar.energy.total - d.silence_smooth_) * kSilenceSmooth;
		const bool silent = d.silence_smooth_ < kSilenceThreshold;

		d.ref_ = std::max(d.ref_ * ref_rel, ar.bass_fast);
		d.lt_ += (d.ref_ - d.lt_) * lt_coeff;
		d.bass_fast_ = ar.bass_fast;
		d.beat_bass_fast_max_ = std::max(d.beat_bass_fast_max_, ar.bass_fast);
		d.beat_bass_fast_min_ = d.beat_bass_fast_min_ < 0.0 ? ar.bass_fast
								    : std::min(d.beat_bass_fast_min_, ar.bass_fast);

		float hop_peak = 0.0f;
		for (std::size_t i = 0; i < kHop; ++i)
			hop_peak = std::max(hop_peak, std::fabs(base[i]));
		d.meter_peak_ = std::max(d.meter_peak_ * meter_rel, static_cast<double>(hop_peak));
		if (hop_peak > 0.891f) // -1 dBFS
			d.clip_hold_ = kClipHold;
		else
			d.clip_hold_ = std::max(0.0, d.clip_hold_ - dt);

		const double before = d.tracker_.beats();
		d.tracker_.process(ar.onset, silent, dt);
		const double after = d.tracker_.beats();

		const int64_t b0 = static_cast<int64_t>(std::floor(before));
		const int64_t b1 = static_cast<int64_t>(std::floor(after));
		for (int64_t b = b0 + 1; b <= b1; ++b)
			d.on_beat(static_cast<uint64_t>(b));
		if (d.scheduler_.drop_cycle_active())
			d.append_events(d.scheduler_.on_tick(after));

		d.scheduler_.advance_pfade(dt, d.tracker_.bpm());

		pos += kHop;
	}
	d.pending_.erase(d.pending_.begin(), d.pending_.begin() + static_cast<std::ptrdiff_t>(pos));
}

DirectorState Director::snapshot() const
{
	return impl_ ? impl_->snapshot_state() : DirectorState{};
}

void Director::push_command(Command cmd, int palette_index)
{
	if (!impl_)
		return;
	impl_->queue_.push(CommandMsg{cmd, palette_index});
}

void Director::apply_set(const SetFile &s)
{
	if (!impl_)
		return;
	impl_->apply(s);
}

void Director::update_set(const SetFile &s, int live_palette)
{
	if (!impl_)
		return;
	impl_->update(s, live_palette);
}

void Director::drain_events(std::vector<Event> &out)
{
	if (!impl_)
		return;
	impl_->drain_enabled_ = true;
	out.insert(out.end(), impl_->out_.begin(), impl_->out_.end());
	impl_->out_.clear();
}

void Director::reset_audio_reference()
{
	if (impl_)
		impl_->reset_reference();
}

void Director::set_keep_history(bool keep)
{
	if (!impl_)
		return;
	impl_->keep_history_ = keep;
	if (!keep)
		impl_->log_.clear();
}

const std::vector<Event> &Director::events() const
{
	static const std::vector<Event> kEmpty;
	return impl_ ? impl_->log_ : kEmpty;
}

} // namespace pd
