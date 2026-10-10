#pragma once

#include <string_view>

bool ShouldWriteFontFace(
	std::string_view stored_name,
	std::string_view selected_name,
	bool face_modified);
