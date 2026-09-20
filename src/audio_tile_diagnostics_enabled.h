#pragma once

#include <cstdlib>
#include <string_view>

namespace aegisub {

// Shared by audio and legacy video GL diagnostics; never enables tracing itself.
inline bool AudioTileDiagnosticsEnabled() noexcept {
	static bool const enabled = [] {
		auto const *value = std::getenv("AEGISUB_AUDIO_TILE_DIAGNOSTICS");
		if (!value)
			return false;
		auto const setting = std::string_view(value);
		return setting == "1" || setting == "true" || setting == "yes" || setting == "on";
	}();
	return enabled;
}

}
