#include "automation_visual_guide_snapshot.h"

#include "../visual_guide_model.h"

namespace Automation4 {
namespace {
constexpr char kAutomationVisualGuideKind[] = "measurement_segment";
constexpr char kAutomationVisualGuideCoordinateSpace[] = "script";
}

AutomationVisualGuide ToAutomationVisualGuide(VisualGuide const& guide) {
	AutomationVisualGuide result;
	result.id = guide.id;
	result.kind = kAutomationVisualGuideKind;
	result.coordinate_space = kAutomationVisualGuideCoordinateSpace;
	result.first = {.x = guide.first.x, .y = guide.first.y};
	result.second = {.x = guide.second.x, .y = guide.second.y};

	auto const metrics = CalculateVisualGuideMetrics(guide);
	result.delta_x = metrics.delta_x;
	result.delta_y = metrics.delta_y;
	result.distance = metrics.distance;
	result.angle_degrees = metrics.angle_degrees;
	result.shear_x = metrics.shear_x;
	result.shear_y = metrics.shear_y;
	result.angle_horizontal_degrees = metrics.angle_horizontal;
	result.angle_vertical_degrees = metrics.angle_vertical;
	return result;
}
}
