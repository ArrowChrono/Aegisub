#pragma once

#include "apply_plan.h"
#include "../ass_dialogue.h"

#include <functional>
#include <exception>
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>

namespace aegisub::motion_track {

/// Keeps the Analyze-time subtitle baseline while Apply replaces its live
/// lines. Reapplying always starts from that baseline, so changing output
/// options never compounds the previously written motion or splits.
class MotionTrackApplySource {
	std::unique_ptr<MotionTrackSourceSnapshot> snapshot_ = std::make_unique<MotionTrackSourceSnapshot>();
	std::vector<std::unique_ptr<AssDialogue>> originals_;
	std::vector<std::vector<AssDialogue *>> current_lines_;
	bool applied_ = false;

	public:
	void Capture(AssFile const& file, std::vector<AssDialogue *> const& targets,
				 agi::vfr::Framerate const& timecodes, int video_frame_count);
	void Clear();
	[[nodiscard]] MotionTrackSourceSnapshot const& Snapshot() const { return *snapshot_; }
	[[nodiscard]] std::vector<AssDialogue *> Targets() const;
	[[nodiscard]] bool ContinueTargetsMatch(std::vector<AssDialogue *> const& targets) const;

	class PreparedApply {
		friend class MotionTrackApplySource;
		struct State;
		std::unique_ptr<State> state;

		public:
		PreparedApply();
		~PreparedApply();
		AssFile& Candidate();
		std::vector<AssDialogue *> const& Outputs() const;
		AssDialogue *LiveLine(AssDialogue const *line) const;
		void Finalize(agi::vfr::Framerate const& timecodes);
		void Install();
	};
	std::unique_ptr<PreparedApply> Prepare(AssFile& file, MotionTrackApplyPlan const& plan,
										   agi::vfr::Framerate const& timecodes, std::string& message);

	bool Apply(AssFile& file, MotionTrackApplyPlan const& plan,
			   agi::vfr::Framerate const& timecodes,
			   std::function<void(std::vector<AssDialogue *> const&)> const& publish,
			   std::string& message);
};

class MotionTrackApplyPublicationError : public std::exception {
	std::exception_ptr cause_;

	public:
	explicit MotionTrackApplyPublicationError(std::exception_ptr cause) noexcept
		: cause_(std::move(cause)) {}
	char const *what() const noexcept override { return "Motion track was applied, but a publication observer failed"; }
	[[nodiscard]] std::exception_ptr const& Cause() const noexcept { return cause_; }
};

} // namespace aegisub::motion_track
