#include "ui_deadline_timer.h"

#include <libaegisub/dispatch.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

struct UiDeadlineTimer::State final : std::enable_shared_from_this<State> {
	mutable std::mutex mutex;
	std::condition_variable cv;
	std::function<void()> callback;
	std::jthread worker;
	TimePoint deadline{};
	std::uint64_t generation = 0;
	bool armed = false;
	bool queued = false;
	bool shutting_down = false;
	bool uses_native = false;
#ifdef _WIN32
	HANDLE native_timer = nullptr;
	HANDLE control_event = nullptr;
#endif

	State(std::function<void()> callback, bool allow_native)
		: callback(std::move(callback)) {
#ifdef _WIN32
		if (allow_native) {
			control_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (control_event) {
				native_timer = CreateWaitableTimerExW(nullptr, nullptr,
													  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
				if (!native_timer)
					native_timer = CreateWaitableTimerExW(nullptr, nullptr, 0,
														  TIMER_MODIFY_STATE | SYNCHRONIZE);
				uses_native = native_timer != nullptr;
			}
		}
#else
		(void)allow_native;
#endif
	}

	~State() {
#ifdef _WIN32
		// Shutdown joins the worker before any handle it waits on is closed.
		if (native_timer)
			CloseHandle(native_timer);
		if (control_event)
			CloseHandle(control_event);
#endif
	}

	void WakeWorker() {
		cv.notify_one();
#ifdef _WIN32
		if (control_event)
			SetEvent(control_event);
#endif
	}

	void WorkerLoop() {
		std::unique_lock<std::mutex> lock(mutex);
		while (true) {
			cv.wait(lock, [&] { return shutting_down || (armed && !queued); });
			if (shutting_down)
				return;

			auto const expected_generation = generation;
			auto const expected_deadline = deadline;
			auto const invalidated = [&] {
				return shutting_down || !armed || generation != expected_generation;
			};
			while (!invalidated()) {
				auto const now = Clock::now();
				if (now >= expected_deadline)
					break;
#ifdef _WIN32
				if (uses_native) {
					using TimerTicks = std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
					LARGE_INTEGER due_time;
					due_time.QuadPart = -std::chrono::ceil<TimerTicks>(expected_deadline - now).count();
					if (SetWaitableTimer(native_timer, &due_time, 0, nullptr, nullptr, FALSE)) {
						HANDLE handles[] = {control_event, native_timer};
						lock.unlock();
						auto const result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
						lock.lock();
						if (result != WAIT_OBJECT_0 + 1)
							CancelWaitableTimer(native_timer);
						if (result != WAIT_OBJECT_0 && result != WAIT_OBJECT_0 + 1)
							uses_native = false;
						continue;
					}
					// A runtime arm/wait failure must not strand this deadline.
					uses_native = false;
				}
#endif
				cv.wait_until(lock, expected_deadline, invalidated);
			}
			if (invalidated())
				continue;

			queued = true;
			auto weak_state = weak_from_this();
			lock.unlock();
			agi::dispatch::Main().Async([weak_state, expected_generation] {
				if (auto state = weak_state.lock())
					state->RunIfCurrent(expected_generation);
			});
			lock.lock();
		}
	}

	void RunIfCurrent(std::uint64_t expected_generation) {
		std::function<void()> current_callback;
		{
			std::scoped_lock lock(mutex);
			if (shutting_down || generation != expected_generation || !armed || !queued)
				return;
			current_callback = callback;
			armed = false;
			queued = false;
		}
		// Consume before invoking: the callback may restart or destroy its timer.
		if (current_callback)
			current_callback();
	}

	void Shutdown() {
		{
			std::scoped_lock lock(mutex);
			++generation;
			armed = false;
			queued = false;
			shutting_down = true;
		}
		WakeWorker();
		if (worker.joinable())
			worker.join();
	}
};

UiDeadlineTimer::UiDeadlineTimer(std::function<void()> callback)
	: UiDeadlineTimer(std::move(callback), true) {
}

UiDeadlineTimer::UiDeadlineTimer(std::function<void()> callback, bool allow_native)
	: state(std::make_shared<State>(std::move(callback), allow_native)) {
	state->worker = std::jthread([worker_state = state.get()] { worker_state->WorkerLoop(); });
}

UiDeadlineTimer::~UiDeadlineTimer() {
	state->Shutdown();
}

void UiDeadlineTimer::StartAt(TimePoint deadline) {
	{
		std::scoped_lock lock(state->mutex);
		++state->generation;
		state->deadline = deadline;
		state->armed = true;
		state->queued = false;
	}
	state->WakeWorker();
}

void UiDeadlineTimer::Stop() {
	{
		std::scoped_lock lock(state->mutex);
		++state->generation;
		state->armed = false;
		state->queued = false;
	}
	state->WakeWorker();
}

bool UiDeadlineTimer::IsRunning() const {
	std::scoped_lock lock(state->mutex);
	return state->armed;
}

bool UiDeadlineTimer::UsesNativeBackend() const {
	std::scoped_lock lock(state->mutex);
	return state->uses_native;
}
