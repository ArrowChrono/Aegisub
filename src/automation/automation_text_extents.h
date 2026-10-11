#pragma once

#include <string>

class AssStyle;

namespace Automation4 {

bool CalculateAutomationTextExtents(AssStyle const& style, std::string const& text,
									double& width, double& height, double& descent, double& extlead);

}
