#include "font_face_selection.h"

bool ShouldWriteFontFace(
	std::string_view stored_name,
	std::string_view selected_name,
	bool face_modified) {
	return face_modified && stored_name != selected_name;
}
