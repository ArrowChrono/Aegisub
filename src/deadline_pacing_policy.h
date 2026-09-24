#pragma once

#include <chrono>
#include <optional>

class DeadlinePacingPolicy final {
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	explicit DeadlinePacingPolicy(std::chrono::milliseconds interval)
	: interval(interval) {
	}

	void Begin(TimePoint now) noexcept {
		active = true;
		pending = false;
		deadline = now + interval;
	}

	bool Request(TimePoint now, bool has_work = true) noexcept {
		if (!has_work) {
			pending = false;
			return false;
		}
		if (!active)
			Begin(now);

		pending = true;
		if (now < deadline)
			return false;

		MarkEmitted(now);
		return true;
	}

	bool OnTimer(TimePoint now, bool has_work = true) noexcept {
		if (!has_work) {
			pending = false;
			return false;
		}
		if (!active || !pending || now < deadline)
			return false;

		MarkEmitted(now);
		return true;
	}

	bool Force(TimePoint now) noexcept {
		if (!active)
			return false;
		MarkEmitted(now);
		return true;
	}

	void End() noexcept {
		active = false;
		pending = false;
		deadline = {};
	}

	bool IsActive() const noexcept { return active; }
	bool HasPending() const noexcept { return active && pending; }

	std::optional<TimePoint> NextDeadline() const noexcept {
		return HasPending() ? std::optional<TimePoint>{deadline} : std::nullopt;
	}

private:
	std::chrono::milliseconds interval;
	TimePoint deadline{};
	bool active = false;
	bool pending = false;

	void MarkEmitted(TimePoint now) noexcept {
		pending = false;
		deadline = now + interval;
	}
};
