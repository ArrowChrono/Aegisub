#include "compact_geometry.h"

#include "apply_plan.h"
#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../perspective_ass_bounds.h"
#include "../perspective_ass_state.h"
#include "../perspective_tag_solver.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace aegisub::motion_track {

std::optional<perspective::ForwardInput> PrepareCompactGeometry(
	AssFile const& file, AssDialogue const& line, ApplyPlanInput const& input,
	double max_scale_x) {
	using namespace perspective;
	if (input.script_width <= 0 || input.script_height <= 0 ||
		input.storage_width <= 0 || input.storage_height <= 0 ||
		line.End.GetMillisecond() <= line.Start.GetMillisecond() ||
		!std::isfinite(max_scale_x) || max_scale_x <= 0 || max_scale_x > MaxTransformParameter)
		return std::nullopt;

	double x = 0.0, y = 0.0;
	ResolveDialogueOrigin(file, line, input.seed_time_ms, input.script_width, input.script_height, x, y);
	AssDialogue frozen(line);
	frozen.Text = ReplacePositionTag(line.Text.get(), "\\pos" + FormatAssPoint({.x = x, .y = y}, 6));
	int const capture_time = std::clamp(input.seed_time_ms,
										line.Start.GetMillisecond(), line.End.GetMillisecond() - 1);
	auto const evaluated = EvaluateEffectiveAssState({.file = &file, .line = &frozen, .play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)}, .capture_time_ms = capture_time});
	// A named reset can still yield one homogeneous static geometry. We only
	// measure the source here; Perspective's rewrite restriction does not apply.
	if (!evaluated || (evaluated.apply_blocker != AssApplyBlocker::None &&
					   evaluated.apply_blocker != AssApplyBlocker::UnsupportedNamedReset))
		return std::nullopt;
	if (!evaluated.value.drawing_mode &&
		(!input.text_extents || evaluated.value.text_style.font_name.empty()))
		return std::nullopt;

	ForwardInput source;
	source.play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)};
	source.video_storage_resolution = Resolution{.width = static_cast<double>(input.storage_width), .height = static_cast<double>(input.storage_height)};
	int const layout_x = file.GetScriptInfoAsInt("LayoutResX");
	int const layout_y = file.GetScriptInfoAsInt("LayoutResY");
	if (layout_x > 0 && layout_y > 0)
		source.layout_resolution = Resolution{.width = static_cast<double>(layout_x), .height = static_cast<double>(layout_y)};
	auto const layout_aspect = ResolvePerspectiveLayoutAspect(source);
	if (!layout_aspect)
		return std::nullopt;

	// Scale does not change the untransformed box, but can trigger automatic
	// wrapping. Measure once against the widest source/reference/candidate
	// scale so a later zoom cannot silently invalidate the cached layout.
	auto bounds_state = evaluated.value;
	bounds_state.transform.scale_x = std::max(bounds_state.transform.scale_x, max_scale_x);
	auto bounds = EvaluateAssBaseBounds({.line = &frozen, .state = &bounds_state, .text_extents = input.text_extents, .layout_aspect = *layout_aspect});
	if (!bounds)
		return std::nullopt;
	source.bounds = std::move(bounds.value);
	source.state = evaluated.value.transform;
	// Multi-line text is sheared per baseline by the renderer, not as one box.
	if (source.bounds.multiline_text && (source.state.shear_x != 0.0 || source.state.shear_y != 0.0))
		return std::nullopt;
	if (!ForwardQuad(source))
		return std::nullopt;
	return source;
}

} // namespace aegisub::motion_track
