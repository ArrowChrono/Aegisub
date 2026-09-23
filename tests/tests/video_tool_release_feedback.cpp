#include <main.h>

#include "../../src/video_tool_release_feedback.h"

namespace {
using Feedback = VideoToolReleaseFeedback;
using namespace std::chrono_literals;
constexpr auto t0 = Feedback::TimePoint{};
}

TEST(video_tool_release_feedback, no_final_keeps_inactive_and_clears_existing_window) {
	Feedback feedback;
	feedback.RequestFeedback();
	EXPECT_FALSE(feedback.Deadline());
	EXPECT_FALSE(feedback.ShouldDefer(t0));
	EXPECT_FALSE(feedback.OnPresented(41));
	EXPECT_FALSE(feedback.Expire(t0 + 1s));
	EXPECT_FALSE(feedback.Begin(0, t0));

	ASSERT_TRUE(feedback.Begin(41, t0));
	EXPECT_FALSE(feedback.Begin(0, t0 + 1ms));
	feedback.RequestFeedback();
	EXPECT_FALSE(feedback.Deadline());
	EXPECT_FALSE(feedback.ShouldDefer(t0 + 2ms));
	EXPECT_FALSE(feedback.OnPresented(41));
	EXPECT_FALSE(feedback.Expire(t0 + 1s));
}

TEST(video_tool_release_feedback, early_expiry_preserves_window_until_exact_sixteen_ms_boundary) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	ASSERT_TRUE(feedback.Deadline());
	EXPECT_EQ(t0 + 16ms, *feedback.Deadline());
	EXPECT_TRUE(feedback.ShouldDefer(t0));
	EXPECT_TRUE(feedback.ShouldDefer(t0 + 16ms - 1ns));
	EXPECT_FALSE(feedback.Expire(t0 + 16ms - 1ns));
	ASSERT_TRUE(feedback.Deadline());
	EXPECT_EQ(t0 + 16ms, *feedback.Deadline());
	EXPECT_FALSE(feedback.ShouldDefer(t0 + 16ms));
	EXPECT_FALSE(feedback.ShouldDefer(t0 + 16ms + 1ns));

	auto const expired = feedback.Expire(t0 + 16ms);
	ASSERT_TRUE(expired.has_value());
	EXPECT_TRUE(*expired);
	EXPECT_FALSE(feedback.Deadline());
	EXPECT_FALSE(feedback.Expire(t0 + 16ms));
}

TEST(video_tool_release_feedback, no_successful_present_leaves_one_late_fallback) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	// A failed swap must not call OnPresented, so it cannot satisfy feedback.
	auto const expired = feedback.Expire(t0 + 1s);
	ASSERT_TRUE(expired.has_value());
	EXPECT_TRUE(*expired);
	EXPECT_FALSE(feedback.ShouldDefer(t0 + 1s));
	EXPECT_FALSE(feedback.Deadline());
	feedback.RequestFeedback();
	EXPECT_FALSE(feedback.Expire(t0 + 2s));
}

TEST(video_tool_release_feedback, ordinary_present_satisfies_feedback_without_closing_window) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	EXPECT_FALSE(feedback.OnPresented(0));
	ASSERT_TRUE(feedback.Deadline());
	EXPECT_EQ(t0 + 16ms, *feedback.Deadline());
	EXPECT_TRUE(feedback.ShouldDefer(t0 + 15ms));

	auto const expired = feedback.Expire(t0 + 16ms);
	ASSERT_TRUE(expired.has_value());
	EXPECT_FALSE(*expired);
	EXPECT_FALSE(feedback.Deadline());
	EXPECT_FALSE(feedback.Expire(t0 + 17ms));
}

TEST(video_tool_release_feedback, repeated_hover_rearms_feedback_without_extending_deadline) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	EXPECT_FALSE(feedback.OnPresented(0));
	feedback.RequestFeedback();
	feedback.RequestFeedback();
	ASSERT_TRUE(feedback.Deadline());
	EXPECT_EQ(t0 + 16ms, *feedback.Deadline());
	EXPECT_FALSE(feedback.ShouldDefer(t0 + 16ms));

	auto const expired = feedback.Expire(t0 + 16ms);
	ASSERT_TRUE(expired.has_value());
	EXPECT_TRUE(*expired);
}

TEST(video_tool_release_feedback, matching_final_closes_window_and_prevents_fallback) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	EXPECT_TRUE(feedback.OnPresented(41));
	EXPECT_FALSE(feedback.Deadline());
	EXPECT_FALSE(feedback.ShouldDefer(t0 + 1ms));
	feedback.RequestFeedback();
	EXPECT_FALSE(feedback.Expire(t0 + 16ms));
	EXPECT_FALSE(feedback.OnPresented(41));
}

TEST(video_tool_release_feedback, unrelated_final_satisfies_feedback_but_keeps_window_open) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	EXPECT_FALSE(feedback.OnPresented(42));
	ASSERT_TRUE(feedback.Deadline());
	EXPECT_EQ(t0 + 16ms, *feedback.Deadline());
	EXPECT_TRUE(feedback.ShouldDefer(t0 + 15ms));

	auto const expired = feedback.Expire(t0 + 16ms);
	ASSERT_TRUE(expired.has_value());
	EXPECT_FALSE(*expired);
}

TEST(video_tool_release_feedback, new_window_replaces_deadline_and_resets_feedback_need) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	EXPECT_FALSE(feedback.OnPresented(0));
	ASSERT_TRUE(feedback.Begin(42, t0 + 4ms));
	ASSERT_TRUE(feedback.Deadline());
	EXPECT_EQ(t0 + 20ms, *feedback.Deadline());
	EXPECT_TRUE(feedback.ShouldDefer(t0 + 16ms));
	EXPECT_FALSE(feedback.Expire(t0 + 16ms));

	auto const expired = feedback.Expire(t0 + 20ms);
	ASSERT_TRUE(expired.has_value());
	EXPECT_TRUE(*expired);
}

TEST(video_tool_release_feedback, replacing_window_rejects_old_final_and_accepts_current_final) {
	Feedback feedback;
	auto const current_id = (std::uint64_t{1} << 32) | 41;
	ASSERT_TRUE(feedback.Begin(41, t0));
	ASSERT_TRUE(feedback.Begin(current_id, t0 + 4ms));
	EXPECT_FALSE(feedback.OnPresented(41));
	EXPECT_TRUE(feedback.ShouldDefer(t0 + 16ms));
	EXPECT_TRUE(feedback.OnPresented(current_id));
	EXPECT_FALSE(feedback.Deadline());
	EXPECT_FALSE(feedback.Expire(t0 + 20ms));
}

TEST(video_tool_release_feedback, cancel_disarms_feedback_and_allows_new_window) {
	Feedback feedback;
	ASSERT_TRUE(feedback.Begin(41, t0));
	feedback.Cancel();
	feedback.RequestFeedback();
	EXPECT_FALSE(feedback.Deadline());
	EXPECT_FALSE(feedback.ShouldDefer(t0 + 1ms));
	EXPECT_FALSE(feedback.OnPresented(41));
	EXPECT_FALSE(feedback.Expire(t0 + 16ms));

	ASSERT_TRUE(feedback.Begin(42, t0 + 20ms));
	ASSERT_TRUE(feedback.Deadline());
	EXPECT_EQ(t0 + 36ms, *feedback.Deadline());
	EXPECT_TRUE(feedback.OnPresented(42));
}
