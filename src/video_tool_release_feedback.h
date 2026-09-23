#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

class VideoToolReleaseFeedback final {
	public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;
	static constexpr std::chrono::milliseconds MergeInterval{16};

	bool Begin(std::uint64_t interaction_id, TimePoint now) noexcept {
		if (!interaction_id) {
			Cancel();
			return false;
		}
		window = Window{.interaction_id = interaction_id, .deadline = now + MergeInterval, .needs_feedback = true};
		return true;
	}

	void RequestFeedback() noexcept {
		if (window)
			window->needs_feedback = true;
	}

	[[nodiscard]] bool ShouldDefer(TimePoint now) const noexcept {
		return window && now < window->deadline;
	}

	[[nodiscard]] std::optional<TimePoint> Deadline() const noexcept {
		return window ? std::optional<TimePoint>{window->deadline} : std::nullopt;
	}

	// Call only after a successful presentation: an unrelated frame can satisfy
	// current feedback, but only this interaction's Final closes the window.
	bool OnPresented(std::uint64_t final_interaction_id) noexcept {
		if (!window)
			return false;
		if (final_interaction_id && final_interaction_id == window->interaction_id) {
			Cancel();
			return true;
		}
		window->needs_feedback = false;
		return false;
	}

	// A value means the window expired; true requests one fallback redraw.
	std::optional<bool> Expire(TimePoint now) noexcept {
		if (!window || now < window->deadline)
			return std::nullopt;
		bool const needs_feedback = window->needs_feedback;
		Cancel();
		return needs_feedback;
	}

	void Cancel() noexcept { window.reset(); }

	private:
	struct Window {
		std::uint64_t interaction_id;
		TimePoint deadline;
		bool needs_feedback;
	};
	std::optional<Window> window;
};
