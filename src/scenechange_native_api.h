#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Synchronized with SceneChangeSharp common provider ABI.
#include "scenechange_provider_api.h"

#if defined(_MSC_VER)
#define SCENECHANGE_NATIVE_CALL __cdecl
#else
#define SCENECHANGE_NATIVE_CALL
#endif

extern "C" {

using scenechange_progress_callback_fn = int32_t (SCENECHANGE_NATIVE_CALL *)(
	void *user_data,
	int32_t processed_frames,
	int32_t scene_change);
}

namespace scenechange {

using InputPixelFormatMask = uint32_t;

constexpr InputPixelFormatMask kInputPixelFormatGray8Padded16 = 1u << 0;
constexpr InputPixelFormatMask kInputPixelFormatYuv420p8 = 1u << 1;
constexpr InputPixelFormatMask kAllInputPixelFormats =
	kInputPixelFormatGray8Padded16 | kInputPixelFormatYuv420p8;

constexpr InputPixelFormatMask GetInputPixelFormatMask(int32_t input_pixel_format) noexcept {
	switch (input_pixel_format) {
	case SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16:
		return kInputPixelFormatGray8Padded16;
	case SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8:
		return kInputPixelFormatYuv420p8;
	default:
		return 0;
	}
}

constexpr bool SupportsInputPixelFormat(InputPixelFormatMask supported_formats,
	int32_t input_pixel_format) noexcept {
	return (supported_formats & GetInputPixelFormatMask(input_pixel_format)) != 0;
}

enum class BackendSelectionStatus {
	Selected,
	Unsupported,
	Unavailable,
};

struct Api {
	enum class Backend {
		None,
		WwxdProvider,
		ScxvidProvider,
	};

	Backend backend = Backend::None;
	scenechange_provider_api provider {};
};

struct BackendSelection {
	BackendSelectionStatus status = BackendSelectionStatus::Unavailable;
	Api selected;
};

inline char const *BackendName(Api::Backend backend) noexcept {
	switch (backend) {
		case Api::Backend::WwxdProvider: return "wwxd-provider";
		case Api::Backend::ScxvidProvider: return "scxvid";
		default: return "none";
	}
}

inline char const *CacheTokenForBackend(Api::Backend backend) noexcept {
	return backend == Api::Backend::ScxvidProvider ? "scxvid" : "wwxd";
}

class KeyframeRequest {
	Api api;

	public:
	explicit KeyframeRequest(Api const& selected) noexcept : api(selected) {}
	[[nodiscard]] char const *GetCacheToken() const noexcept { return CacheTokenForBackend(api.backend); }
	[[nodiscard]] Api const& GetApi() const noexcept { return api; }
};

// Selects the preferred compatible backend which accepts one of
// supported_input_formats. Returns the selected Api by value so callers do not
// depend on mutable process-global selection state.
BackendSelection SelectBackendForDimensions(
	int32_t width,
	int32_t height,
	InputPixelFormatMask supported_input_formats,
	bool log_fallback = false);

} // namespace scenechange
