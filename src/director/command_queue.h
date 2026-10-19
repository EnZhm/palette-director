#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace pd {

enum class Command {
	ForceOn,  // toggle: lock held on, director can't turn it off
	ForceOff, // hold: force the lock off
	ReleaseForce,
	Pause,
	Resume,
	Next,
	Prev,
	GoLive, // palette_index selects the target palette
	RotateNow,
	Panic // lock on + director paused + palette held + rotation reset
};

struct CommandMsg {
	Command cmd = Command::ForceOn;
	int palette_index = -1;
};

// Lock-free single-producer/single-consumer queue (dock/UI thread -> Director thread).
class CommandQueue {
public:
	explicit CommandQueue(std::size_t capacity = 1024)
		: capacity_(capacity + 1),
		  buf_(capacity + 1),
		  head_(0),
		  tail_(0)
	{
	}

	bool push(const CommandMsg &msg)
	{
		const std::size_t h = head_.load(std::memory_order_relaxed);
		const std::size_t t = tail_.load(std::memory_order_acquire);
		const std::size_t next = (t + 1) % capacity_;
		if (next == h)
			return false; // full
		buf_[t] = msg;
		tail_.store(next, std::memory_order_release);
		return true;
	}

	bool pop(CommandMsg &msg)
	{
		const std::size_t h = head_.load(std::memory_order_relaxed);
		const std::size_t t = tail_.load(std::memory_order_acquire);
		if (h == t)
			return false; // empty
		msg = buf_[h];
		head_.store((h + 1) % capacity_, std::memory_order_release);
		return true;
	}

	bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }

private:
	std::size_t capacity_;
	std::vector<CommandMsg> buf_;
	std::atomic<std::size_t> head_;
	std::atomic<std::size_t> tail_;
};

} // namespace pd
