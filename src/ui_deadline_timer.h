#pragma once

#include <chrono>
#include <functional>
#include <memory>

class UiDeadlineTimerTestPeer;

/// One-shot timer for short UI-interaction deadlines, without changing the
/// process-wide timer resolution. The worker only posts to the main dispatch
/// queue; the callback and all public operations belong to the owning UI thread.
class UiDeadlineTimer {
	public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	explicit UiDeadlineTimer(std::function<void()> callback);
	~UiDeadlineTimer();

	UiDeadlineTimer(UiDeadlineTimer const&) = delete;
	UiDeadlineTimer& operator=(UiDeadlineTimer const&) = delete;
	UiDeadlineTimer(UiDeadlineTimer&&) = delete;
	UiDeadlineTimer& operator=(UiDeadlineTimer&&) = delete;

	/// Replaces any pending deadline, including an already queued callback.
	void StartAt(TimePoint deadline);
	void Stop();
	/// Remains true while the current callback is queued but not yet consumed.
	[[nodiscard]] bool IsRunning() const;

	private:
	struct State;
	std::shared_ptr<State> state;

	friend class UiDeadlineTimerTestPeer;
	UiDeadlineTimer(std::function<void()> callback, bool allow_native);
	[[nodiscard]] bool UsesNativeBackend() const;
};
