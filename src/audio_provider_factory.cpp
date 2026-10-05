// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "audio_provider_factory.h"

#include "options.h"
#include "provider_catalog_builder.h"
#include "provider_factory_entry.h"
#include "provider_open_policy.h"
#include "translation_service.h"
#include "ui_services.h"
#ifdef WITH_LSMASNATIVE
#include "lsmas_native_api.h"
#endif
#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"
#endif

#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <exception>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

using namespace agi;

std::unique_ptr<AudioProvider> CreateLsmasNativeAudioProvider(fs::path const& filename, BackgroundRunner *, std::shared_ptr<SingleChoiceInteractionSink> choice_sink);
std::unique_ptr<AudioProvider> CreateFFmpegSourceAudioProvider(fs::path const& filename, BackgroundRunner *, std::shared_ptr<SingleChoiceInteractionSink> choice_sink);

namespace {
thread_local aegisub::provider_selection_diagnostics::SelectionReport last_audio_provider_selection_report;

using Factory = AudioProviderFactory;

std::unique_ptr<AudioProvider> CreateDummyAudioProviderWithChoice(fs::path const& filename, BackgroundRunner *br, std::shared_ptr<SingleChoiceInteractionSink>) {
	return CreateDummyAudioProvider(filename, br);
}

std::unique_ptr<AudioProvider> CreatePCMAudioProviderWithChoice(fs::path const& filename, BackgroundRunner *br, std::shared_ptr<SingleChoiceInteractionSink>) {
	return CreatePCMAudioProvider(filename, br);
}

#ifdef WITH_FFMS2
bool IsFFmpegSourceAvailable() {
	return ffms::IsAvailable();
}

std::string GetFFmpegSourceAvailabilityError() {
	auto err = ffms::GetLoadError();
	return err.empty() ? "runtime library is unavailable." : err;
}
#endif

#ifdef WITH_LSMASNATIVE
bool IsLsmasNativeAvailable() {
	return lsmas::IsAvailable();
}

std::string GetLsmasNativeAvailabilityError() {
	auto err = lsmas::GetLoadError();
	return err.empty() ? "runtime library is unavailable." : err;
}
#endif

const Factory builtin_providers[] = {
	{"Dummy", CreateDummyAudioProviderWithChoice, nullptr, nullptr, true},
	{"PCM", CreatePCMAudioProviderWithChoice, nullptr, nullptr, true},
#ifdef WITH_FFMS2
	{"FFmpegSource", CreateFFmpegSourceAudioProvider, IsFFmpegSourceAvailable, GetFFmpegSourceAvailabilityError, false},
#endif
#ifdef WITH_LSMASNATIVE
	{"LsmasNative", CreateLsmasNativeAudioProvider, IsLsmasNativeAvailable, GetLsmasNativeAvailabilityError, false},
#endif
};

std::string GetConfiguredAudioProvider() {
	return config::GetStringOptionOrDefault("Audio/Provider", {});
}

int GetConfiguredAudioCacheType() {
	return config::GetIntOptionOrDefault("Audio/Cache/Type", 0);
}

std::string GetConfiguredHDAudioCacheLocation() {
	return config::GetStringOptionOrDefault("Audio/Cache/HD/Location", "default");
}

std::vector<Factory>& RegisteredProviders() {
	static std::vector<Factory> providers;
	return providers;
}

bool& RegisteredProvidersFrozen() {
	static bool frozen = false;
	return frozen;
}

std::mutex& RegisteredProvidersMutex() {
	static std::mutex mutex;
	return mutex;
}

bool IsValidFactory(Factory const& factory) {
	return factory.name && *factory.name && factory.create;
}

std::vector<Factory> ProviderFactories() {
	std::vector<Factory> factories(std::begin(builtin_providers), std::end(builtin_providers));
	std::lock_guard<std::mutex> lock(RegisteredProvidersMutex());
	auto const& registered = RegisteredProviders();
	factories.insert(factories.end(), registered.begin(), registered.end());
	return factories;
}

}

bool TryRegisterAudioProviderFactory(AudioProviderFactory factory) {
	std::lock_guard<std::mutex> lock(RegisteredProvidersMutex());
	if (RegisteredProvidersFrozen())
		return false;
	if (!IsValidFactory(factory))
		return false;

	auto& providers = RegisteredProviders();
	auto name = factory.name ? factory.name : "";
	auto existing = std::find_if(providers.begin(), providers.end(), [&](auto const& provider) {
		return name == std::string(provider.name ? provider.name : "");
	});
	if (existing == providers.end())
		providers.push_back(factory);
	return true;
}

void RegisterAudioProviderFactory(AudioProviderFactory factory) {
	if (!factory.name || !*factory.name)
		throw std::invalid_argument("audio provider factory requires a non-empty name");
	if (!factory.create)
		throw std::invalid_argument("audio provider factory requires a create callback");
	if (!TryRegisterAudioProviderFactory(factory))
		throw std::logic_error("audio provider registry is frozen");
}

void FreezeAudioProviderFactoryRegistry() {
	std::lock_guard<std::mutex> lock(RegisteredProvidersMutex());
	RegisteredProvidersFrozen() = true;
}

bool IsAudioProviderFactoryRegistryFrozen() {
	std::lock_guard<std::mutex> lock(RegisteredProvidersMutex());
	return RegisteredProvidersFrozen();
}

aegisub::provider_catalog::ProviderCatalog GetAudioProviderCatalog(std::string const& preferred_provider) {
	auto providers = ProviderFactories();
	return aegisub::provider_catalog::BuildCatalog(
		aegisub::provider_catalog::ProviderKind::Audio,
		providers,
		preferred_provider,
		aegisub::provider_catalog::DescribeProviderFactoryEntry<AudioProviderCreate>);
}

std::vector<std::string> GetAudioProviderNames() {
	auto providers = ProviderFactories();
	return aegisub::provider_catalog::VisibleFactoryNames(providers, aegisub::provider_catalog::DescribeProviderFactoryEntry<AudioProviderCreate>);
}

std::vector<std::pair<std::string, std::string>> GetAudioProviderChoices() {
	return aegisub::provider_catalog::VisibleProviderChoices(GetAudioProviderCatalog());
}

std::unique_ptr<AudioProvider> OpenAudioProviderWithPreferred(fs::path const& filename,
                                                              std::string const& preferred_provider,
                                                              BackgroundRunner *br,
                                                              std::shared_ptr<SingleChoiceInteractionSink> choice_sink,
                                                              bool& source_needs_cache) {
	auto preferred = aegisub::provider_selection_diagnostics::CanonicalizeProviderName(preferred_provider);
	auto providers = ProviderFactories();
	auto sorted = aegisub::provider_catalog::SortFactories(providers, preferred, aegisub::provider_catalog::DescribeProviderFactoryEntry<AudioProviderCreate>);
	aegisub::provider_selection_diagnostics::SelectionReport diagnostics;
	diagnostics.preferred_provider = preferred;
	last_audio_provider_selection_report = diagnostics;
	std::unique_ptr<AudioProvider> provider;
	aegisub::provider_catalog::AudioProviderOpenFailureReport open_failures;

	for (auto const& factory : sorted) {
		try {
			auto attempt = aegisub::provider_catalog::TryOpenProviderFactory(
				*factory,
				aegisub::provider_catalog::DescribeProviderFactoryEntry<AudioProviderCreate>,
				diagnostics,
				[&](Factory const& provider_factory) {
					return provider_factory.create(filename, br, choice_sink);
				});

			if (attempt.state == aegisub::provider_catalog::ProviderOpenAttemptState::Unavailable) {
				std::string err;
				err.append(factory->name);
				err.append(": ");
				err.append(attempt.unavailable_reason);
				LOG_D("audio_provider") << err;
				aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
					open_failures,
					diagnostics,
					factory->name,
					attempt.unavailable_reason,
					aegisub::provider_catalog::AudioProviderOpenAttemptFailure::Unavailable);
				continue;
			}
			if (attempt.state == aegisub::provider_catalog::ProviderOpenAttemptState::ReturnedNull)
				continue;

			provider = std::move(attempt.provider);
			LOG_I("audio_provider") << "Using audio provider: " << factory->name;
			break;
		}
		catch (UserCancelException const&) {
			last_audio_provider_selection_report = diagnostics;
			throw;
		}
		catch (fs::FileNotFound const& err) {
			LOG_D("audio_provider") << err.GetMessage();
			auto line_detail = err.GetMessage() + " not found.";
			aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				line_detail,
				aegisub::provider_catalog::AudioProviderOpenAttemptFailure::FileNotFound,
				nullptr,
				err.GetMessage());
		}
		catch (AudioDataNotFound const& err) {
			LOG_D("audio_provider") << err.GetMessage();
			aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				err.GetMessage(),
				aegisub::provider_catalog::AudioProviderOpenAttemptFailure::AudioNotFound);
		}
		catch (AudioProviderError const& err) {
			LOG_D("audio_provider") << err.GetMessage();
			aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				err.GetMessage(),
				aegisub::provider_catalog::AudioProviderOpenAttemptFailure::ProviderError);
		}
		catch (std::exception const& err) {
			LOG_W("audio_provider") << factory->name << " threw std::exception: " << err.what();
			aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				err.what(),
				aegisub::provider_catalog::AudioProviderOpenAttemptFailure::ProviderError,
				"std_exception");
		}
		catch (...) {
			LOG_W("audio_provider") << factory->name << " threw unknown exception";
			aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				"unknown exception",
				aegisub::provider_catalog::AudioProviderOpenAttemptFailure::ProviderError,
				"unknown_exception");
		}
	}

	last_audio_provider_selection_report = diagnostics;

	if (!provider) {
		switch (aegisub::provider_catalog::FinalAudioProviderOpenFailure(open_failures)) {
		case aegisub::provider_catalog::AudioProviderOpenFailure::ProviderError:
			throw AudioProviderError(aegisub::provider_catalog::FinalAudioProviderOpenErrorDetail(open_failures));
		case aegisub::provider_catalog::AudioProviderOpenFailure::AudioNotFound:
			throw AudioDataNotFound(aegisub::provider_catalog::FinalAudioProviderOpenErrorDetail(open_failures));
		case aegisub::provider_catalog::AudioProviderOpenFailure::FileNotFound:
			break;
		}
		throw fs::FileNotFound(filename);
	}

	source_needs_cache = provider->NeedsCache();

	// Give it a converter if needed
	if (provider->GetBytesPerSample() != 2 || provider->GetSampleRate() < 32000 || provider->GetChannels() != 1)
		provider = CreateConvertAudioProvider(std::move(provider));

	return provider;
}

std::unique_ptr<agi::AudioProvider> GetAudioProviderWithPreferred(fs::path const& filename,
                                                                  std::string const& preferred_provider,
                                                                  BackgroundRunner *br,
                                                                  std::shared_ptr<SingleChoiceInteractionSink> choice_sink,
                                                                  bool *source_needs_cache) {
	bool needs_cache = false;
	auto provider = OpenAudioProviderWithPreferred(filename, preferred_provider, br, std::move(choice_sink), needs_cache);
	if (source_needs_cache)
		*source_needs_cache = needs_cache;
	return CreateLockAudioProvider(std::move(provider));
}

std::unique_ptr<agi::AudioProvider> GetAudioProvider(fs::path const& filename,
                                                     Path const& path_helper,
                                                     BackgroundRunner *br,
                                                     NotificationSink& notification_sink,
                                                     std::shared_ptr<SingleChoiceInteractionSink> choice_sink) {
	bool needs_cache = false;
	auto provider = OpenAudioProviderWithPreferred(
		filename,
		GetConfiguredAudioProvider(),
		br,
		std::move(choice_sink),
		needs_cache);

	// Change provider to RAM/HD cache if needed
	int cache = GetConfiguredAudioCacheType();
	if (!cache || !needs_cache)
		return CreateLockAudioProvider(std::move(provider));

	// Convert to RAM
	if (cache == 1) {
		if (sizeof(void*) == 4 && (provider->GetNumSamples() * provider->GetChannels() * provider->GetBytesPerSample() >= (1 << 30))) {
			auto message = _(
				"Unable to create RAM audio cache: 32-bit memory limit exceeded. Fallback to hard disk cache.\n\n"
				"Possible solutions:\n"
				"- Use 64-bit version\n"
				"- Turn off cache or switch to hard disk cache in Preferences -> Advanced -> Audio -> Cache -> Cache type\n"
				"- Enable channel downmix in Preferences -> Advanced -> Audio"
			);
			notification_sink.ShowError(_("Out of Memory"), message);
			cache = 2;
		}
		else
			return CreateRAMAudioProvider(std::move(provider));
	}

	// Convert to HD
	if (cache == 2) {
		auto path = GetConfiguredHDAudioCacheLocation();
		if (path == "default")
			path = "?temp";
		auto cache_dir = path_helper.MakeAbsolute(path_helper.Decode(path), "?temp");
		return CreateHDAudioProvider(std::move(provider), cache_dir);
	}

	throw InternalError("Invalid audio caching method");
}

aegisub::provider_selection_diagnostics::SelectionReport GetLastAudioProviderSelectionReport() {
	return last_audio_provider_selection_report;
}

void ClearLastAudioProviderSelectionReport() {
	last_audio_provider_selection_report = {};
}
