#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct VisualGuide;

namespace Automation4 {

struct AutomationVisualGuidePoint {
	double x = 0.0;
	double y = 0.0;
};

// A UI- and renderer-independent copy of one visual measurement guide. The
// string fields deliberately form the Automation boundary rather than exposing
// visual-guide model types to Lua or other scripting engines.
//
// kind is currently always "measurement_segment" and coordinate_space is always
// "script". Scripts should tolerate unknown future values.
struct AutomationVisualGuide {
	std::string id;
	std::string kind;
	std::string coordinate_space;
	AutomationVisualGuidePoint first;
	AutomationVisualGuidePoint second;
	double delta_x = 0.0;
	double delta_y = 0.0;
	double distance = 0.0;
	double angle_degrees = 0.0;
	// Shear coefficients aligned with the \fax/\fay tags. shear_x = Δx/Δy,
	// shear_y = Δy/Δx; 0.0 when the denominator is 0. angle_horizontal/vertical
	// are the angles to the X/Y axes in degrees (atan2-based). Added after
	// schema 1; scripts that ignore unknown keys are unaffected.
	double shear_x = 0.0;
	double shear_y = 0.0;
	double angle_horizontal_degrees = 0.0;
	double angle_vertical_degrees = 0.0;
};

[[nodiscard]] AutomationVisualGuide ToAutomationVisualGuide(VisualGuide const& guide);

// A value-only snapshot captured by the live UI host. It intentionally has no
// wx, rendering, Lua, controller, or guide-model references.
struct AutomationVisualGuideSnapshot {
	bool available = false;
	std::uint64_t generation = 0;
	int frame = -1;
	int script_width = 0;
	int script_height = 0;
	int frame_width = 0;
	int frame_height = 0;
	std::optional<std::string> selected_id;
	std::optional<std::string> last_measurement_id;
	std::vector<AutomationVisualGuide> guides;
};

}
