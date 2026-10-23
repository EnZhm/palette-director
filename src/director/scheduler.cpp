#include "scheduler.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

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
	shuffle_cycle_.clear();
	cooldown_ = 0;
	off_until_ = -1;
	off_reason_.clear();
	paused_ = false;
	panic_ = false;
	force_off_ = false;
	forced_on_ = false;
	cycle_active_ = false;
	cycle_seq_.clear();
}

void Scheduler::reconfigure(const SchedulerConfig &cfg, int live)
{
	const bool structure = cfg.palette_count != cfg_.palette_count || cfg.names != cfg_.names ||
			       cfg.breakdown_index != cfg_.breakdown_index || cfg.order != cfg_.order;
	const bool reseed = cfg.seed != cfg_.seed;
	cfg_ = cfg;
	if (reseed)
		rng_.reseed(cfg.seed);
	if (structure || reseed) {
		order_.clear();
		shuffle_cycle_.clear();
	}
	if (structure || !cfg_.drop_cycle || !cfg_.enabled) {
		// palette indices may have moved (or the behaviour was switched off): stop any cycle
		cycle_active_ = false;
		cycle_seq_.clear();
	}
	if (structure) {
		// indices may have moved: drop the outgoing palette instead of fading from a stale one
		prev_ = -1;
		pfade_ = 1.0f;
		if (last_normal_ >= cfg_.palette_count || last_normal_ == cfg_.breakdown_index)
			last_normal_ = -1;
	}
	if (cfg_.breakdown_index < 0)
		bd_active_ = false;
	if (live >= 0 && live < cfg_.palette_count && live != live_) {
		live_ = live;
		prev_ = -1;
		pfade_ = 1.0f;
		rot_ = 0;
	}
	if (live_ >= cfg_.palette_count || live_ < 0) {
		live_ = cfg_.palette_count > 0 ? cfg_.palette_count - 1 : 0;
		prev_ = -1;
		pfade_ = 1.0f;
		rot_ = 0;
	}
	if (prev_ >= cfg_.palette_count)
		prev_ = -1;
	if (live_ < static_cast<int>(cfg_.colour_counts.size()) && cfg_.colour_counts[live_] > 0)
		rot_ %= cfg_.colour_counts[static_cast<std::size_t>(live_)];
	if (!cfg_.off_on_breakdown && off_reason_ == "breakdown") {
		off_until_ = -1;
		off_reason_.clear();
	}
}

int Scheduler::peek_next() const
{
	if (cfg_.trigger == ChangeTrigger::Manual)
		return -1;
	const std::vector<int> pool = normal_idx();
	if (pool.empty())
		return -1;
	if (cfg_.order == ChangeOrder::Shuffle)
		return order_.empty() ? -1 : order_[0];
	auto it = std::find(pool.begin(), pool.end(), live_);
	int base;
	if (it != pool.end())
		base = live_;
	else if (last_normal_ >= 0 && std::find(pool.begin(), pool.end(), last_normal_) != pool.end())
		base = last_normal_;
	else
		base = pool[0];
	const auto bit = std::find(pool.begin(), pool.end(), base);
	const int idx = static_cast<int>(std::distance(pool.begin(), bit));
	const int n = static_cast<int>(pool.size());
	return pool[static_cast<std::size_t>((idx + 1) % n)];
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
			if (shuffle_cycle_.empty()) {
				// Build the canonical seeded permutation over the FULL normal pool
				// (spec 4.2: no repeats until every palette has played). The breakdown
				// palette is already excluded by normal_idx().
				shuffle_cycle_ = pool;
				for (std::size_t i = shuffle_cycle_.size(); i-- > 1;) {
					const std::size_t j =
						static_cast<std::size_t>(rng_.next() * static_cast<double>(i + 1));
					std::swap(shuffle_cycle_[i], shuffle_cycle_[j]);
				}
				// Never open a pass on the palette already live: rotate it to the back
				// so the first change is real and the pass still covers every palette
				// exactly once.
				if (!shuffle_cycle_.empty() && shuffle_cycle_.front() == live_)
					std::rotate(shuffle_cycle_.begin(), shuffle_cycle_.begin() + 1,
						    shuffle_cycle_.end());
			}
			// Replay the same permutation each full pass, so no palette repeats until
			// every normal palette has played (acceptance: shuffle plays each palette
			// once before any repeat).
			order_ = shuffle_cycle_;
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
	// Consume the shuffle pass even when the pick equals the live palette, so a no-op
	// change can never stall order_ on the same front element (spec 4.2 shuffle rule).
	if (!order_.empty() && order_.front() == i)
		order_.erase(order_.begin());

	if (i == live_)
		return {};

	prev_ = live_;
	live_ = i;
	pfade_ = (cfg_.crossfade_beats <= 0.0f) ? 1.0f : 0.0f;
	rot_ = 0;
	if (i != cfg_.breakdown_index)
		last_normal_ = i;

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
		char beats[32];
		std::snprintf(beats, sizeof(beats), "%g", static_cast<double>(cfg_.crossfade_beats));
		e.detail += std::string(", ") + beats + "-beat crossfade";
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
	if (cycle_active_)
		cancel_drop_cycle("section change", beat, out);

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
		bool cycling = false;
		if (bd_active_) {
			bd_active_ = false;
			const int target = pick_next(1);
			/* v1.0.1 drop colour cycle: roll the seeded RNG ONLY when enabled, so a set without it
			 * replays exactly the v1.0.0 decisions */
			if (cfg_.drop_cycle) {
				const double r = rng_.next();
				const int steps = std::max(1, cfg_.drop_cycle_bars) * 4 *
						  std::max(1, cfg_.drop_cycle_steps_per_beat);
				char why[96];
				std::snprintf(why, sizeof(why), "drop cycle (%.2f %s %.2f): %d bar%s at 1/%d", r,
					      r < cfg_.drop_cycle_chance ? "<" : ">=",
					      static_cast<double>(cfg_.drop_cycle_chance), cfg_.drop_cycle_bars,
					      cfg_.drop_cycle_bars > 1 ? "s" : "", 4 * cfg_.drop_cycle_steps_per_beat);
				Event e;
				e.type = Event::Type::Command;
				e.beat = beat;
				e.detail = r < cfg_.drop_cycle_chance
						   ? std::string(why) + ", " + std::to_string(steps) + " cuts"
						   : std::string(why) + ": no cycle";
				out.push_back(e);
				if (r < cfg_.drop_cycle_chance && normal_idx().size() >= 2) {
					start_drop_cycle(beat, target, out);
					cycling = true;
				}
			}
			if (!cycling) {
				auto evs = go_live(target, "drop after breakdown", beat);
				out.insert(out.end(), evs.begin(), evs.end());
			}
		} else if (cfg_.trigger == ChangeTrigger::Drop) {
			auto evs = go_live(pick_next(1), "drop", beat);
			out.insert(out.end(), evs.begin(), evs.end());
		}
		if (cfg_.off_on_drop && cycling) {
			Event e;
			e.type = Event::Type::Command;
			e.beat = beat;
			e.detail = "drop cycle replaces the raw bars for this drop (lock held on)";
			out.push_back(e);
			if (off_reason_ == "breakdown") {
				off_until_ = -1;
				off_reason_.clear();
			}
		} else if (cfg_.off_on_drop) {
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

	if (cfg_.rotation && !forced_on_ && !cycle_active_) {
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

	if (!bd_active_ && !cycle_active_) {
		if (cfg_.trigger == ChangeTrigger::Phrases &&
		    (phrase % static_cast<uint64_t>(cfg_.every_phrases)) == 0) {
			auto evs =
				go_live(pick_next(1), "every " + std::to_string(cfg_.every_phrases) + " phrases", beat);
			out.insert(out.end(), evs.begin(), evs.end());
		}
		if (cfg_.trigger == ChangeTrigger::Random) {
			const double r = rng_.next();
			if (r < cfg_.chance) {
				char why[48];
				std::snprintf(why, sizeof(why), "random %.2f < %.2f", r,
					      static_cast<double>(cfg_.chance));
				auto evs = go_live(pick_next(1), why, beat);
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
			char why[64];
			std::snprintf(why, sizeof(why), "random off (%.2f < %.2f) for %d bar%s", r,
				      static_cast<double>(cfg_.random_chance), bars, bars > 1 ? "s" : "");
			e.detail = std::string(why) + ", cooldown " + std::to_string(cfg_.cooldown_phrases);
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

	if (cycle_active_) {
		switch (cmd) {
		case Command::ForceOff:
			cancel_drop_cycle("force off", beat, out);
			break;
		case Command::Pause:
			cancel_drop_cycle("pause", beat, out);
			break;
		case Command::Panic:
			cancel_drop_cycle("PANIC", beat, out);
			break;
		case Command::Next:
		case Command::Prev:
		case Command::GoLive:
			cancel_drop_cycle("manual pick wins", beat, out);
			break;
		default:
			break;
		}
	}

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
		if (panic_) {
			release_panic();
			log("PANIC released: director resumed");
		} else {
			paused_ = false;
			log("director resumed");
		}
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
		/* v1.0.1: PANIC is a toggle. The second press releases it (resume, clear force on/off). */
		if (panic_) {
			release_panic();
			log("PANIC released: director resumed");
			break;
		}
		panic_ = true;
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

void Scheduler::start_drop_cycle(uint64_t beat, int target, std::vector<Event> &out)
{
	const std::vector<int> pool = normal_idx();
	const int spb = std::max(1, cfg_.drop_cycle_steps_per_beat);
	cycle_steps_ = std::max(1, cfg_.drop_cycle_bars) * 4 * spb;
	cycle_start_ = static_cast<double>(beat);
	cycle_target_ = target;
	cycle_seq_.clear();
	/* seeded shuffles of the normal pool (breakdown palette excluded), no palette twice in a row,
	 * never the live palette first, and the last cut is not the landing palette (so the landing is a
	 * real change with the normal crossfade) */
	int last = live_;
	while (static_cast<int>(cycle_seq_.size()) < cycle_steps_) {
		std::vector<int> perm = pool;
		for (std::size_t i = perm.size(); i-- > 1;) {
			const std::size_t j = static_cast<std::size_t>(rng_.next() * static_cast<double>(i + 1));
			std::swap(perm[i], perm[j]);
		}
		if (perm.front() == last)
			std::rotate(perm.begin(), perm.begin() + 1, perm.end());
		for (int p : perm) {
			if (static_cast<int>(cycle_seq_.size()) >= cycle_steps_)
				break;
			cycle_seq_.push_back(p);
			last = p;
		}
	}
	/* the last cut must not be the landing palette (the landing is then a real change) */
	if (cycle_seq_.back() == target) {
		const int before = cycle_seq_.size() >= 2 ? cycle_seq_[cycle_seq_.size() - 2] : live_;
		for (int p : pool) {
			if (p != target && p != before) {
				cycle_seq_.back() = p;
				break;
			}
		}
	}
	cycle_active_ = true;
	cycle_next_ = 0;
	off_until_ = -1; // the lock is held on for the cycle
	cycle_cut(cycle_seq_[0], 0, beat, out);
	cycle_next_ = 1;
}

void Scheduler::cycle_cut(int i, int step, uint64_t beat, std::vector<Event> &out)
{
	if (i == live_)
		return;
	prev_ = live_;
	live_ = i;
	pfade_ = 1.0f; // hard cut, whatever crossfade_beats says
	rot_ = 0;
	Event e;
	e.type = Event::Type::PaletteChange;
	e.beat = beat;
	e.palette = i;
	e.prev_palette = prev_;
	const std::string nm = (i >= 0 && i < static_cast<int>(cfg_.names.size()))
				       ? cfg_.names[static_cast<std::size_t>(i)]
				       : std::to_string(i);
	e.detail = "palette -> " + nm + " (drop cycle " + std::to_string(step + 1) + "/" +
		   std::to_string(cycle_steps_) + ", cut)";
	out.push_back(e);
}

void Scheduler::cancel_drop_cycle(const char *why, uint64_t beat, std::vector<Event> &out)
{
	cycle_active_ = false;
	cycle_seq_.clear();
	Event e;
	e.type = Event::Type::Command;
	e.beat = beat;
	e.detail = std::string("drop cycle cancelled (") + why + ")";
	out.push_back(e);
}

std::vector<Event> Scheduler::on_tick(double beats)
{
	if (!cycle_active_)
		return {};
	std::vector<Event> out;
	const int spb = std::max(1, cfg_.drop_cycle_steps_per_beat);
	const double rel = beats - cycle_start_;
	const uint64_t b = static_cast<uint64_t>(std::max(0.0, std::floor(beats)));
	if (rel >= static_cast<double>(cycle_steps_) / spb) {
		cycle_active_ = false;
		cycle_seq_.clear();
		const int t = (cycle_target_ >= 0 && cycle_target_ < cfg_.palette_count) ? cycle_target_ : pick_next(1);
		Event e;
		e.type = Event::Type::Command;
		e.beat = b;
		const std::string nm = (t >= 0 && t < static_cast<int>(cfg_.names.size()))
					       ? cfg_.names[static_cast<std::size_t>(t)]
					       : std::to_string(t);
		e.detail = "drop cycle over -> " + nm;
		out.push_back(e);
		auto evs = go_live(t, "drop after breakdown", b);
		out.insert(out.end(), evs.begin(), evs.end());
		return out;
	}
	const int due = std::min(cycle_steps_ - 1, static_cast<int>(std::floor(rel * spb)));
	while (cycle_next_ <= due) {
		cycle_cut(cycle_seq_[static_cast<std::size_t>(cycle_next_)], cycle_next_, b, out);
		cycle_next_++;
	}
	return out;
}

void Scheduler::release_panic()
{
	panic_ = false;
	paused_ = false;
	forced_on_ = false;
	force_off_ = false;
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
	if (panic_)
		return 1.0; // PANIC holds the lock on, whatever force on/off say
	if (cycle_active_)
		return 1.0; // the drop cycle holds the lock on (force off / pause / panic cancel it)
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
