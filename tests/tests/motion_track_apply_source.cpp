#include <main.h>

#include "../../src/motion_track/apply_source.h"
#include "../../src/perspective_ass_state.h"

#include <ass_file.h>
#include <ass_style.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace aegisub::motion_track;

class motion_track_apply_source : public testing::Test {
	protected:
	AssFile file;
	MotionTrackApplySource source;
	ApplyPlanInput input;
	AssDialogue *untouched = nullptr;
	std::vector<AssDialogue *> selection;
	int commits = 0;

	void SetUp() override {
		file.Styles.push_back(*new AssStyle);
		auto *first = AddLine(R"({\pos(0,0)\frz15\fscx120\fscy80}first)");
		untouched = AddLine("unselected");
		auto *second = AddLine(R"({\pos(20,30)\frz15\fscx120\fscy80}second)");
		file.Commit("setup", AssFile::COMMIT_DIAG_ADDREM);
		selection = {first, second};
		input.model = TrackModel::Similarity;
		input.decode_interval = {.first = 0, .last = 19};
		input.direction_domain = {.first = 0, .last = 19};
		input.storage_width = input.script_width = 1920;
		input.storage_height = input.script_height = 1080;
		input.timecodes = agi::vfr::Framerate(10.0);
		input.video_frame_count = 20;
		input.options.compact_epsilon = 100.0;
		for (int frame = 0; frame < 20; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.status = TrackStatus::Ok;
			sample.center_x = 2.0 * frame;
			double const angle = 0.5 * frame * std::numbers::pi / 180.0;
			double const scale = 1.0 + 0.01 * frame;
			sample.transform.matrix[0] = scale * std::cos(angle);
			sample.transform.matrix[1] = -scale * std::sin(angle);
			sample.transform.matrix[3] = scale * std::sin(angle);
			sample.transform.matrix[4] = scale * std::cos(angle);
			input.samples.push_back(sample);
		}
		source.Capture(file, selection, input.timecodes, input.video_frame_count);
	}

	AssDialogue *AddLine(std::string text) {
		auto *line = new AssDialogue;
		line->Start = 0;
		line->End = 1950;
		line->Text = std::move(text);
		file.Events.push_back(*line);
		return line;
	}

	bool Apply(MotionTrackApplyPlan const& plan, agi::vfr::Framerate const& timecodes,
			   std::function<void(AssFile&)> const& prepare = {}) {
		std::string message;
		auto prepared = source.Prepare(file, plan, timecodes, message);
		if (!prepared)
			return false;
		int const type = prepared->Candidate().PrepareCommit(
			AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT);
		if (prepare)
			prepare(prepared->Candidate());
		prepared->Finalize(timecodes);
		prepared->Install();
		for (auto *line : selection)
			EXPECT_FALSE(line->Text.get().empty());
		selection = prepared->Outputs();
		int revision = -1;
		file.PublishCommit({.message = "apply", .commit_id = &revision, .single_line = nullptr, .type = type, .changed_lines = {}}, false);
		++commits;
		return true;
	}

	MotionTrackApplyPlan Plan(ApplyMode mode) {
		input.options.mode = mode;
		return BuildApplyPlan(file, source.Targets(), input);
	}

	[[nodiscard]] std::vector<std::string> Entries() const {
		std::vector<std::string> entries;
		for (auto const& line : file.Events)
			entries.push_back(line.GetEntryData());
		return entries;
	}
	[[nodiscard]] std::vector<std::uintptr_t> EventIdentities() const {
		std::vector<std::uintptr_t> identities;
		for (auto const& line : file.Events)
			identities.push_back(reinterpret_cast<std::uintptr_t>(&line));
		return identities;
	}

	void ExpectSourceSnapshot(MotionTrackSourceSnapshot const& expected) const {
		auto const& actual = source.Snapshot();
		EXPECT_EQ(expected.file_identity, actual.file_identity);
		EXPECT_EQ(expected.script_info_entries, actual.script_info_entries);
		EXPECT_EQ(expected.video_frame_count, actual.video_frame_count);
		ASSERT_EQ(expected.timecodes.IsLoaded(), actual.timecodes.IsLoaded());
		for (int frame = 0; frame <= expected.video_frame_count; ++frame)
			EXPECT_EQ(expected.timecodes.TimeAtFrame(frame), actual.timecodes.TimeAtFrame(frame)) << frame;
		ASSERT_EQ(expected.lines.size(), actual.lines.size());
		auto fields = [](MotionTrackLineFingerprint const& line) {
			return std::tie(line.identity, line.id, line.row, line.comment, line.layer, line.margins,
							line.start_ms, line.end_ms, line.style, line.actor, line.effect,
							line.extradata_ids, line.text, line.style_entry);
		};
		for (std::size_t index = 0; index < expected.lines.size(); ++index)
			EXPECT_EQ(fields(expected.lines[index]), fields(actual.lines[index])) << index;
	}
};
} // namespace

TEST_F(motion_track_apply_source, exact_preparation_exception_preserves_original_source_and_allows_retry) {
	auto const plan = Plan(ApplyMode::Exact);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	ASSERT_EQ(2u, plan.lines.size());
	ASSERT_EQ(40u, plan.event_count);
	auto const entries = Entries();
	auto const identities = EventIdentities();
	auto const selected = selection;
	auto const snapshot = source.Snapshot();
	auto const baseline_targets = source.Targets();
	int attempts = 0;
	auto reject_preparation = [&](AssFile& candidate) {
		++attempts;
		EXPECT_NE(&file, &candidate);
		EXPECT_EQ(41u, candidate.Events.size());
		throw std::runtime_error("candidate preparation failed");
	};

	EXPECT_THROW(Apply(plan, input.timecodes, reject_preparation), std::runtime_error);
	EXPECT_EQ(1, attempts);
	EXPECT_EQ(0, commits);
	EXPECT_EQ(entries, Entries());
	ExpectSourceSnapshot(snapshot);
	ASSERT_EQ(identities, EventIdentities());
	EXPECT_EQ(selected, selection);
	EXPECT_EQ(baseline_targets, source.Targets());
	EXPECT_EQ(1, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, snapshot, input.timecodes));
	EXPECT_TRUE(source.ContinueTargetsMatch(selection));

	ASSERT_TRUE(Apply(plan, input.timecodes));
	EXPECT_EQ(1, commits);
	ASSERT_EQ(40u, selection.size());
	EXPECT_EQ(41u, Entries().size());
	EXPECT_EQ(20, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	std::size_t selected_index = 0;
	for (auto const& line : plan.lines)
		for (auto const& part : line.parts) {
			auto const *output = selection[selected_index++];
			EXPECT_EQ(part.text, output->Text.get());
			EXPECT_EQ(part.start_ms, output->Start);
			EXPECT_EQ(part.end_ms, output->End);
		}
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));
}

TEST_F(motion_track_apply_source, reapply_preparation_exception_preserves_last_apply_and_allows_retry) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Exact), input.timecodes));
	ASSERT_EQ(40u, selection.size());
	auto const plan = Plan(ApplyMode::Compact);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	ASSERT_EQ(2u, plan.lines.size());
	ASSERT_EQ(2u, plan.event_count);
	auto const entries = Entries();
	auto const identities = EventIdentities();
	auto const selected = selection;
	auto const snapshot = source.Snapshot();
	auto const baseline_targets = source.Targets();
	int attempts = 0;
	auto reject_preparation = [&](AssFile& candidate) {
		++attempts;
		EXPECT_NE(&file, &candidate);
		EXPECT_EQ(3u, candidate.Events.size());
		throw std::runtime_error("candidate reapply preparation failed");
	};

	EXPECT_THROW(Apply(plan, input.timecodes, reject_preparation), std::runtime_error);
	EXPECT_EQ(1, attempts);
	EXPECT_EQ(1, commits);
	EXPECT_EQ(entries, Entries());
	ExpectSourceSnapshot(snapshot);
	ASSERT_EQ(identities, EventIdentities());
	EXPECT_EQ(selected, selection);
	EXPECT_EQ(baseline_targets, source.Targets());
	EXPECT_EQ(20, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, snapshot, input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));

	ASSERT_TRUE(Apply(plan, input.timecodes));
	EXPECT_EQ(2, commits);
	ASSERT_EQ(2u, selection.size());
	EXPECT_EQ(3u, Entries().size());
	EXPECT_EQ(1, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	for (std::size_t index = 0; index < plan.lines.size(); ++index) {
		ASSERT_EQ(1u, plan.lines[index].parts.size());
		auto const& part = plan.lines[index].parts.front();
		EXPECT_EQ(part.text, selection[index]->Text.get());
		EXPECT_EQ(part.start_ms, selection[index]->Start);
		EXPECT_EQ(part.end_ms, selection[index]->End);
	}
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));
}

TEST_F(motion_track_apply_source, compact_exact_round_trip_reuses_baseline_and_preserves_other_lines) {
	auto compact = Plan(ApplyMode::Compact);
	ASSERT_TRUE(compact.has_mutations()) << compact.message;
	ASSERT_EQ(2u, compact.event_count);
	ASSERT_TRUE(Apply(compact, input.timecodes));
	auto const compact_entries = Entries();
	ASSERT_EQ(3u, compact_entries.size());
	EXPECT_EQ(1, untouched->Row);
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
	EXPECT_FALSE(source.ContinueTargetsMatch(selection));

	auto exact = Plan(ApplyMode::Exact);
	ASSERT_TRUE(exact.has_mutations()) << exact.message;
	ASSERT_EQ(40u, exact.event_count);
	ASSERT_TRUE(Apply(exact, input.timecodes));
	ASSERT_EQ(40u, selection.size());
	EXPECT_EQ(41u, Entries().size());
	EXPECT_EQ(20, untouched->Row);
	EXPECT_EQ("unselected", untouched->Text.get());
	EXPECT_EQ(0, selection.front()->Row);
	EXPECT_EQ(40, selection.back()->Row);
	for (int frame : {0, 9, 19}) {
		auto *line = selection[frame];
		auto const state = perspective::EvaluateEffectiveAssState({.file = &file,
																   .line = line,
																   .play_resolution = {.width = 1920.0, .height = 1080.0},
																   .capture_time_ms = input.timecodes.TimeAtFrame(frame)});
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		EXPECT_NEAR(2.0 * frame, state.value.transform.position.x, 1e-6);
		EXPECT_NEAR(15.0 - 0.5 * frame, state.value.transform.rotation_z, 1e-6);
		EXPECT_NEAR(120.0 * (1.0 + 0.01 * frame), state.value.transform.scale_x, 1e-6);
		EXPECT_NEAR(80.0 * (1.0 + 0.01 * frame), state.value.transform.scale_y, 1e-6);
	}
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	EXPECT_EQ(compact_entries, Entries());
	EXPECT_EQ(2u, selection.size());
	EXPECT_EQ(3, commits);
	EXPECT_TRUE(MotionTrackSourceIsCurrent(file, source.Snapshot(), input.timecodes));
}

TEST_F(motion_track_apply_source, hand_edit_after_plan_preparation_rejects_reapply_without_mutation) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	selection.front()->Text = "edited after preview";
	auto const edited_entries = Entries();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	EXPECT_EQ(edited_entries, Entries());
	EXPECT_EQ(1, commits);
}

TEST_F(motion_track_apply_source, undo_replacement_rejects_reapply_without_mutation) {
	AssFile original(file);
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	file = original;
	selection.clear();
	auto const restored_entries = Entries();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	EXPECT_EQ(restored_entries, Entries());
	EXPECT_EQ(1, commits);
}

TEST_F(motion_track_apply_source, cleared_source_rejects_pending_plan_without_accessing_old_baseline) {
	auto plan = Plan(ApplyMode::Exact);
	ASSERT_TRUE(plan.has_mutations());
	auto const entries = Entries();
	source.Clear();
	EXPECT_FALSE(Apply(plan, input.timecodes));
	EXPECT_EQ(entries, Entries());
	EXPECT_EQ(0, commits);
}

TEST_F(motion_track_apply_source, changed_style_or_timecodes_rejects_reapply) {
	ASSERT_TRUE(Apply(Plan(ApplyMode::Compact), input.timecodes));
	auto exact = Plan(ApplyMode::Exact);
	auto const entries = Entries();
	auto *style = file.GetStyle("Default");
	ASSERT_NE(nullptr, style);
	double const previous_scale = style->scalex;
	style->scalex = previous_scale + 10;
	style->UpdateData();
	EXPECT_FALSE(Apply(exact, input.timecodes));
	style->scalex = previous_scale;
	style->UpdateData();
	EXPECT_FALSE(Apply(exact, agi::vfr::Framerate(20.0)));
	EXPECT_EQ(entries, Entries());
	EXPECT_EQ(1, commits);
}
