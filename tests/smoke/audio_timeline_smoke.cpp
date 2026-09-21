#include "audio_provider_factory.h"
#include "options.h"
#include "ui_services.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct AudioBlock {
	int64_t start;
	std::vector<int16_t> samples;
};

struct Reference {
	std::string filename;
	int64_t samples;
	int64_t silence;
	int tolerance;
	std::vector<AudioBlock> blocks;
};

void Require(bool condition, std::string const& message) {
	if (!condition)
		throw std::runtime_error(message);
}

std::vector<Reference> ReadReference(fs::path const& filename) {
	std::ifstream input(filename);
	std::string token;
	int version = 0;
	input >> token >> version;
	Require(input && token == "MEDIA_SEEK" && version == 1, "invalid media reference header");
	std::vector<Reference> references;
	while (input >> token) {
		Require(token == "FILE", "expected FILE in media reference");
		Reference reference{};
		int frames = 0;
		input >> reference.filename >> frames >> reference.samples >> reference.silence >> reference.tolerance;
		Require(input && frames > 0 && reference.samples > reference.silence && reference.tolerance == 2,
				"invalid media reference metadata");
		input >> token;
		Require(token == "PTS", "expected PTS in media reference");
		for (int frame = 0; frame < frames; ++frame) {
			int64_t pts = 0;
			input >> pts;
		}
		while (input >> token && token != "END") {
			Require(token == "AUDIO", "expected AUDIO in media reference");
			AudioBlock block{};
			int count = 0;
			input >> block.start >> count;
			Require(input && count == 960, "invalid audio reference block size");
			block.samples.resize(static_cast<size_t>(count));
			for (auto& sample : block.samples) {
				int value = 0;
				input >> value;
				Require(input && value >= -32768 && value <= 32767, "invalid PCM reference sample");
				sample = static_cast<int16_t>(value);
			}
			reference.blocks.push_back(std::move(block));
		}
		Require(token == "END" && reference.blocks.size() == 8, "incomplete media reference");
		references.push_back(std::move(reference));
	}
	Require(references.size() == 2, "expected AAC and Opus media references");
	return references;
}

std::unique_ptr<agi::AudioProvider> OpenProvider(fs::path const& file, std::string const& name, Reference const& reference) {
	agi::InlineBackgroundRunnerFactory runners;
	auto runner = runners.Create({}, {});
	auto provider = GetAudioProviderWithPreferred(file, name, runner.get(), nullptr);
	Require(provider->GetMemoryStats().provider_name == name, "audio provider unexpectedly fell back from " + name);
	Require(provider->GetSampleRate() == 48000 && provider->GetChannels() == 1, "unexpected audio format");
	Require(provider->GetNumSamples() == reference.samples,
			"sample count mismatch: expected " + std::to_string(reference.samples) + ", actual " + std::to_string(provider->GetNumSamples()));
	return provider;
}

std::vector<int16_t> Read(agi::AudioProvider const& provider, int64_t start, int64_t count) {
	std::vector<int16_t> result(static_cast<size_t>(count), -32768);
	provider.GetInt16MonoAudioChecked(result.data(), start, count);
	return result;
}

void CompareReference(Reference const& reference, AudioBlock const& block, std::vector<int16_t> const& actual, bool fresh_seek) {
	Require(actual.size() == block.samples.size(), "PCM block size mismatch");
	double signal_energy = 0;
	double residual_energy = 0;
	int peak_error = 0;
	bool all_actual_zero = true;
	for (size_t i = 0; i < actual.size(); ++i) {
		int const expected = block.samples[i];
		int const error = std::abs(static_cast<int>(actual[i]) - expected);
		int64_t const position = block.start + static_cast<int64_t>(i);
		if (position < reference.silence || position >= reference.samples)
			Require(actual[i] == 0, "nonzero timeline padding at sample " + std::to_string(position));
		int const residual = std::max(0, error - reference.tolerance);
		residual_energy += static_cast<double>(residual) * residual;
		signal_energy += static_cast<double>(expected) * expected;
		peak_error = std::max(peak_error, error);
		all_actual_zero = all_actual_zero && actual[i] == 0;
	}
	auto const position = " at sample " + std::to_string(block.start);
	if (signal_energy == 0)
		Require(all_actual_zero, "nonzero silent reference block" + position);
	else if (fresh_seek)
		Require(std::sqrt(residual_energy / signal_energy) <= 0.01,
				"fresh codec seek differs from independent reference" + position + ": normalized residual=" + std::to_string(std::sqrt(residual_energy / signal_energy)));
	else
		Require(peak_error <= reference.tolerance,
				"sequential PCM differs from independent reference" + position + ": peak error=" + std::to_string(peak_error));
}

std::vector<int16_t> Slice(std::vector<int16_t> const& samples, int64_t start, size_t count) {
	std::vector<int16_t> result(count);
	for (size_t i = 0; i < count; ++i) {
		int64_t const index = start + static_cast<int64_t>(i);
		if (index >= 0 && std::cmp_less(index, samples.size()))
			result[i] = samples[static_cast<size_t>(index)];
	}
	return result;
}

void CheckPadding(agi::AudioProvider const& provider, Reference const& reference) {
	for (auto const range : {std::pair<int64_t, int64_t>{-64, 128}, {reference.samples, 128}}) {
		auto const pcm = Read(provider, range.first, range.second);
		Require(std::ranges::all_of(pcm, [](int16_t sample) { return sample == 0; }), "head/EOF padding is not exact silence");
	}
}

void CheckProvider(fs::path const& fixtures, fs::path const& artifacts, Reference const& reference, std::string const& name) {
	auto const file = fixtures / reference.filename;
	constexpr std::array<size_t, 10> order{6, 1, 7, 3, 0, 5, 2, 4, 1, 7};
	for (std::string const mode : {"sequential", "fresh-seek", "ram", "hd"}) {
		std::cout << "CHECK " << reference.filename << " provider=" << name << " mode=" << mode << '\n';
		auto provider = OpenProvider(file, name, reference);
		bool const cached = mode == "ram" || mode == "hd";
		if (mode == "ram")
			provider = agi::CreateRAMAudioProvider(std::move(provider));
		else if (mode == "hd")
			provider = agi::CreateHDAudioProvider(std::move(provider), artifacts);
		if (cached) {
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (provider->GetDecodedSamples() < reference.samples && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Require(provider->GetDecodedSamples() == reference.samples, "audio cache did not finish within 10 seconds");
		}
		bool const fresh_seek = mode == "fresh-seek";
		std::vector<int16_t> complete;
		if (!fresh_seek) {
			complete = Read(*provider, 0, reference.samples);
			auto const first = std::ranges::find_if(complete, [](int16_t sample) { return sample != 0; }) - complete.begin();
			if (reference.filename == "vfr-opus.mkv")
				Require(first == reference.silence, "Opus retained PCM origin does not match the container timeline");
			Require(std::ranges::all_of(complete.begin(), complete.begin() + reference.silence,
										[](int16_t sample) { return sample == 0; }),
					"leading timeline silence is not exact");
			for (auto const& block : reference.blocks)
				CompareReference(reference, block, Slice(complete, block.start, block.samples.size()), false);
			std::cout << "PCM total=" << complete.size() << " first_nonzero=" << first << '\n';
		}
		for (size_t index : order) {
			auto const& block = reference.blocks[index];
			auto const actual = Read(*provider, block.start, static_cast<int64_t>(block.samples.size()));
			CompareReference(reference, block, actual, !cached);
			if (cached)
				Require(actual == Slice(complete, block.start, actual.size()), "cache random read does not match its complete PCM");
		}
		CheckPadding(*provider, reference);
		std::cout << "PASS " << reference.filename << " provider=" << name << " mode=" << mode << '\n';
	}
}
}

int main(int argc, char **argv) {
	if (argc > 3) {
		std::cerr << "usage: audio-timeline-smoke [fixtures-directory] [artifacts-directory]\n";
		return 2;
	}
	agi::dispatch::Init([](agi::dispatch::Thunk const& task) { task(); });
	auto shutdown = agi::make_scope_exit([] { agi::dispatch::Shutdown(); });
	agi::log::LogSink log;
	agi::log::log = &log;
	auto reset_log = agi::make_scope_exit([] { agi::log::log = nullptr; });
	try {
		fs::path const fixtures = argc >= 2 ? argv[1] : "tests/fixtures/media-seek";
		fs::path const artifacts = argc >= 3 ? argv[2] : "build-dir/artifacts/audio-timeline";
		fs::create_directories(artifacts);
		auto const profile = agi::fs::UniquePath(artifacts / "profile-%%%%%%%%");
		fs::create_directories(profile);
		agi::Path paths;
		paths.SetToken("?local", fs::absolute(profile));
		config::path = &paths;
		config::opt = nullptr;
		auto reset_config = agi::make_scope_exit([] { config::path = nullptr; });
		for (auto const& reference : ReadReference(fixtures / "reference.txt"))
			for (std::string const provider : {"FFmpegSource", "LsmasNative"})
				CheckProvider(fixtures, artifacts, reference, provider);
		std::cout << "audio timeline smoke passed\n";
		return 0;
	}
	catch (agi::Exception const& error) {
		std::cerr << "audio timeline smoke failed: " << error.GetMessage() << '\n';
	}
	catch (std::exception const& error) {
		std::cerr << "audio timeline smoke failed: " << error.what() << '\n';
	}
	return 1;
}
