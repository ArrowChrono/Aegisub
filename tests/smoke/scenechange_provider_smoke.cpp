#include "include/aegisub/video_provider.h"
#include "scenechange_native_api.h"
#include "video_provider_manager.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/keyframe.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Progress final : public agi::ProgressSink {
	int64_t current = 0;
	int64_t total = 0;
	bool monotonic = true;
	int64_t cancel_at = 0;

public:
	explicit Progress(int64_t cancel_at = 0) : cancel_at(cancel_at) { }

	void SetIndeterminate() override { }
	void SetTitle(std::string const&) override { }
	void SetMessage(std::string const&) override { }
	void Log(std::string const&) override { }
	bool IsCancelled() override { return cancel_at > 0 && current >= cancel_at; }

	void SetProgress(int64_t value, int64_t maximum) override {
		monotonic = monotonic && value >= current;
		current = value;
		total = maximum;
	}

	void Validate(int expected_frames) const {
		if (!monotonic || current != expected_frames || total != expected_frames)
			throw std::runtime_error("SceneChange progress did not reach the video frame count");
	}

	void ValidateCancellation(int expected_frame, int expected_total) const {
		if (!monotonic || current != expected_frame || total != expected_total)
			throw std::runtime_error("SceneChange cancellation occurred at an unexpected progress value");
	}
};

std::vector<int> ParseExpected(std::string const& text) {
	std::vector<int> expected;
	std::istringstream stream(text);
	std::string item;
	while (std::getline(stream, item, ',')) {
		if (item.empty())
			throw std::invalid_argument("expected keyframe list contains an empty item");
		size_t consumed = 0;
		int value = std::stoi(item, &consumed);
		if (consumed != item.size() || value < 0)
			throw std::invalid_argument("expected keyframe list contains an invalid frame");
		expected.push_back(value);
	}
	return expected;
}

void PrintKeyframes(std::vector<int> const& keyframes) {
	std::cout << "keyframes=";
	for (size_t index = 0; index < keyframes.size(); ++index) {
		if (index)
			std::cout << ',';
		std::cout << keyframes[index];
	}
	std::cout << '\n';
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3 || argc > 6) {
		std::cerr << "usage: scenechange-provider-smoke <video> <output.kf.txt> "
			"[expected-frames] [scxvid|wwxd|wwxd-provider] [cancel-at]\n";
		return 2;
	}

	try {
		auto video_path = agi::fs::PathFromString(argv[1]);
		auto output_path = agi::fs::PathFromString(argv[2]);
		auto provider = VideoProviderFactory::GetProviderWithPreferred(
			video_path, {}, "LsmasNative", nullptr, {}, size_t{0});
		if (!provider || provider->GetDecoderName() != "LsmasNative")
			throw std::runtime_error("LsmasNative video provider did not open");
		if (!provider->CanGenerateSceneChangeKeyframes())
			throw std::runtime_error("SceneChange keyframe generation is unavailable");

		std::string expected_backend = argc >= 5 ? argv[4] : "scxvid";
		// CMake STRINGS / CLI: scxvid, wwxd-provider (alias wwxd). Reject typos
		// that would otherwise silently map to the wwxd cache token.
		if (expected_backend != "scxvid" && expected_backend != "wwxd"
			&& expected_backend != "wwxd-provider")
			throw std::invalid_argument("unknown expected backend: " + expected_backend);

		// argv is the independent source; provider token is the system under test.
		std::string const expected_cache_token =
			expected_backend == "scxvid" ? "scxvid" : "wwxd";
		std::string const cache_token = provider->GetSceneChangeKeyframeCacheToken();
		if (cache_token != expected_cache_token)
			throw std::runtime_error("selected SceneChange backend returned an incompatible cache token");

		// Display name only (token assert above is the real check).
		std::string const backend = cache_token == "scxvid"
			? scenechange::BackendName(scenechange::Api::Backend::ScxvidProvider)
			: scenechange::BackendName(scenechange::Api::Backend::WwxdProvider);

		int64_t cancel_at = argc == 6 ? std::stoll(argv[5]) : 0;
		if (cancel_at < 0 || cancel_at > provider->GetFrameCount())
			throw std::invalid_argument("cancel-at is outside the video frame range");
		Progress progress(cancel_at);
		bool canceled = false;
		try {
			provider->GenerateSceneChangeKeyframes(output_path, &progress);
		}
		catch (agi::UserCancelException const&) {
			canceled = true;
		}
		if (cancel_at > 0) {
			if (!canceled)
				throw std::runtime_error("SceneChange scan ignored the requested cancellation");
			progress.ValidateCancellation(static_cast<int>(cancel_at), provider->GetFrameCount());
			// Odd dimensions must select WWXD when it is available (scxvid requires even sizes).
			auto const odd = scenechange::SelectBackendForDimensions(
				65, 49, scenechange::kAllInputPixelFormats);
			bool const odd_supported = odd.status == scenechange::BackendSelectionStatus::Selected;
			std::string const odd_backend = scenechange::BackendName(odd.selected.backend);
			if (odd_supported != (odd_backend.rfind("wwxd-", 0) == 0))
				throw std::runtime_error("selected SceneChange backend returned an invalid odd-dimension capability");
			std::cout << "backend=" << backend << '\n';
			std::cout << "canceled-at=" << cancel_at << '\n';
			return 0;
		}
		if (canceled)
			throw std::runtime_error("SceneChange scan was canceled unexpectedly");
		progress.Validate(provider->GetFrameCount());

		auto keyframes = agi::keyframe::Load(output_path);
		if (!std::is_sorted(keyframes.begin(), keyframes.end()))
			throw std::runtime_error("generated keyframes are not sorted");
		if (argc >= 4 && keyframes != ParseExpected(argv[3]))
			throw std::runtime_error("generated keyframes do not match the expected list");
		auto const odd = scenechange::SelectBackendForDimensions(
			65, 49, scenechange::kAllInputPixelFormats);
		bool const odd_supported = odd.status == scenechange::BackendSelectionStatus::Selected;
		std::string const odd_backend = scenechange::BackendName(odd.selected.backend);
		if (odd_supported != (odd_backend.rfind("wwxd-", 0) == 0))
			throw std::runtime_error("selected SceneChange backend returned an invalid odd-dimension capability");

		std::cout << "backend=" << backend << '\n';
		std::cout << "frames=" << provider->GetFrameCount() << '\n';
		PrintKeyframes(keyframes);
		return 0;
	}
	catch (agi::Exception const& error) {
		std::cerr << "scenechange-provider-smoke: " << error.GetMessage() << '\n';
		return 1;
	}
	catch (std::exception const& error) {
		std::cerr << "scenechange-provider-smoke: " << error.what() << '\n';
		return 1;
	}
}
