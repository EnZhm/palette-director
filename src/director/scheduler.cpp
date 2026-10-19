#include "scheduler.h"

#include <algorithm>
#include <climits>

namespace pd {

void Scheduler::reset(const SchedulerConfig &cfg)
{
	cfg_ = cfg;
	rng_.reseed(cfg.seed);
	live_ = 0;
	prev_ = -1;
	pfade_ = 1.0f;
	rot_ = 0;
	bd_active_ = false;
	last_normal_ = -1;
	order_.clear();
	cooldown_ = 0;
	off_until_ = -1;
	off_reason_.clear();
	paused_ = false;
	force_off_ = false;
	forced_on_ = false;
}

std::vector<int> Scheduler::normal_idx() const
{
	std::vector<int> pool;
	pool.reserve(static_cast<std::size_t>(cfg_.palette_count));
	for (int i = 0; i < cfg_.palette_count; ++i) {
		if (i != cfg_.breakdown_index)
			pool.push_back(i);
	}
	return pool;
}

int Scheduler::pick_next(int dir)
{
	const std::vector<int> pool = normal_idx();
	if (pool.empty())
		return live_;

	if (cfg_.order == ChangeOrder::Shuffle && dir == 1) {
		if (order_.empty()) {
			order_.clear();
			for (int i : pool) {
				if (i != live_)
					order_.push_back(i);
			}
			// Fisher-Yates (mock v2 pickNext), consuming the shared RNG.
			for (std::size_t i = order_.size(); i-- > 1;) {
				const std::size_t j =
					static_cast<std::size_t>(rng_.next() * static_cast<double>(i + 1));
				std::swap(order_[i], order_[j]);
			}
		}
		return order_.empty() ? live_ : order_[0];
	}

	auto it = std::find(pool.begin(), pool.end(), live_);
	int base;
	if (it != pool.end()) {
		base = live_;
	} else if (last_normal_ >= 0) {
		base = last_normal_;
	} else {
		base = pool[0];
	}
	const auto bit = std::find(pool.begin(), pool.end(), base);
	const int idx = static_cast<int>(std::distance(pool.begin(), bit));
	const int n = static_cast<int>(pool.size());
	return pool[((idx + dir) % n + n) % n];
}

std::vector<Event> Scheduler::go_live(int i, const std::string &why, uint64_t beat)
{
	if (i == live_)
		return {};

	prev_ = live_;
	live_ = i;
	pfade_ = (cfg_.crossfade_beats <= 0.0f) ? 1.0f : 0.0f;
	rot_ = 0;
	if (i != cfg_.breakdown_index)
		last_normal_ = i;
	if (!order_.empty() && order_.front() == i)
		order_.erase(order_.begin());

	Event e;
	e.type = Event::Type::PaletteChange;
	e.beat = beat;
	e.palette = i;
	e.prev_palette = prev_;
	const std::string nm = (i >= 0 && i < static_cast<int>(cfg_.names.size()))
				       ? cfg_.names[static_cast<std::size_t>(i)]
				       : std::to_string(i);
	e.detail = "palette -> " + nm + " (" + why;
	if (cfg_.crossfade_beats > 0.0f) {
		e.detail += ", " + std::to_string(cfg_.crossfade_beats) + "-beat crossfade";
	} else {
		e.detail += ", cut";
	}
	e.detail += ")";
	return {e};
}

std::vector<Event> Scheduler::on_section_change(Section sec, uint64_t beat)
{
	if (!cfg_.enabled || paused_)
		return {};
	std::vector<Event> out;

	if (sec == Section::Breakdown) {
		if (cfg_.off_on_breakdown) {
			off_until_ = INT64_MAX;
			off_reason_ = "breakdown";
			Event e;
			e.type = Event::Type::LockOff;
			e.beat = beat;
			e.detail = "breakdown -> lock fades out until the drop";
			out.push_back(e);
		} else if (cfg_.breakdown_index >= 0 && cfg_.breakdown_index < cfg_.palette_count) {
			bd_active_ = true;
			auto evs = go_live(cfg_.breakdown_index, "breakdown", beat);
			out.insert(out.end(), evs.begin(), evs.end());
		}
	} else if (sec == Section::Drop) {
		if (bd_active_) {
			bd_active_ = false;
			auto evs = go_live(pick_next(1), "drop after breakdown", beat);
			out.insert(out.end(), evs.begin(), evs.end());
		} else if (cfg_.trigger == ChangeTrigger::Drop) {
			auto evs = go_live(pick_next(1), "drop", beat);
			out.insert(out.end(), evs.begin(), evs.end());
		}
		if (cfg_.off_on_drop) {
			off_until_ = static_cast<int64_t>(beat) + cfg_.drop_raw_bars * 4;
			off_reason_ = "drop";
			Event e;
			e.type = Event::Type::LockOff;
			e.beat = beat;
			e.detail = "drop! raw visuals for " + std::to_string(cfg_.drop_raw_bars) + " bars";
			out.push_back(e);
		} else if (off_reason_ == "breakdown") {
			off_until_ = -1;
			off_reason_.clear();
			Event e;
			e.type = Event::Type::LockOn;
			e.beat = beat;
			e.detail = "breakdown over -> lock back in";
			out.push_back(e);
		}
	}
	return out;
}

std::vector<Event> Scheduler::on_phrase(uint64_t phrase, uint64_t beat)
{
	if (!cfg_.enabled || paused_)
		return {};
	std::vector<Event> out;

	if (cfg_.rotation && !forced_on_) {
		int n = (live_ >= 0 && live_ < static_cast<int>(cfg_.colour_counts.size()))
				? cfg_.colour_counts[static_cast<std::size_t>(live_)]
				: 1;
		if (n < 1)
			n = 1;
		rot_ = (rot_ + 1) % n;
		Event e;
		e.type = Event::Type::Command;
		e.beat = beat;
		e.detail = "rotate colours -> offset " + std::to_string(rot_);
		out.push_back(e);
	}

	if (!bd_active_) {
		if (cfg_.trigger == ChangeTrigger::Phrases &&
		    (phrase % static_cast<uint64_t>(cfg_.every_phrases)) == 0) {
			auto evs =
				go_live(pick_next(1), "every " + std::to_string(cfg_.every_phrases) + " phrases", beat);
			out.insert(out.end(), evs.begin(), evs.end());
		}
		if (cfg_.trigger == ChangeTrigger::Random) {
			const double r = rng_.next();
			if (r < cfg_.chance) {
				auto evs = go_live(pick_next(1), "random", beat);
				out.insert(out.end(), evs.begin(), evs.end());
			}
		}
	}

	if (off_until_ > static_cast<int64_t>(beat))
		return out;
	if (cooldown_ > 0) {
		cooldown_--;
		return out;
	}
	if (cfg_.random_off) {
		const double r = rng_.next();
		if (r < cfg_.random_chance) {
			const int lo = std::min(cfg_.random_bars_min, cfg_.random_bars_max);
			const int hi = std::max(cfg_.random_bars_min, cfg_.random_bars_max);
			const int bars = lo + static_cast<int>(rng_.next() * static_cast<double>(hi - lo + 1));
			off_until_ = static_cast<int64_t>(beat) + bars * 4;
			off_reason_ = "random off";
			cooldown_ = cfg_.cooldown_phrases;
			Event e;
			e.type = Event::Type::LockOff;
			e.beat = beat;
			e.detail = "random off for " + std::to_string(bars) + " bars, cooldown " +
				   std::to_string(cfg_.cooldown_phrases);
			out.push_back(e);
		}
	}
	return out;
}

std::vector<Event> Scheduler::on_beat(uint64_t beat)
{
	if (off_until_ != -1 && off_until_ != INT64_MAX && off_until_ <= static_cast<int64_t>(beat)) {
		off_until_ = -1;
		const std::string r = off_reason_;
		off_reason_.clear();
		Event e;
		e.type = Event::Type::LockOn;
		e.beat = beat;
		e.detail = r + " over -> lock back in";
		return {e};
	}
	return {};
}

std::vector<Event> Scheduler::command(Command cmd, int palette_index, uint64_t beat)
{
	std::vector<Event> out;
	auto log = [&](const std::string &s) {
		Event e;
		e.type = Event::Type::Command;
		e.beat = beat;
		e.detail = s;
		out.push_back(e);
	};

	switch (cmd) {
	case Command::ForceOn:
		forced_on_ = !forced_on_;
		if (forced_on_) {
			off_until_ = -1;
			off_reason_.clear();
		}
		log(forced_on_ ? "force on: director can't turn it off" : "force on released");
		break;
	case Command::ForceOff:
		force_off_ = true;
		log("force off (hold)");
		break;
	case Command::ReleaseForce:
		force_off_ = false;
		log("force off released");
		break;
	case Command::Pause:
		paused_ = true;
		log("director paused (lock and palette held)");
		break;
	case Command::Resume:
		paused_ = false;
		log("director resumed");
		break;
	case Command::Next: {
		auto evs = go_live(pick_next(1), "manual next", beat);
		out.insert(out.end(), evs.begin(), evs.end());
		break;
	}
	case Command::Prev: {
		auto evs = go_live(pick_next(-1), "manual prev", beat);
		out.insert(out.end(), evs.begin(), evs.end());
		break;
	}
	case Command::GoLive: {
		if (palette_index >= 0 && palette_index < cfg_.palette_count) {
			auto evs = go_live(palette_index, "manual", beat);
			out.insert(out.end(), evs.begin(), evs.end());
		}
		break;
	}
	case Command::RotateNow: {
		int n = (live_ >= 0 && live_ < static_cast<int>(cfg_.colour_counts.size()))
				? cfg_.colour_counts[static_cast<std::size_t>(live_)]
				: 1;
		if (n < 1)
			n = 1;
		rot_ = (rot_ + 1) % n;
		log("rotate colours now -> offset " + std::to_string(rot_));
		break;
	}
	case Command::Panic:
		paused_ = true;
		force_off_ = false;
		off_until_ = -1;
		off_reason_.clear();
		rot_ = 0;
		pfade_ = 1.0f;
		bd_active_ = false;
		log("PANIC: lock on, director paused, palette held");
		break;
	}
	return out;
}

void Scheduler::advance_pfade(double dt, double bpm)
{
	if (pfade_ < 1.0f) {
		const double beats = (bpm / 60.0) * dt / std::max(0.01, static_cast<double>(cfg_.crossfade_beats));
		pfade_ = std::min(1.0f, pfade_ + static_cast<float>(beats));
	}
}

double Scheduler::mix_target(uint64_t beat) const
{
	double t = 1.0;
	if (cfg_.enabled && !paused_ && off_until_ > static_cast<int64_t>(beat))
		t = 0.0;
	if (forced_on_)
		t = 1.0;
	if (force_off_)
		t = 0.0;
	return t;
}

} // namespace pd
