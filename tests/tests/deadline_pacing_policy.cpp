#include <main.h>

#include "../../src/deadline_pacing_policy.h"

TEST(deadline_pacing_policy, request_at_deadline_emits_without_waiting_for_timer) {
	using Policy = DeadlinePacingPolicy;
	auto const t0 = Policy::TimePoint{};
	Policy policy(std::chrono::milliseconds(16));

	policy.Begin(t0);
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(1)));
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(15)));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + std::chrono::milliseconds(16), *policy.NextDeadline());

	EXPECT_TRUE(policy.Request(t0 + std::chrono::milliseconds(16)));
	EXPECT_FALSE(policy.HasPending());
}

TEST(deadline_pacing_policy, timer_is_trailing_and_early_or_stale_events_do_not_emit) {
	using Policy = DeadlinePacingPolicy;
	auto const t0 = Policy::TimePoint{};
	Policy policy(std::chrono::milliseconds(16));

	policy.Begin(t0);
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(1)));
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(15)));
	EXPECT_TRUE(policy.OnTimer(t0 + std::chrono::milliseconds(16)));

	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(17)));
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(18)));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + std::chrono::milliseconds(32), *policy.NextDeadline());
	EXPECT_TRUE(policy.OnTimer(t0 + std::chrono::milliseconds(32)));
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(33)));
}

TEST(deadline_pacing_policy, force_clears_pending_and_end_disarms_policy) {
	using Policy = DeadlinePacingPolicy;
	auto const t0 = Policy::TimePoint{};
	Policy policy(std::chrono::milliseconds(33));

	policy.Begin(t0);
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(2)));
	EXPECT_TRUE(policy.Force(t0 + std::chrono::milliseconds(3)));
	EXPECT_FALSE(policy.HasPending());

	policy.End();
	EXPECT_FALSE(policy.IsActive());
	EXPECT_FALSE(policy.NextDeadline());
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::seconds(1)));
}

TEST(deadline_pacing_policy, no_work_neither_activates_nor_leaves_pending_demand) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	DeadlinePacingPolicy policy(17ms);
	EXPECT_FALSE(policy.Request(t0, false));
	EXPECT_FALSE(policy.OnTimer(t0 + 1s, false));
	EXPECT_FALSE(policy.IsActive());
	EXPECT_FALSE(policy.HasPending());
	EXPECT_FALSE(policy.NextDeadline());

	EXPECT_FALSE(policy.Request(t0 + 1s));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + 1017ms, *policy.NextDeadline());
}

TEST(deadline_pacing_policy, ready_emission_clears_timer_and_rebases_later_feedback) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	DeadlinePacingPolicy policy(17ms);
	EXPECT_FALSE(policy.Force(t0));
	policy.Begin(t0);
	ASSERT_FALSE(policy.Request(t0 + 1ms));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + 17ms, *policy.NextDeadline());

	EXPECT_TRUE(policy.Force(t0 + 3ms));
	EXPECT_FALSE(policy.HasPending());
	EXPECT_FALSE(policy.NextDeadline());
	EXPECT_FALSE(policy.OnTimer(t0 + 17ms));
	EXPECT_FALSE(policy.Request(t0 + 18ms));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + 20ms, *policy.NextDeadline());
	EXPECT_FALSE(policy.OnTimer(t0 + 20ms - 1ns));
	EXPECT_TRUE(policy.OnTimer(t0 + 20ms));
	EXPECT_FALSE(policy.OnTimer(t0 + 20ms + 1ns));
	policy.End();
	EXPECT_FALSE(policy.Force(t0 + 21ms));
	EXPECT_FALSE(policy.HasPending());
}

TEST(deadline_pacing_policy, canceled_demand_preserves_deadline_on_both_entry_points) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	for (bool timer : {false, true}) {
		for (auto cancel_at : {16ms, 17ms, 18ms}) {
			SCOPED_TRACE(timer);
			SCOPED_TRACE(cancel_at.count());
			DeadlinePacingPolicy policy(17ms);
			policy.Begin(t0);
			ASSERT_FALSE(policy.Request(t0 + 1ms));
			EXPECT_FALSE(timer ? policy.OnTimer(t0 + cancel_at, false) : policy.Request(t0 + cancel_at, false));
			EXPECT_TRUE(policy.IsActive());
			EXPECT_FALSE(policy.HasPending());
			EXPECT_FALSE(policy.NextDeadline());
			EXPECT_FALSE(policy.OnTimer(t0 + 20ms));
			EXPECT_TRUE(policy.Request(t0 + 21ms));
			EXPECT_FALSE(policy.Request(t0 + 37ms));
			ASSERT_TRUE(policy.NextDeadline());
			EXPECT_EQ(t0 + 38ms, *policy.NextDeadline());
			EXPECT_TRUE(policy.OnTimer(t0 + 38ms));
		}
	}
}

TEST(deadline_pacing_policy, consumed_packet_timer_does_not_spend_another_display_period) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	DeadlinePacingPolicy policy(17ms);
	policy.Begin(t0);
	ASSERT_FALSE(policy.Request(t0 + 5ms));
	EXPECT_FALSE(policy.OnTimer(t0 + 17ms, false));
	EXPECT_FALSE(policy.HasPending());
	EXPECT_FALSE(policy.NextDeadline());
	EXPECT_TRUE(policy.Request(t0 + 17201us));
}

TEST(deadline_pacing_policy, cancel_before_deadline_does_not_allow_early_or_starved_output) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	DeadlinePacingPolicy policy(17ms);
	policy.Begin(t0);
	ASSERT_FALSE(policy.Request(t0 + 1ms));
	EXPECT_FALSE(policy.Request(t0 + 2ms, false));
	EXPECT_FALSE(policy.Request(t0 + 3ms));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + 17ms, *policy.NextDeadline());
	EXPECT_FALSE(policy.OnTimer(t0 + 17ms - 1ns));
	EXPECT_TRUE(policy.OnTimer(t0 + 17ms));
	EXPECT_FALSE(policy.OnTimer(t0 + 17ms + 1ns));
}
