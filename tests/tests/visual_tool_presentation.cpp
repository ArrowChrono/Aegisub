#include <main.h>

#include "../../src/visual_tool_presentation.h"

#include <memory>
#include <utility>

namespace {
class CpuSnapshot final : public VisualToolRenderSnapshot {
	public:
	explicit CpuSnapshot(std::shared_ptr<const VisualToolRenderContext> context)
		: VisualToolRenderSnapshot(std::move(context)) {}

	void Draw(VideoOverlayDrawContext&) const override {}
};

std::shared_ptr<const VisualToolRenderSnapshot> Snapshot(VisualToolPresentation const& state) {
	return std::make_shared<const CpuSnapshot>(state.Context());
}
}

TEST(visual_tool_presentation, inactive_selection_never_retains_a_matching_packet) {
	VisualToolPresentation state;
	VisualToolPresentation other;
	auto const packet = Snapshot(state);

	ASSERT_TRUE(state.Context());
	EXPECT_NE(state.Context(), other.Context());
	EXPECT_FALSE(state.IsActive());
	EXPECT_EQ(0u, state.InteractionId());
	EXPECT_TRUE(state.Matches(packet));
	EXPECT_FALSE(state.Matches({}));
	EXPECT_FALSE(state.Matches(Snapshot(other)));
	EXPECT_EQ(nullptr, state.Select(packet));
	EXPECT_EQ(nullptr, state.Select({}));
	EXPECT_FALSE(state.OnFinalPresented(0));
}

TEST(visual_tool_presentation, begin_requires_an_id_and_a_matching_nonnull_baseline) {
	VisualToolPresentation state;
	VisualToolPresentation other;
	auto const context = state.Context();
	auto const missing_context = std::make_shared<const CpuSnapshot>(nullptr);

	EXPECT_FALSE(state.Begin(0, Snapshot(state)));
	EXPECT_FALSE(state.Begin(12, {}));
	EXPECT_FALSE(state.Begin(12, Snapshot(other)));
	EXPECT_FALSE(state.Begin(12, missing_context));
	EXPECT_FALSE(state.IsActive());
	EXPECT_EQ(0u, state.InteractionId());
	EXPECT_EQ(context, state.Context());
	EXPECT_EQ(nullptr, state.Select({}));
}

TEST(visual_tool_presentation, ready_packet_requires_active_interaction_and_snapshot_context) {
	VisualToolPresentation state;
	VisualToolPresentation other;
	auto const packet = Snapshot(state);
	EXPECT_FALSE(state.MatchesActiveInteraction(0, packet));
	EXPECT_FALSE(state.MatchesActiveInteraction(12, packet));
	ASSERT_TRUE(state.Begin(12, packet));
	EXPECT_TRUE(state.MatchesActiveInteraction(12, packet));
	EXPECT_FALSE(state.MatchesActiveInteraction(0, packet));
	EXPECT_FALSE(state.MatchesActiveInteraction(11, packet));
	EXPECT_FALSE(state.MatchesActiveInteraction(13, packet));
	EXPECT_FALSE(state.MatchesActiveInteraction(12, {}));
	EXPECT_FALSE(state.MatchesActiveInteraction(12, Snapshot(other)));
	EXPECT_EQ(packet, state.Select({}));
	EXPECT_EQ(12u, state.InteractionId());

	ASSERT_TRUE(state.Begin(13, packet));
	EXPECT_FALSE(state.MatchesActiveInteraction(12, packet));
	EXPECT_TRUE(state.MatchesActiveInteraction(13, packet));
	state.Reset();
	ASSERT_TRUE(state.Begin(14, Snapshot(state)));
	EXPECT_FALSE(state.MatchesActiveInteraction(14, packet));
	EXPECT_TRUE(state.MatchesActiveInteraction(14, Snapshot(state)));
}

TEST(visual_tool_presentation, invalid_begin_preserves_the_existing_interaction) {
	VisualToolPresentation state;
	VisualToolPresentation other;
	auto const baseline = Snapshot(state);
	auto const context = state.Context();
	ASSERT_TRUE(state.Begin(12, baseline));

	EXPECT_FALSE(state.Begin(0, Snapshot(state)));
	EXPECT_FALSE(state.Begin(13, {}));
	EXPECT_FALSE(state.Begin(13, Snapshot(other)));
	EXPECT_TRUE(state.IsActive());
	EXPECT_EQ(12u, state.InteractionId());
	EXPECT_EQ(context, state.Context());
	EXPECT_EQ(baseline, state.Select({}));
}

TEST(visual_tool_presentation, selects_packet_snapshot_or_the_retained_baseline_without_live_fallback) {
	VisualToolPresentation state;
	VisualToolPresentation other;
	auto const baseline = Snapshot(state);
	auto const packet = Snapshot(state);
	auto const unrelated = Snapshot(other);
	ASSERT_TRUE(state.Begin(12, baseline));

	EXPECT_EQ(packet, state.Select(packet));
	EXPECT_EQ(baseline, state.Select({}));
	EXPECT_EQ(baseline, state.Select(unrelated));
	EXPECT_TRUE(state.IsActive());
	EXPECT_EQ(12u, state.InteractionId());
}

TEST(visual_tool_presentation, redrag_keeps_displayed_snapshots_and_ignores_the_prior_final) {
	VisualToolPresentation state;
	auto const baseline = Snapshot(state);
	auto const prior_drag_packet = Snapshot(state);
	auto const new_drag_packet = Snapshot(state);
	ASSERT_TRUE(state.Begin(12, baseline));
	ASSERT_EQ(prior_drag_packet, state.Select(prior_drag_packet));

	ASSERT_TRUE(state.Begin(13, prior_drag_packet));
	EXPECT_EQ(prior_drag_packet, state.Select({}));
	EXPECT_EQ(prior_drag_packet, state.Select(prior_drag_packet));
	EXPECT_FALSE(state.OnFinalPresented(12));
	EXPECT_FALSE(state.OnFinalPresented(0));
	EXPECT_TRUE(state.IsActive());
	EXPECT_EQ(13u, state.InteractionId());
	EXPECT_EQ(new_drag_packet, state.Select(new_drag_packet));
	EXPECT_EQ(prior_drag_packet, state.Select({}));
	EXPECT_TRUE(state.OnFinalPresented(13));
	EXPECT_EQ(nullptr, state.Select(prior_drag_packet));
}

TEST(visual_tool_presentation, only_the_exact_presented_final_releases_the_baseline) {
	VisualToolPresentation state;
	auto baseline = Snapshot(state);
	std::weak_ptr<const VisualToolRenderSnapshot> retained = baseline;
	auto const context = state.Context();
	ASSERT_TRUE(state.Begin(12, baseline));
	baseline.reset();
	ASSERT_FALSE(retained.expired());

	EXPECT_FALSE(state.OnFinalPresented(11));
	EXPECT_FALSE(state.OnFinalPresented(13));
	EXPECT_FALSE(retained.expired());
	EXPECT_EQ(context, state.Context());
	EXPECT_TRUE(state.OnFinalPresented(12));
	EXPECT_TRUE(retained.expired());
	EXPECT_FALSE(state.IsActive());
	EXPECT_EQ(0u, state.InteractionId());
	EXPECT_NE(context, state.Context());
	EXPECT_EQ(nullptr, state.Select(Snapshot(state)));
	EXPECT_FALSE(state.OnFinalPresented(12));
}

TEST(visual_tool_presentation, completed_final_cannot_override_next_press_baseline) {
	VisualToolPresentation state;
	auto const released_packet = Snapshot(state);
	ASSERT_TRUE(state.Begin(12, Snapshot(state)));
	ASSERT_TRUE(state.OnFinalPresented(12));

	auto const pressed_baseline = Snapshot(state);
	ASSERT_TRUE(state.Begin(13, pressed_baseline));
	EXPECT_FALSE(state.Matches(released_packet));
	EXPECT_EQ(pressed_baseline, state.Select(released_packet));
	EXPECT_EQ(pressed_baseline, state.Select({}));
	EXPECT_FALSE(state.OnFinalPresented(12));
	EXPECT_EQ(pressed_baseline, state.Select(released_packet));
	EXPECT_EQ(13u, state.InteractionId());
}

TEST(visual_tool_presentation, reset_releases_owned_snapshots_and_creates_a_fresh_context) {
	VisualToolPresentation state;
	auto baseline = Snapshot(state);
	std::weak_ptr<const VisualToolRenderContext> old_context = state.Context();
	std::weak_ptr<const VisualToolRenderSnapshot> old_snapshot = baseline;
	ASSERT_TRUE(state.Begin(12, baseline));
	baseline.reset();
	ASSERT_FALSE(old_context.expired());
	ASSERT_FALSE(old_snapshot.expired());

	state.Reset();
	EXPECT_TRUE(old_snapshot.expired());
	EXPECT_TRUE(old_context.expired());
	ASSERT_TRUE(state.Context());
	EXPECT_FALSE(state.IsActive());
	EXPECT_EQ(0u, state.InteractionId());
	EXPECT_EQ(nullptr, state.Select({}));
	EXPECT_FALSE(state.OnFinalPresented(12));
	EXPECT_TRUE(state.Begin(13, Snapshot(state)));
}

TEST(visual_tool_presentation, reset_rejects_late_snapshots_still_owned_by_packets) {
	VisualToolPresentation state;
	auto old_packet = Snapshot(state);
	std::weak_ptr<const VisualToolRenderContext> old_context = state.Context();
	ASSERT_TRUE(state.Begin(12, old_packet));

	state.Reset();
	EXPECT_FALSE(old_context.expired());
	EXPECT_FALSE(state.Matches(old_packet));
	EXPECT_FALSE(state.Begin(13, old_packet));
	auto const baseline = Snapshot(state);
	ASSERT_TRUE(state.Begin(13, baseline));
	EXPECT_EQ(baseline, state.Select(old_packet));
	EXPECT_FALSE(state.OnFinalPresented(12));
	EXPECT_EQ(13u, state.InteractionId());
	old_packet.reset();
	EXPECT_TRUE(old_context.expired());
}
