#include <main.h>

#include "../../src/ui_deadline_timer.h"

#include <libaegisub/dispatch.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

class UiDeadlineTimerTestPeer {
	public:
	static std::unique_ptr<UiDeadlineTimer> CreatePortable(std::function<void()> callback) {
		return std::unique_ptr<UiDeadlineTimer>(new UiDeadlineTimer(std::move(callback), false));
	}

	static bool UsesNativeBackend(UiDeadlineTimer const& timer) {
		return timer.UsesNativeBackend();
	}
};

namespace {
class UiDeadlineTimerTest : public ::testing::TestWithParam<bool> {
	protected:
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<agi::dispatch::Thunk> main_queue;
	std::thread::id const main_thread_id = std::this_thread::get_id();
	UiDeadlineTimer::TimePoint last_enqueued_at{};

	void SetUp() override {
		agi::dispatch::Init([this](agi::dispatch::Thunk thunk) {
			{
				std::scoped_lock lock(mutex);
				last_enqueued_at = UiDeadlineTimer::Clock::now();
				main_queue.emplace_back(std::move(thunk));
			}
			cv.notify_one(); }, [this] { return std::this_thread::get_id() == main_thread_id; });
	}

	void TearDown() override {
		agi::dispatch::Init([](agi::dispatch::Thunk const&) {}, [] { return false; });
	}

	std::unique_ptr<UiDeadlineTimer> CreateTimer(std::function<void()> callback) {
		if (GetParam())
			return std::make_unique<UiDeadlineTimer>(std::move(callback));
		auto timer = UiDeadlineTimerTestPeer::CreatePortable(std::move(callback));
		EXPECT_FALSE(UiDeadlineTimerTestPeer::UsesNativeBackend(*timer));
		return timer;
	}

	bool WaitForMainTasks(std::size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return main_queue.size() >= count; });
	}

	bool PumpOneMainTask() {
		agi::dispatch::Thunk thunk;
		{
			std::scoped_lock lock(mutex);
			if (main_queue.empty())
				return false;
			thunk = std::move(main_queue.front());
			main_queue.pop_front();
		}
		thunk();
		return true;
	}

	UiDeadlineTimer::TimePoint LastEnqueuedAt() {
		std::scoped_lock lock(mutex);
		return last_enqueued_at;
	}
};
}

TEST_P(UiDeadlineTimerTest, queued_one_shot_stays_running_until_consumed_on_main_thread) {
	int calls = 0;
	std::thread::id callback_thread;
	auto timer = CreateTimer([&] {
		++calls;
		callback_thread = std::this_thread::get_id();
	});
	EXPECT_FALSE(timer->IsRunning());
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(1));
	EXPECT_EQ(0, calls);
	EXPECT_TRUE(timer->IsRunning());

	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(1, calls);
	EXPECT_EQ(main_thread_id, callback_thread);
	EXPECT_FALSE(timer->IsRunning());
	EXPECT_FALSE(PumpOneMainTask());
}

TEST_P(UiDeadlineTimerTest, future_deadline_is_not_posted_early) {
	int calls = 0;
	auto timer = CreateTimer([&] { ++calls; });
	auto const deadline = UiDeadlineTimer::Clock::now() + std::chrono::milliseconds(20);
	timer->StartAt(deadline);
	ASSERT_TRUE(WaitForMainTasks(1));
	EXPECT_GE(LastEnqueuedAt(), deadline);
	EXPECT_EQ(0, calls);
	EXPECT_TRUE(timer->IsRunning());
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(1, calls);
	EXPECT_FALSE(timer->IsRunning());
}

TEST_P(UiDeadlineTimerTest, stop_rejects_already_queued_callback) {
	int calls = 0;
	auto timer = CreateTimer([&] { ++calls; });
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(1));
	timer->Stop();
	EXPECT_FALSE(timer->IsRunning());
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(0, calls);
	EXPECT_FALSE(timer->IsRunning());
}

TEST_P(UiDeadlineTimerTest, restart_rejects_old_queue_entry_without_consuming_new_deadline) {
	int calls = 0;
	auto timer = CreateTimer([&] { ++calls; });
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(1));
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(2));

	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(0, calls);
	EXPECT_TRUE(timer->IsRunning());
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(1, calls);
	EXPECT_FALSE(timer->IsRunning());
}

TEST_P(UiDeadlineTimerTest, destruction_rejects_already_queued_callback) {
	int calls = 0;
	auto timer = CreateTimer([&] { ++calls; });
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(1));
	timer.reset();
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(0, calls);
}

TEST_P(UiDeadlineTimerTest, callback_can_restart_timer) {
	int calls = 0;
	std::unique_ptr<UiDeadlineTimer> timer;
	timer = CreateTimer([&] {
		EXPECT_FALSE(timer->IsRunning());
		if (++calls == 1)
			timer->StartAt(UiDeadlineTimer::Clock::now());
	});
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(1));
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(1, calls);
	EXPECT_TRUE(timer->IsRunning());
	ASSERT_TRUE(WaitForMainTasks(1));
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(2, calls);
	EXPECT_FALSE(timer->IsRunning());
}

TEST_P(UiDeadlineTimerTest, callback_can_destroy_timer) {
	int calls = 0;
	std::unique_ptr<UiDeadlineTimer> timer;
	timer = CreateTimer([&] {
		timer.reset();
		++calls;
	});
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(1));
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(1, calls);
	EXPECT_EQ(nullptr, timer);
}

TEST_P(UiDeadlineTimerTest, far_future_deadline_can_be_stopped_and_replaced) {
	int calls = 0;
	auto timer = CreateTimer([&] { ++calls; });
	timer->StartAt(UiDeadlineTimer::Clock::now() + std::chrono::hours(1));
	EXPECT_TRUE(timer->IsRunning());
	timer->Stop();
	EXPECT_FALSE(timer->IsRunning());
	timer->StartAt(UiDeadlineTimer::Clock::now());
	ASSERT_TRUE(WaitForMainTasks(1));
	ASSERT_TRUE(PumpOneMainTask());
	EXPECT_EQ(1, calls);
	EXPECT_FALSE(timer->IsRunning());
}

TEST_P(UiDeadlineTimerTest, destruction_does_not_wait_for_far_future_deadline) {
	int calls = 0;
	auto timer = CreateTimer([&] { ++calls; });
	timer->StartAt(UiDeadlineTimer::Clock::now() + std::chrono::hours(1));
	auto const before = UiDeadlineTimer::Clock::now();
	timer.reset();
	EXPECT_LT(UiDeadlineTimer::Clock::now() - before, std::chrono::seconds(2));
	EXPECT_EQ(0, calls);
	EXPECT_FALSE(PumpOneMainTask());
}

INSTANTIATE_TEST_SUITE_P(Backends, UiDeadlineTimerTest, ::testing::Bool(),
						 [](::testing::TestParamInfo<bool> const& info) { return info.param ? "Automatic" : "Portable"; });
