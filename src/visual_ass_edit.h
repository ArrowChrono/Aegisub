#pragma once

#include "visual_edit_controller.h"

class AssDialogue;
class AssFile;
class AssStyle;
namespace agi::vfr {
class Framerate;
}

namespace aegisub::visual_edit {

Line ReadLine(AssDialogue const& dialogue, AssStyle const *style, Vector2D script_size);
Line ReadLine(AssDialogue const& dialogue, AssStyle const *style, Vector2D script_size, agi::vfr::Framerate const& timecodes);
void RefreshGeometryFromText(Line& line, Vector2D script_size);
std::vector<Line> BuildLines(AssFile const& file, agi::vfr::Framerate const& timecodes, Vector2D script_size);
void SetOverride(AssDialogue& line, std::string const& tag, std::string const& value);
std::string SetOverride(std::string const& text, std::string const& tag, std::string const& value);

}
