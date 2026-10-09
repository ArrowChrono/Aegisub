#include "scenechange_native_api.h"

#include "options.h"

#include <libaegisub/exception.h>
#include <libaegisub/log.h>
#include <libaegisub/native_library.h>

#include <mutex>
#include <string>

namespace scenechange {
namespace {

constexpr char kLogTag[] = "provider/scenechange/runtime";
constexpr char kScxvidLibraryName[] = "scenechange_xvid";
constexpr char kWwxdLibraryName[] = "scenechange_wwxd";

Api scxvid_api;
Api wwxd_api;
std::mutex api_mutex;

void ResolveProvider(scenechange_provider_get_api_fn get_api, int32_t expected_backend, Api& loaded) {
	scenechange_provider_api provider {};
	provider.struct_size = SCENECHANGE_PROVIDER_API_V1_SIZE;
	char error[1024] = {};
	std::int32_t const rc = get_api(
		SCENECHANGE_PROVIDER_API_VERSION,
		&provider,
		error,
		sizeof(error));
	if (rc != 0)
		throw agi::EnvironmentError(error[0] ? error : "SceneChange provider API negotiation failed.");
	if (provider.struct_size != SCENECHANGE_PROVIDER_API_V1_SIZE)
		throw agi::EnvironmentError("SceneChange provider returned a truncated function table.");
	if (provider.api_version != SCENECHANGE_PROVIDER_API_VERSION)
		throw agi::EnvironmentError("SceneChange provider returned an incompatible API version.");
	if (provider.backend != expected_backend)
		throw agi::EnvironmentError("SceneChange provider returned an unexpected backend.");
	int32_t const expected_pixel_format = expected_backend == SCENECHANGE_PROVIDER_BACKEND_SCXVID
		? SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8
		: SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16;
	if (provider.input_pixel_format != expected_pixel_format)
		throw agi::EnvironmentError("SceneChange provider returned an incompatible input pixel format.");
	constexpr std::uint32_t required_capabilities =
		SCENECHANGE_PROVIDER_CAPABILITY_OWNED_WRITE_FRAME |
		SCENECHANGE_PROVIDER_CAPABILITY_RESET |
		SCENECHANGE_PROVIDER_CAPABILITY_PROGRESS_CALLBACK;
	if ((provider.capabilities & required_capabilities) != required_capabilities)
		throw agi::EnvironmentError("SceneChange provider does not expose the required capabilities.");
	if (!provider.create || !provider.destroy || !provider.reset ||
		!provider.set_progress_callback || !provider.get_write_frame ||
		!provider.commit_written_frame)
		throw agi::EnvironmentError("SceneChange provider returned an incomplete function table.");
	loaded.provider = provider;
	loaded.backend = expected_backend == SCENECHANGE_PROVIDER_BACKEND_SCXVID
		? Api::Backend::ScxvidProvider
		: Api::Backend::WwxdProvider;
}

std::string FormatRuntimeVersionDetail(Api const& loaded) {
	if (loaded.backend == Api::Backend::None)
		return {};
	return "provider-api=" + std::to_string(loaded.provider.api_version)
		+ ", backend=" + BackendName(loaded.backend);
}

std::string GetScxvidRuntimeVersionDetail() {
	std::lock_guard<std::mutex> lock(api_mutex);
	return FormatRuntimeVersionDetail(scxvid_api);
}

std::string GetWwxdRuntimeVersionDetail() {
	std::lock_guard<std::mutex> lock(api_mutex);
	return FormatRuntimeVersionDetail(wwxd_api);
}

void InitializeScxvidProvider(agi::native::Library& library) {
	Api loaded;
	auto get_api = library.ResolveSymbol<scenechange_provider_get_api_fn>("scenechange_provider_get_api");
	ResolveProvider(get_api, SCENECHANGE_PROVIDER_BACKEND_SCXVID, loaded);

	std::lock_guard<std::mutex> lock(api_mutex);
	scxvid_api = loaded;
}

void InitializeWwxdProvider(agi::native::Library& library) {
	Api loaded;
	auto get_api = library.ResolveSymbol<scenechange_provider_get_api_fn>("scenechange_provider_get_api");
	ResolveProvider(get_api, SCENECHANGE_PROVIDER_BACKEND_WWXD, loaded);

	std::lock_guard<std::mutex> lock(api_mutex);
	wwxd_api = loaded;
}

agi::native::CachedLibrary scxvid_provider_library(
	kScxvidLibraryName,
	"SceneChange Xvid provider",
	kLogTag,
	InitializeScxvidProvider,
	GetScxvidRuntimeVersionDetail,
	agi::native::DefaultAppLocalLoadOptions(false));

agi::native::CachedLibrary wwxd_provider_library(
	kWwxdLibraryName,
	"SceneChange WWXD provider",
	kLogTag,
	InitializeWwxdProvider,
	GetWwxdRuntimeVersionDetail,
	agi::native::DefaultAppLocalLoadOptions(false));

enum class BackendPreference {
	Auto,
	Scxvid,
	Wwxd,
};

BackendPreference GetBackendPreference() noexcept {
	try {
		auto const value = config::GetStringOptionOrDefault("Provider/SceneChange/Backend", "auto");
		if (value == "scxvid")
			return BackendPreference::Scxvid;
		if (value == "wwxd")
			return BackendPreference::Wwxd;
	}
	catch (...) {
	}
	return BackendPreference::Auto;
}

// Copy a candidate under the load mutex so callers hold a stable Api snapshot.
BackendSelectionStatus TryCandidate(agi::native::CachedLibrary& library,
	Api const& candidate,
	InputPixelFormatMask supported_input_formats,
	Api *out_selected) noexcept {
	if (!library.IsAvailable())
		return BackendSelectionStatus::Unavailable;

	{
		std::lock_guard<std::mutex> lock(api_mutex);
		if (!SupportsInputPixelFormat(supported_input_formats, candidate.provider.input_pixel_format))
			return BackendSelectionStatus::Unsupported;
		if (out_selected)
			*out_selected = candidate;
	}
	return BackendSelectionStatus::Selected;
}

void LogFallback(Api::Backend skipped, Api::Backend selected) {
	try {
		LOG_I(kLogTag) << "Fallback from " << BackendName(skipped)
			<< " because its input pixel format is unsupported; using "
			<< BackendName(selected) << ".";
	}
	catch (...) {
	}
}

} // namespace

BackendSelection SelectBackendForDimensions(
	int32_t width,
	int32_t height,
	InputPixelFormatMask supported_input_formats,
	bool log_fallback) {
	BackendSelection result;
	if (width <= 0 || height <= 0)
		return result;

	const bool even_dimensions = (width & 1) == 0 && (height & 1) == 0;
	const auto preference = GetBackendPreference();
	bool saw_unsupported = false;
	Api::Backend skipped_backend = Api::Backend::None;

	auto try_select = [&](agi::native::CachedLibrary& library, Api const& candidate) {
		Api selected;
		auto const status = TryCandidate(library, candidate, supported_input_formats, &selected);
		if (status == BackendSelectionStatus::Unsupported) {
			saw_unsupported = true;
			if (skipped_backend == Api::Backend::None)
				skipped_backend = candidate.backend;
		}
		else if (status == BackendSelectionStatus::Selected) {
			if (log_fallback && skipped_backend != Api::Backend::None)
				LogFallback(skipped_backend, selected.backend);
			result.status = BackendSelectionStatus::Selected;
			result.selected = selected;
		}
		return status;
	};
	auto try_scxvid = [&] {
		return even_dimensions
			? try_select(scxvid_provider_library, scxvid_api)
			: BackendSelectionStatus::Unavailable;
	};
	auto try_wwxd = [&] {
		return try_select(wwxd_provider_library, wwxd_api);
	};

	if (preference == BackendPreference::Scxvid) {
		if (try_scxvid() == BackendSelectionStatus::Selected ||
			try_wwxd() == BackendSelectionStatus::Selected)
			return result;
	}
	else if (preference == BackendPreference::Wwxd) {
		if (try_wwxd() == BackendSelectionStatus::Selected ||
			try_scxvid() == BackendSelectionStatus::Selected)
			return result;
	}
	else {
		if (try_scxvid() == BackendSelectionStatus::Selected ||
			try_wwxd() == BackendSelectionStatus::Selected)
			return result;
	}

	result.status = saw_unsupported
		? BackendSelectionStatus::Unsupported
		: BackendSelectionStatus::Unavailable;
	return result;
}

} // namespace scenechange
