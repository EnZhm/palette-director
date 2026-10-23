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
	return c;
}
} // namespace

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
		silence_smooth_ = 0.0;
		bass_peak_ = 0.0;
		bass_fast_ = 0.0;
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
	}

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
		beat_bass_fast_max_ = 0.0;

		const bool silent = silence_smooth_ < kSilenceThreshold;
		const double peak = std::max(bass_peak_, 1e-9);
		const bool kick = !silent && (bass_fast_max > 0.4 * peak);

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
		const double peak = std::max(bass_peak_, 1e-9);
		st.bass = static_cast<float>(std::min(1.0, bass_fast_ / peak));
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
		st.forced_on = scheduler_.forced_on();
		st.force_off = scheduler_.force_off();
		st.enabled = set_.director.enabled;
		st.silent = silence_smooth_ < kSilenceThreshold;
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
	double silence_smooth_ = 0.0;
	double bass_peak_ = 0.0;
	double bass_fast_ = 0.0;

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

	d.pending_.insert(d.pending_.end(), mono, mono + frames);

	const double dt = static_cast<double>(kHop) / static_cast<double>(sample_rate);
	std::size_t pos = 0;
	while (pos + kHop <= d.pending_.size()) {
		const float *base = d.pending_.data() + pos;
		const AnalysisResult ar = d.analysis_.process(base, kHop);

		d.silence_smooth_ += (ar.energy.total - d.silence_smooth_) * kSilenceSmooth;
		const bool silent = d.silence_smooth_ < kSilenceThreshold;

		d.bass_peak_ = std::max(d.bass_peak_, ar.bass_fast);
		d.bass_fast_ = ar.bass_fast;
		d.beat_bass_fast_max_ = std::max(d.beat_bass_fast_max_, ar.bass_fast);

		const double before = d.tracker_.beats();
		d.tracker_.process(ar.onset, silent, dt);
		const double after = d.tracker_.beats();

		const int64_t b0 = static_cast<int64_t>(std::floor(before));
		const int64_t b1 = static_cast<int64_t>(std::floor(after));
		for (int64_t b = b0 + 1; b <= b1; ++b)
			d.on_beat(static_cast<uint64_t>(b));

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
