#pragma once

#include "apply_plan.h"
#include "../ass_file.h"

#include <iosfwd>
#include <memory>

namespace aegisub::motion_track {

struct MotionTrackSnapshot;

/// Owns the source lines used by input/exported_plan. No project properties,
/// unrelated dialogue, attachments, or extradata payloads are serialized.
struct MotionTrackDebugBundle {
	std::unique_ptr<AssFile> file;
	std::vector<AssDialogue *> targets;
	ApplyPlanInput input;
	MotionTrackApplyPlan exported_plan;
	bool used_platform_text_extents = false;
};

/// Includes subtitle text/font names; it is diagnostic data, not anonymized
/// content. Call with the Analyze-time targets, not Apply's replacement lines.
/// The optional snapshot adds ROI/direction/stop metadata, never provider IDs.
void WriteMotionTrackDebugBundle(std::ostream& output, AssFile const& file,
								 std::vector<AssDialogue *> const& targets, ApplyPlanInput const& input,
								 MotionTrackApplyPlan const& plan, MotionTrackSnapshot const *snapshot = nullptr,
								 std::string const& application_version = {});

/// Throws on malformed/unsupported data. Geometry-aware Similarity Compact and
/// full-geometry replay need the same font measurement provider and fonts as
/// the capture. nullptr keeps Similarity's strict scalar fallback and uses the
/// existing deterministic bounds approximation for full geometry.
MotionTrackDebugBundle ReadMotionTrackDebugBundle(std::istream& stream,
												  perspective::AssTextExtentsProvider text_extents = nullptr);

} // namespace aegisub::motion_track
