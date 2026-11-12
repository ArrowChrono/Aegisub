#include "apply_source.h"

#include "../ass_attachment.h"
#include "../ass_file.h"
#include "../ass_info.h"
#include "../ass_style.h"
#include "../subtitle_grid_folding.h"

#include <algorithm>
#include <utility>
#include <map>

namespace aegisub::motion_track {

void MotionTrackApplySource::Capture(AssFile const& file,
									 std::vector<AssDialogue *> const& targets,
									 agi::vfr::Framerate const& timecodes, int video_frame_count) {
	Clear();
	*snapshot_ = CaptureMotionTrackSource(file, targets, timecodes, video_frame_count);
	for (auto *line : targets) {
		originals_.push_back(std::make_unique<AssDialogue>(*line));
		current_lines_.push_back({line});
	}
}

void MotionTrackApplySource::Clear() {
	*snapshot_ = {};
	originals_.clear();
	current_lines_.clear();
	applied_ = false;
}

std::vector<AssDialogue *> MotionTrackApplySource::Targets() const {
	std::vector<AssDialogue *> targets;
	targets.reserve(originals_.size());
	for (auto const& line : originals_)
		targets.push_back(line.get());
	return targets;
}

bool MotionTrackApplySource::ContinueTargetsMatch(
	std::vector<AssDialogue *> const& targets) const {
	// An explicit Analyze after Apply starts a new subtitle baseline. Reusing
	// the old origin with already transformed text would apply motion twice.
	return !applied_ && MotionTrackContinueTargetsMatch(*snapshot_, targets);
}

struct MotionTrackApplySource::PreparedApply::State {
	MotionTrackApplySource *source = nullptr;
	AssFile *live = nullptr;
	AssFile candidate;
	std::map<AssDialogue const *, AssDialogue *> live_lines;
	std::vector<AssDialogue *> outputs;
	std::vector<AssDialogue *> order;
	std::vector<AssDialogue *> removed;
	std::vector<std::vector<AssDialogue *>> groups;
	std::unique_ptr<MotionTrackSourceSnapshot> snapshot;
	bool finalized = false;
	bool installed = false;
};

MotionTrackApplySource::PreparedApply::PreparedApply() : state(std::make_unique<State>()) {}
MotionTrackApplySource::PreparedApply::~PreparedApply() {
	if (state->installed)
		for (auto *line : state->removed)
			delete line;
}
AssFile& MotionTrackApplySource::PreparedApply::Candidate() { return state->candidate; }
std::vector<AssDialogue *> const& MotionTrackApplySource::PreparedApply::Outputs() const { return state->outputs; }
AssDialogue *MotionTrackApplySource::PreparedApply::LiveLine(AssDialogue const *line) const {
	if (auto const found = state->live_lines.find(line); found != state->live_lines.end())
		return found->second;
	throw std::logic_error("A prepared Motion Track line has no live identity");
}

std::unique_ptr<MotionTrackApplySource::PreparedApply> MotionTrackApplySource::Prepare(
	AssFile& file, MotionTrackApplyPlan const& plan, agi::vfr::Framerate const& timecodes, std::string& message) {
	if (!MotionTrackSourceIsCurrent(file, *snapshot_, timecodes)) {
		message = "The subtitles or timecodes changed; re-run Analyze first.";
		return {};
	}
	if (!plan.has_mutations()) {
		message = plan.message;
		return {};
	}
	auto prepared = std::make_unique<PreparedApply>();
	auto& state = *prepared->state;
	state.source = this;
	state.live = &file;
	state.candidate.Info = file.Info;
	state.candidate.Attachments = file.Attachments;
	state.candidate.Properties = file.Properties;
	state.candidate.transient_fonts = file.transient_fonts;
	for (auto const& style : file.Styles)
		state.candidate.Styles.push_back(*new AssStyle(style));
	state.candidate.Extradata = file.Extradata;
	state.candidate.next_extradata_id = file.next_extradata_id;
	state.candidate.InheritFoldingView(file);
	std::map<AssDialogue *, AssDialogue *> candidates;
	for (auto& line : file.Events) {
		auto candidate = std::make_unique<AssDialogue>(static_cast<AssDialogueBase const&>(line));
		candidates.emplace(&line, candidate.get());
		state.live_lines.emplace(candidate.get(), &line);
		state.candidate.Events.push_back(*candidate.release());
	}
	state.groups.resize(current_lines_.size());
	for (std::size_t i = 0; i < current_lines_.size(); ++i)
		for (auto *line : current_lines_[i])
			state.groups[i].push_back(candidates.at(line));
	std::vector<bool> touched(originals_.size());
	for (auto const& line : plan.lines) {
		auto const found = std::ranges::find_if(originals_, [&](auto const& candidate) { return candidate.get() == line.source; });
		if (found == originals_.end() || line.parts.empty()) {
			message = "The apply plan does not match the captured subtitle baseline.";
			return {};
		}
		auto const index = static_cast<std::size_t>(found - originals_.begin());
		if (touched[index]) {
			message = "The apply plan contains a duplicate source line.";
			return {};
		}
		touched[index] = true;
		auto& previous = state.groups[index];
		auto const anchor = state.candidate.Events.iterator_to(*previous.front());
		std::vector<AssDialogue *> next;
		for (auto const& part : line.parts) {
			auto output = std::make_unique<AssDialogue>(*line.source);
			output->Start = part.start_ms;
			output->End = part.end_ms;
			output->Text = part.text;
			next.push_back(output.get());
			state.outputs.push_back(output.get());
			state.live_lines.emplace(output.get(), output.get());
			state.candidate.Events.insert(anchor, *output.release());
		}
		for (auto *candidate : previous) {
			state.removed.push_back(prepared->LiveLine(candidate));
			state.candidate.Events.erase(state.candidate.Events.iterator_to(*candidate));
			state.live_lines.erase(candidate);
			delete candidate;
		}
		previous = std::move(next);
	}
	return prepared;
}

void MotionTrackApplySource::PreparedApply::Finalize(agi::vfr::Framerate const& timecodes) {
	if (state->installed || state->finalized)
		throw std::logic_error("A Motion Track transaction can only be finalized once");
	std::vector<AssDialogue *> current;
	auto groups = state->groups;
	for (auto& group : groups)
		for (auto *& line : group) {
			current.push_back(line);
			line = LiveLine(line);
		}
	auto snapshot = std::make_unique<MotionTrackSourceSnapshot>(CaptureMotionTrackSource(state->candidate, current, timecodes, state->source->snapshot_->video_frame_count));
	snapshot->file_identity = reinterpret_cast<std::uintptr_t>(state->live);
	for (auto& fingerprint : snapshot->lines)
		fingerprint.identity = reinterpret_cast<std::uintptr_t>(LiveLine(reinterpret_cast<AssDialogue *>(fingerprint.identity)));
	std::vector<AssDialogue *> order;
	for (auto& line : state->candidate.Events)
		order.push_back(LiveLine(&line));
	state->candidate.Folding().RemapPreparedRows([this](AssDialogue *line) { return LiveLine(line); });
	state->groups.swap(groups);
	state->order.swap(order);
	state->snapshot = std::move(snapshot);
	state->finalized = true;
}

void MotionTrackApplySource::PreparedApply::Install() {
	if (!state->finalized || state->installed)
		throw std::logic_error("A Motion Track transaction must be finalized before installation");
	for (auto& candidate : state->candidate.Events) {
		auto *live = LiveLine(&candidate);
		if (live != &candidate) {
			live->Row = candidate.Row;
			live->ExtradataIds.swap(candidate.ExtradataIds);
		}
	}
	state->live->Events.clear();
	for (auto *line : state->order) {
		if (line->is_linked())
			line->unlink();
		state->live->Events.push_back(*line);
	}
	state->live->InstallMotionTrackMetadata(state->candidate);
	state->source->current_lines_.swap(state->groups);
	state->source->snapshot_.swap(state->snapshot);
	state->source->applied_ = true;
	state->installed = true;
}

bool MotionTrackApplySource::Apply(AssFile& file, MotionTrackApplyPlan const& plan,
								   agi::vfr::Framerate const& timecodes,
								   std::function<void(std::vector<AssDialogue *> const&)> const& publish,
								   std::string& message) {
	auto prepared = Prepare(file, plan, timecodes, message);
	if (!prepared)
		return false;
	prepared->Candidate().PrepareCommit(AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME);
	prepared->Finalize(timecodes);
	prepared->Install();
	try {
		publish(prepared->Outputs());
	}
	catch (...) {
		throw MotionTrackApplyPublicationError(std::current_exception());
	}
	return true;
}

} // namespace aegisub::motion_track
