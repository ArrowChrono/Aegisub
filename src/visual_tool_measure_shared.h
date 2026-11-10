#pragma once

#include "perspective_apply_plan.h"
#include "perspective_quad_edit_state.h"
#include "visual_guide_overlay.h"

#include <libaegisub/color.h>

#include <optional>
#include <string>

class VideoOverlayDrawContext;

namespace aegisub::visual_measure {

inline constexpr double PerspectiveMaxError = 0.1;

struct PerspectiveOptions {
	bool fit_text = true;
	bool best_fit = false;
	bool fax_frz_only = false;
	int decimal_places = 4;
	double shape_tolerance = 0.1;
	perspective::PerspectiveEdgeAnchor edge_anchor = perspective::PerspectiveEdgeAnchor::None;
};

struct PerspectivePreview {
	bool feasible = false;
	perspective::SolverError error = perspective::SolverError::None;
	perspective::NoFeasibleReason no_feasible_reason = perspective::NoFeasibleReason::None;
	std::optional<perspective::NoFeasibleMetrics> no_feasible_metrics;
	std::optional<perspective::Quad> reachable;
};

[[nodiscard]] PerspectivePreview SolvePerspectivePreview(
	perspective::PerspectiveSourceSnapshot const& source,
	perspective::Quad const& target, perspective::OutputCoordinateMapping mapping,
	PerspectiveOptions const& options);

[[nodiscard]] std::string LocalizedGeometryError(perspective::GeometryError error);
[[nodiscard]] std::string LocalizedAssApplyBlocker(perspective::AssApplyBlocker blocker);
[[nodiscard]] std::string LocalizedQuadEditError(perspective::PerspectiveQuadEditError error);
[[nodiscard]] std::string LocalizedPlanError(perspective::PerspectivePlanError error);
[[nodiscard]] std::string DescribePerspectiveFailure(perspective::PerspectivePlanDiagnostic const& diagnostic);
[[nodiscard]] std::string DescribePerspectiveApplySuccess(
	perspective::PerspectiveMutationPlan const& plan, perspective::Quad const& drawn_target,
	perspective::PerspectiveApplyContext const& context);
[[nodiscard]] std::string UnreachableTargetDiagnostic(
	perspective::NoFeasibleReason reason,
	std::optional<perspective::NoFeasibleMetrics> const& metrics, int decimal_places);
[[nodiscard]] perspective::PerspectiveEdgeAnchor EdgeAnchorForHandle(perspective::PerspectiveQuadHandle handle);
[[nodiscard]] std::string DescribeEdgeAnchor(perspective::PerspectiveEdgeAnchor anchor);

struct PerspectiveOverlay {
	std::optional<perspective::Quad> current;
	std::optional<perspective::Quad> target;
	std::optional<perspective::Quad> reachable;
	bool creating = false;
	bool editable = false;
	bool off_frame = false;
	bool modified = false;
	bool valid = false;
	bool best_fit = false;
	perspective::PerspectiveQuadHandle active_handle = perspective::PerspectiveQuadHandle::None;
	perspective::PerspectiveEdgeAnchor edge_anchor = perspective::PerspectiveEdgeAnchor::None;
	std::string diagnostic;
};

struct PerspectiveOverlayStyle {
	agi::Color line_primary;
	agi::Color line_secondary;
	agi::Color highlight_primary;
	agi::Color highlight_secondary;
	agi::Color invalid_line;
	agi::Color invalid_handle;
	int label_font_size = 12;
	double ui_scale = 1.0;
};

void DrawPerspective(VideoOverlayDrawContext& context,
					 VisualGuideViewport const& viewport, Vector2D canvas_size,
					 PerspectiveOverlay const& overlay, PerspectiveOverlayStyle const& style);
void DrawGuideHandles(VideoOverlayDrawContext& context, VisualGuideViewport const& viewport,
					  VisualGuide const& guide, VisualGuideOverlayStyle const& style);

}
