#pragma once

#include "../perspective_forward.h"

#include <optional>

class AssDialogue;
class AssFile;

namespace aegisub::motion_track {

struct ApplyPlanInput;

/// Prepares immutable bounds for optional geometry-aware Compact fitting.
/// max_scale_x bounds both raw reference and serialized candidate X scales
/// over the processed trajectory. Candidates must not exceed it. Dynamic layout
/// returns nullopt so the caller can retain its scalar fitting path.
/// Text requires a font measurement provider; drawings have intrinsic bounds.
[[nodiscard]] std::optional<perspective::ForwardInput> PrepareCompactGeometry(
	AssFile const& file, AssDialogue const& line, ApplyPlanInput const& input,
	double max_scale_x);

} // namespace aegisub::motion_track
