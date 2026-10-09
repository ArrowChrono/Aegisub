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

#include "project.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "audio_controller.h"
#include "audio_provider_factory.h"
#include "charset_detect.h"
#include "compat.h"
#include "dialogs.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/video_provider.h"
#include "mkv_wrap.h"
#include "options.h"
#include "perf_trace.h"
#ifdef WITH_SCENECHANGE
#include "provider_index_cache.h"
#endif
#include "provider_selection_diagnostics.h"
#include "project_session_ops.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "transient_font_set.h"
#include "ui_services.h"
#include "include/aegisub/subtitles_provider.h"
#include "utils.h"
#include "video_memory_stats.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_session_ops.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/access.h>
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/keyframe.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>

namespace {
bool transient_font_environment_matches(std::shared_ptr<const TransientFontSet> const& left, std::shared_ptr<const TransientFontSet> const& right) {
	if (left == right)
		return true;

	auto const is_empty = [](std::shared_ptr<const TransientFontSet> const& fonts) {
		return !fonts || fonts->empty();
	};
	if (is_empty(left) && is_empty(right))
		return true;
	if (!left || !right)
		return false;
	return left->generation != 0 && left->generation == right->generation;
}

bool try_check_readable_media_path(agi::fs::path const& path, std::string& error) {
	error.clear();
	if (agi::IsNonFilesystemMediaPath(path))
		return true;

	try {
		agi::acs::CheckFileRead(path);
		return true;
	}
	catch (agi::fs::FileSystemError const& err) {
		error = err.GetMessage();
		return false;
	}
}

agi::ProjectUiStateSnapshot BuildSubtitleUiStateSnapshot(ProjectProperties const& properties) {
	agi::ProjectUiStateSnapshot snapshot;
	snapshot.subtitle_scroll_position = properties.scroll_position;
	return snapshot;
}

agi::ProjectUiStateSnapshot BuildVideoUiStateSnapshot(ProjectProperties const& properties) {
	agi::ProjectUiStateSnapshot snapshot;
	snapshot.video_zoom = properties.video_zoom;
	return snapshot;
}

void RestoreProjectUiState(agi::Context *context, agi::ProjectUiStateSnapshot const& state) {
	if (auto sink = context->GetProjectUiStateSink())
		sink->RestoreProjectUiState(state);
}

void ApplyPostOpenVideoPlan(agi::Context *context, aegisub::video_session_ops::PostOpenPlan const& plan) {
	auto video_controller = context->GetCore().videoController.get();
	if (plan.display_aspect_ratio_override)
		video_controller->SetAspectRatio(*plan.display_aspect_ratio_override);
	else
		video_controller->SetAspectRatio(AspectRatio::Default);
	video_controller->JumpToFrame(plan.initial_frame);
}

#ifdef WITH_SCENECHANGE
char const *kSceneChangeKeyframeCacheToken = "?local/scenechangekeyframes/";
char const *kSceneChangeKeyframeManifestName = "manifest.json";

struct SceneChangeKeyframeCacheManifestEntry {
	std::string keyframes_filename;
	std::string video_path;
	std::string video_name;
};

agi::fs::path GetSceneChangeKeyframeCacheFilename(agi::fs::path const& filename,
	std::string const& backend_cache_token) {
	return aegisub::provider_index_cache::BuildFilename(filename,
		kSceneChangeKeyframeCacheToken,
		".kf.txt",
		{ backend_cache_token });
}

agi::fs::path GetSceneChangeKeyframeCacheDirectory() {
	return aegisub::provider_index_cache::CacheDirectory(kSceneChangeKeyframeCacheToken);
}

// True when path is a file produced under the SceneChange keyframe cache dir.
// Note: CacheDirectory() may create the cache directory as a side effect.
bool IsSceneChangeKeyframeCachePath(agi::fs::path const& path) {
	if (path.empty())
		return false;

	auto const name = agi::fs::PathToString(path.filename());
	if (!name.ends_with(".kf.txt"))
		return false;

	try {
		auto const cache_dir = agi::fs::Canonicalize(GetSceneChangeKeyframeCacheDirectory());
		auto const file = agi::fs::Canonicalize(path);
		return file.parent_path() == cache_dir;
	}
	catch (...) {
		// Do not re-enter CacheDirectory() here: CreateDirectory / path decode
		// failures are a common reason we reached this catch, and rethrowing
		// would escape the Preferences option-changed callback.
		return false;
	}
}

agi::fs::path GetSceneChangeKeyframeManifestPath() {
	return GetSceneChangeKeyframeCacheDirectory() / kSceneChangeKeyframeManifestName;
}

std::mutex& SceneChangeKeyframeManifestMutex() {
	static std::mutex mutex;
	return mutex;
}

std::string JsonStringValue(json::Object const& object, char const *key) {
	auto it = object.find(key);
	if (it == object.end())
		return {};

	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (json::Exception const&) {
		return {};
	}
}

json::Array const& SceneChangeKeyframeManifestEntries(json::UnknownElement const& root) {
	try {
		return static_cast<json::Array const&>(root);
	}
	catch (json::Exception const&) {
		json::Object const& root_object = root;
		auto entries_it = root_object.find("entries");
		if (entries_it == root_object.end())
			throw;
		return static_cast<json::Array const&>(entries_it->second);
	}
}

std::vector<SceneChangeKeyframeCacheManifestEntry> LoadSceneChangeKeyframeManifest() {
	auto manifest_path = GetSceneChangeKeyframeManifestPath();
	if (!agi::fs::FileExists(manifest_path))
		return {};

	try {
		auto stream = agi::io::Open(manifest_path);
		json::UnknownElement root;
		json::Reader::Read(root, *stream);

		json::Array const& entries_array = SceneChangeKeyframeManifestEntries(root);
		std::vector<SceneChangeKeyframeCacheManifestEntry> entries;
		entries.reserve(entries_array.size());
		for (auto const& item : entries_array) {
			json::Object const& object = item;
			SceneChangeKeyframeCacheManifestEntry entry;
			entry.keyframes_filename = JsonStringValue(object, "keyframes");
			entry.video_path = JsonStringValue(object, "video_path");
			entry.video_name = JsonStringValue(object, "video_name");
			if (!entry.keyframes_filename.empty())
				entries.push_back(std::move(entry));
		}
		return entries;
	}
	catch (json::Exception const& err) {
		LOG_W("project/scenechange")
			<< "Ignoring invalid SceneChange keyframe manifest "
			<< agi::fs::PathToString(manifest_path)
			<< ": " << err.what();
	}
	catch (agi::Exception const& err) {
		LOG_W("project/scenechange")
			<< "Ignoring unreadable SceneChange keyframe manifest "
			<< agi::fs::PathToString(manifest_path)
			<< ": " << err.GetMessage();
	}
	catch (std::exception const& err) {
		LOG_W("project/scenechange")
			<< "Ignoring unreadable SceneChange keyframe manifest "
			<< agi::fs::PathToString(manifest_path)
			<< ": " << err.what();
	}

	return {};
}

void SaveSceneChangeKeyframeManifest(std::vector<SceneChangeKeyframeCacheManifestEntry> const& entries) {
	auto manifest_path = GetSceneChangeKeyframeManifestPath();

	try {
		json::Array root;
		root.reserve(entries.size());
		for (auto const& entry : entries) {
			json::Object object;
			object["keyframes"] = entry.keyframes_filename;
			object["video_path"] = entry.video_path;
			object["video_name"] = entry.video_name;
			root.push_back(std::move(object));
		}

		agi::JsonWriter::Write(root, agi::io::Save(manifest_path).Get());
	}
	catch (agi::Exception const& err) {
		LOG_W("project/scenechange")
			<< "Failed to write SceneChange keyframe manifest "
			<< agi::fs::PathToString(manifest_path)
			<< ": " << err.GetMessage();
	}
	catch (std::exception const& err) {
		LOG_W("project/scenechange")
			<< "Failed to write SceneChange keyframe manifest "
			<< agi::fs::PathToString(manifest_path)
			<< ": " << err.what();
	}
}

template<typename Update>
void UpdateSceneChangeKeyframeManifest(Update&& update) {
	std::lock_guard<std::mutex> lock(SceneChangeKeyframeManifestMutex());
	auto entries = LoadSceneChangeKeyframeManifest();
	if (update(entries))
		SaveSceneChangeKeyframeManifest(entries);
}

void PruneSceneChangeKeyframeManifest() {
	auto directory = aegisub::provider_index_cache::CacheDirectory(kSceneChangeKeyframeCacheToken);
	UpdateSceneChangeKeyframeManifest([&](auto& entries) {
		if (entries.empty())
			return false;

		auto const old_size = entries.size();
		entries.erase(
			std::remove_if(entries.begin(), entries.end(), [&](auto const& entry) {
				auto cache_filename = agi::fs::PathFromString(entry.keyframes_filename).filename();
				return cache_filename.empty() || !agi::fs::FileExists(directory / cache_filename);
			}),
			entries.end());
		return entries.size() != old_size;
	});
}

void RecordSceneChangeKeyframeCache(agi::fs::path const& cache_path, agi::fs::path const& video_path) {
	auto const keyframes_filename = agi::fs::PathToGenericString(cache_path.filename());
	auto const video_path_text = agi::fs::PathToGenericString(video_path);
	auto const video_name = agi::fs::PathToString(video_path.filename());

	UpdateSceneChangeKeyframeManifest([&](auto& entries) {
		auto existing = std::find_if(entries.begin(), entries.end(), [&](auto const& entry) {
			return entry.keyframes_filename == keyframes_filename;
		});

		if (existing == entries.end())
			entries.push_back({ keyframes_filename, video_path_text, video_name });
		else if (existing->video_path != video_path_text || existing->video_name != video_name)
			*existing = { keyframes_filename, video_path_text, video_name };
		else
			return false;
		return true;
	});
}

void CleanSceneChangeKeyframeCache() {
	aegisub::provider_index_cache::Clean(kSceneChangeKeyframeCacheToken,
		"*.kf.txt",
		"Provider/SceneChange/Cache/Size",
		"Provider/SceneChange/Cache/Files",
		[] { PruneSceneChangeKeyframeManifest(); });
}

void RemoveSceneChangeKeyframeCacheFile(agi::fs::path const& path) {
	try {
		if (!path.empty())
			agi::fs::Remove(path);
		PruneSceneChangeKeyframeManifest();
	}
	catch (...) {
	}
}
#endif
}

Project::Project(agi::Context *c) : context(c) {
	option_connections = agi::signal::make_vector({
		OPT_SUB("Audio/Cache/Type", &Project::ReloadAudio, this),
		OPT_SUB("Audio/Provider", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Audio/FFmpegSource/Decode Error Handling", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Audio/FFmpegSource/Downmix", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Audio/LsmasNative/Downmix", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Avisynth/Allow Ancient", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Avisynth/Allow Ancient", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Avisynth/Memory Max", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Avisynth/Memory Max", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Avisynth/Runtime Path", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Avisynth/Runtime Path", &Project::ReloadAudio, this),
		OPT_SUB("Provider/Video/FFmpegSource/Decoding Threads", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Video/FFmpegSource/Unsafe Seeking", &Project::ReloadVideo, this),
		OPT_SUB("Provider/Video/LsmasNative/Decoding Threads", &Project::ReloadVideo, this),
#ifdef WITH_SCENECHANGE
		// Backend choice only changes SceneChange keyframe generation and its
		// cache key — reopening the video would re-run LsmasNative indexing.
		OPT_SUB("Provider/SceneChange/Backend", &Project::ReloadSceneChangeKeyframes, this),
#endif
		OPT_SUB("Subtitle/Provider", &Project::ReloadSubtitlesProvider, this),
		OPT_SUB("Video/Provider", &Project::ReloadVideo, this),
	});
}

Project::~Project() {
	// Quiesce any outstanding raw-video lease before members unwind; tasks
	// observe the retirement flag and release within one batch.
	BeginVideoProviderRetirement();
	WaitForLeaseDrain();
}

bool Project::CanLoadBitmapSubtitlesFromVideo(MkvBitmapSubtitleCodec codec) const {
	switch (codec) {
	case MkvBitmapSubtitleCodec::HdmvPgs:
		return video_has_pgs_subtitles;
	case MkvBitmapSubtitleCodec::VobSub:
		return video_has_vobsub_subtitles;
	case MkvBitmapSubtitleCodec::Unsupported:
		break;
	}
	return false;
}

void Project::UpdateRelativePaths() {
	auto core = context->GetCore();
	core.ass->Properties.audio_file     = agi::fs::PathToGenericString(core.path->MakeRelative(audio_file, "?script"));
	core.ass->Properties.video_file     = agi::fs::PathToGenericString(core.path->MakeRelative(video_file, "?script"));
	core.ass->Properties.timecodes_file = agi::fs::PathToGenericString(core.path->MakeRelative(timecodes_file, "?script"));
	core.ass->Properties.keyframes_file = agi::fs::PathToGenericString(core.path->MakeRelative(keyframes_file, "?script"));
}

void Project::ReloadAudio() {
	if (audio_provider)
		LoadAudio(audio_file);
}

void Project::RefreshSubtitlesProvider(bool recreate_provider) {
	if (!video_provider)
		return;

	auto core = context->GetCore();
	try {
		if (recreate_provider) {
			video_provider->ReplaceSubtitlesProvider(SubtitlesProviderFactory::GetProvider({
				GetProgressRunner(),
				core.ass->GetTransientFonts()
			}));
		}
		video_provider->SetSubtitlesTimecodes(timecodes);
		video_provider->LoadSubtitles(core.ass.get());
		core.videoController->JumpToFrame(core.videoController->GetFrameN());
	}
	catch (agi::UserCancelException const&) {
	}
	catch (std::string const& err) {
		ShowError(err);
	}
	catch (agi::Exception const& err) {
		ShowError(err.GetMessage());
	}
	catch (...) {
		ShowError(std::string("Failed to reload subtitles provider."));
	}
}

void Project::ReloadSubtitlesProvider() {
	RefreshSubtitlesProvider(true);
}

void Project::RefreshVideoFrameForTimecodesChange() {
	if (!video_provider)
		return;

	video_provider->SetSubtitlesTimecodes(timecodes);
	auto core = context->GetCore();
	core.videoController->JumpToFrame(core.videoController->GetFrameN());
}

void Project::ReloadVideo() {
	if (video_provider) {
		DoLoadVideo(video_file);
		auto core = context->GetCore();
		core.videoController->JumpToFrame(core.videoController->GetFrameN());
	}
}

agi::BackgroundRunner *Project::GetProgressRunner(std::string const& title, std::string const& message) {
	if (!progress_runner)
		progress_runner = context->CreateBackgroundRunner(title, message);
	return progress_runner.get();
}

void Project::ShowError(wxString const& message, std::string const& title) {
	context->ShowError(from_wx(message), title);
}

void Project::ShowError(std::string const& message, std::string const& title) {
	context->ShowError(message, title);
}

void Project::ShowWarning(std::string const& message, std::string const& title) {
	context->ShowWarning(message, title);
}

void Project::SetPath(agi::fs::path& var, const char *token, const char *mru, agi::fs::path const& value) {
	auto core = context->GetCore();
	var = value;
	if (*token)
		core.path->SetToken(token, value);
	if (*mru)
		config::mru->Add(mru, value);
	UpdateRelativePaths();
}

bool Project::DoLoadSubtitles(agi::fs::path const& path, std::string encoding, ProjectProperties &properties, bool is_reload) {
	auto core = context->GetCore();
	auto const previous_transient_fonts = core.ass->GetTransientFonts();
	auto remove_mru = [](char const* category, agi::fs::path const& candidate) {
		config::mru->Remove(category, candidate);
	};

	auto resolved_encoding = aegisub::project_session_ops::ResolveSubtitleEncoding(
		path,
		std::move(encoding),
		[&] { return CharSetDetect::GetEncoding(path, context->GetSingleChoiceInteractionSink()); },
		*context->GetNotificationSink(),
		remove_mru);
	if (!resolved_encoding)
		return false;
	encoding = *resolved_encoding;

	if (encoding != "binary") {
		// Try loading as timecodes and keyframes first since we can't
		// distinguish them based on filename alone, and just ignore failures
		// rather than trying to differentiate between malformed timecodes
		// files and things that aren't timecodes files at all
		try { DoLoadTimecodes(path); return false; } catch (...) { }
		try { DoLoadKeyframes(path); return false; } catch (...) { }
	}

	if (!aegisub::project_session_ops::LoadSubtitlesWithErrorHandling(
		path,
		[&] { properties = core.subsController->Load(path, encoding, is_reload); },
		*context->GetNotificationSink(),
		remove_mru)) {
		return false;
	}

	Selection sel;
	AssDialogue *active_line = nullptr;
	if (!core.ass->Events.empty()) {
		int row = mid<int>(0, properties.active_row, core.ass->Events.size() - 1);
		active_line = &*std::next(core.ass->Events.begin(), row);
		sel.insert(active_line);
	}
	core.selectionController->SetSelectionAndActive(std::move(sel), active_line);
	core.selectionController->ClearSelectionHistory();
	core.selectionController->ClearSelectionAnchor();
	RestoreProjectUiState(context, BuildSubtitleUiStateSnapshot(properties));

	if (video_provider)
		RefreshSubtitlesProvider(!transient_font_environment_matches(previous_transient_fonts, core.ass->GetTransientFonts()));

	return true;
}

void Project::LoadSubtitles(agi::fs::path path, std::string encoding, bool load_linked) {
	ProjectProperties properties;
	if (DoLoadSubtitles(path, encoding, properties) && load_linked)
		LoadUnloadFiles(properties);
}

bool Project::ReloadSubtitles(agi::fs::path path, std::string encoding, bool load_linked, bool is_reload) {
	ProjectProperties properties;
	if (!DoLoadSubtitles(path, encoding, properties, is_reload))
		return false;

	if (load_linked)
		LoadUnloadFiles(properties);
	return true;
}

void Project::CloseSubtitles() {
	auto core = context->GetCore();
	auto const previous_transient_fonts = core.ass->GetTransientFonts();

	core.subsController->Close();
	core.path->SetToken("?script", "");
	LoadUnloadFiles(core.ass->Properties);
	auto line = &*core.ass->Events.begin();
	core.selectionController->SetSelectionAndActive({line}, line);
	core.selectionController->ClearSelectionHistory();
	core.selectionController->ClearSelectionAnchor();
	if (video_provider)
		RefreshSubtitlesProvider(!transient_font_environment_matches(previous_transient_fonts, core.ass->GetTransientFonts()));
}

void Project::LoadUnloadFiles(ProjectProperties properties) {
	auto load_linked = OPT_GET("App/Auto/Load Linked Files")->GetInt();
	if (!load_linked) return;

	auto core = context->GetCore();
	auto audio     = core.path->MakeAbsolute(properties.audio_file, "?script");
	auto video     = core.path->MakeAbsolute(properties.video_file, "?script");
	auto timecodes = core.path->MakeAbsolute(properties.timecodes_file, "?script");
	auto keyframes = core.path->MakeAbsolute(properties.keyframes_file, "?script");

	if (video == video_file && audio == audio_file && keyframes == keyframes_file && timecodes == timecodes_file)
		return;

	if (load_linked == 2) {
		std::string message = from_wx(_("Do you want to load/unload the associated files?"));
		message += "\n";

		auto append_file = [&](agi::fs::path const& p, wxString const& unload, wxString const& load) {
			message += "\n";
			if (p.empty())
				message += from_wx(unload);
			else
				message += agi::format(load, p);
		};

		if (audio != audio_file)
			append_file(audio, _("Unload audio"), _("Load audio file: %s"));
		if (video != video_file)
			append_file(video, _("Unload video"), _("Load video file: %s"));
		if (timecodes != timecodes_file)
			append_file(timecodes, _("Unload timecodes"), _("Load timecodes file: %s"));
		if (keyframes != keyframes_file)
			append_file(keyframes, _("Unload keyframes"), _("Load keyframes file: %s"));

		if (context->RequestInteraction({
			from_wx(_("(Un)Load files?")),
			message,
			agi::InteractionButtons::YesNo,
			agi::InteractionIcon::Question
		}) != agi::InteractionResult::Yes)
			return;
	}

	bool loaded_video = false;
	aegisub::video_session_ops::OpenedVideoSummary opened_video_summary;
	bool skip_duplicate_audio_error = false;
	if (video != video_file) {
		if (video.empty())
			CloseVideo();
		else {
			loaded_video = DoLoadVideo(video, &opened_video_summary);
			if (loaded_video) {
				auto vc = core.videoController.get();
				vc->JumpToFrame(properties.video_position);

				auto ar_mode = static_cast<AspectRatio>(properties.ar_mode);
				if (ar_mode == AspectRatio::Custom)
					vc->SetAspectRatio(properties.ar_value);
				else
					vc->SetAspectRatio(ar_mode);
				RestoreProjectUiState(context, BuildVideoUiStateSnapshot(properties));
			}
			else if (audio == video) {
				std::string ignored_error;
				skip_duplicate_audio_error = !try_check_readable_media_path(video, ignored_error);
			}
		}
	}

	if (!timecodes.empty()) LoadTimecodes(timecodes);
	if (!keyframes.empty()) LoadKeyframes(keyframes);

	if (audio != audio_file) {
		if (audio.empty())
			CloseAudio();
		else if (!skip_duplicate_audio_error)
			DoLoadAudio(audio, false);
	}
	else if (loaded_video && aegisub::video_session_ops::PlanPostOpen(
		opened_video_summary,
		OPT_GET("Video/Open Audio")->GetBool(),
		audio_file,
		video_file).auto_load_linked_audio)
		DoLoadAudio(video, true);
}

void Project::DoLoadAudio(agi::fs::path const& path, bool quiet) {
	auto remove_mru = [](char const* category, agi::fs::path const& candidate) {
		config::mru->Remove(category, candidate);
	};

	std::string access_error;
	if (!try_check_readable_media_path(path, access_error)) {
		aegisub::project_session_ops::HandleUnreadableAudioOpenPath(
			path,
			access_error,
			*context->GetNotificationSink(),
			remove_mru);
		return;
	}

	audio_provider = aegisub::project_session_ops::CreateAudioProviderWithErrorHandling(
		path,
		quiet,
		[&]() {
			auto core = context->GetCore();
			return GetAudioProvider(path, *core.path, GetProgressRunner(), *context->GetNotificationSink(), context->GetSingleChoiceInteractionSink());
		},
		*context->GetNotificationSink(),
		[&](std::string const& error) {
			LOG_D("video/open/audio") << "File " << agi::fs::PathToString(video_file) << " has no audio data: " << error;
		},
		remove_mru);
	if (!audio_provider)
	{
		auto const report = GetLastAudioProviderSelectionReport();
		LOG_W("project/audio") << "failed to open audio path=" << agi::fs::PathToString(path)
			<< " preferred_provider=" << report.preferred_provider
			<< " selected_provider=" << (report.selected_provider.empty() ? std::string("<none>") : report.selected_provider)
			<< " attempts=" << aegisub::provider_selection_diagnostics::FormatAttempts(report);
		return;
	}

	SetPath(audio_file, "?audio", "Audio", path);
	if (perf_trace::ShouldSampleVideoMemory(true)) {
		VideoMemorySnapshot snapshot;
		if (video_provider)
			snapshot.async = video_provider->CollectMemoryStats();
		if (auto video_display = context->GetUI().videoDisplay)
			snapshot.display = video_display->CollectMemoryStats();
		snapshot.audio = audio_provider->GetMemoryStats();
		perf_trace::ObserveVideoMemorySnapshot("audio_open", snapshot, true);
	}
	AnnounceAudioProviderModified(audio_provider.get());
}

void Project::LoadAudio(agi::fs::path path) {
	DoLoadAudio(path, false);
}

void Project::CloseAudio() {
	AnnounceAudioProviderModified(nullptr);
	audio_provider.reset();
	SetPath(audio_file, "?audio", "", "");
}

bool Project::DoLoadVideo(agi::fs::path const& path, aegisub::video_session_ops::OpenedVideoSummary* summary) {
	std::string access_error;
	if (!try_check_readable_media_path(path, access_error)) {
		return aegisub::video_session_ops::HandleUnreadableVideoOpenPath(
			path,
			access_error,
			*context->GetNotificationSink(),
			[](char const* category, agi::fs::path const& candidate) {
				config::mru->Remove(category, candidate);
			});
	}

	auto const load_started = std::chrono::steady_clock::now();
	can_generate_scene_change_keyframes = false;
	// Candidate-first: the new provider is built into a local object so a
	// failed open leaves the previous provider (and any session state)
	// untouched.
	auto candidate = aegisub::video_session_ops::CreateVideoProviderWithErrorHandling(
		path,
		[&] {
			auto core = context->GetCore();
			auto old_matrix = core.ass->GetScriptInfo("YCbCr Matrix");
			auto event_sink = core.videoController->CreateAsyncVideoProviderEventSink();
			return agi::make_unique<AsyncVideoProvider>(
				path,
				old_matrix,
				std::move(event_sink),
				GetProgressRunner(),
				core.ass->GetTransientFonts(),
				context->GetSingleChoiceInteractionSink());
		},
		*context->GetNotificationSink(),
		[](char const* category, agi::fs::path const& candidate) {
			config::mru->Remove(category, candidate);
		});
	if (!candidate)
		return false;

	if (perf_trace::ShouldSampleVideoMemory(true)) {
		VideoMemorySnapshot snapshot;
		snapshot.async = candidate->CollectMemoryStats();
		if (audio_provider)
			snapshot.audio = audio_provider->GetMemoryStats();
		perf_trace::ObserveVideoMemorySnapshot("video_provider_ready", snapshot, true);
	}

	MkvSubtitleAvailability subtitle_availability;
	auto opened_video = aegisub::video_session_ops::BuildOpenedVideoSummary(
		*candidate,
		path,
		[&](agi::fs::path const& candidate) {
			subtitle_availability = MatroskaWrapper::GetSubtitleAvailability(candidate);
			return subtitle_availability.text;
		});

	timecodes_file.clear();
	keyframes_file.clear();
	// Video-open listeners read Project::VideoName() and
	// CanLoadSubtitlesFromVideo(), so publish the new path and the
	// embedded-subtitles flag before notifying them.
	SetPath(video_file, "?video", "Video", path);
	video_has_subtitles = opened_video.has_subtitles;
	video_has_bitmap_subtitles = subtitle_availability.bitmap;
	video_has_pgs_subtitles = subtitle_availability.hdmv_pgs;
	video_has_vobsub_subtitles = subtitle_availability.vobsub;

	// Raw-video lifecycle barrier: deny new leases and wait for outstanding
	// analysis batches before swapping providers. Candidate construction and
	// all user interaction already happened above, so this wait is bounded
	// by at most the current batch.
	BeginVideoProviderRetirement();
	WaitForLeaseDrain();
	video_provider = std::move(candidate);
	CompleteVideoProviderRetirement();

	AnnounceVideoProviderModified(video_provider.get());

	auto core = context->GetCore();
	UpdateVideoProperties(context, core.ass.get(), video_provider.get());
	video_provider->LoadSubtitles(core.ass.get());
	if (perf_trace::ShouldSampleVideoMemory(true)) {
		VideoMemorySnapshot snapshot;
		snapshot.async = video_provider->CollectMemoryStats();
		if (audio_provider)
			snapshot.audio = audio_provider->GetMemoryStats();
		perf_trace::ObserveVideoMemorySnapshot("video_subtitles_bound", snapshot, true);
	}

	timecodes = opened_video.timecodes;
	keyframes = opened_video.keyframes;
	video_provider->SetSubtitlesTimecodes(timecodes);
#ifdef WITH_SCENECHANGE
	std::optional<scenechange::KeyframeRequest> scenechange_request;
	if (video_provider->GetDecoderName() == "LsmasNative")
		scenechange_request = video_provider->PrepareSceneChangeKeyframes();
	can_generate_scene_change_keyframes = scenechange_request.has_value();
	bool scenechange_keyframes_loaded = scenechange_request &&
										TryLoadSceneChangeKeyframes(path, true, *scenechange_request);
#else
	bool scenechange_keyframes_loaded = false;
#endif

	std::string warning = opened_video.warning;
	if (!warning.empty())
		ShowWarning(warning, "Warning");

	if (summary)
		*summary = opened_video;

	if (!scenechange_keyframes_loaded)
		AnnounceKeyframesModified(keyframes);
	AnnounceTimecodesModified(timecodes);
	auto const duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_started).count();
	perf_trace::TraceVideoOpen(path, video_provider->GetWidth(), video_provider->GetHeight(), video_provider->GetFrameCount(), video_provider->HasAudio(), video_provider->GetDecoderName(), duration_ms);
	if (perf_trace::ShouldSampleVideoMemory(true)) {
		VideoMemorySnapshot snapshot;
		snapshot.async = video_provider->CollectMemoryStats();
		if (audio_provider)
			snapshot.audio = audio_provider->GetMemoryStats();
		perf_trace::ObserveVideoMemorySnapshot("video_open", snapshot, true);
	}
	return true;
}

void Project::LoadVideo(agi::fs::path path) {
	if (path.empty()) return;
	aegisub::video_session_ops::OpenedVideoSummary opened_video;
	if (!DoLoadVideo(path, &opened_video)) return;
	auto plan = aegisub::video_session_ops::PlanPostOpen(
		opened_video,
		OPT_GET("Video/Open Audio")->GetBool(),
		audio_file,
		video_file);
	if (plan.auto_load_linked_audio)
		DoLoadAudio(video_file, true);
	ApplyPostOpenVideoPlan(context, plan);
}

void Project::CloseVideo() {
	auto core = context->GetCore();
	// Clear state before notifying so listeners see the closed video.
	video_has_subtitles = false;
	video_has_bitmap_subtitles = false;
	video_has_pgs_subtitles = false;
	video_has_vobsub_subtitles = false;
	AnnounceVideoProviderModified(nullptr);
	// Same barrier as DoLoadVideo: quiesce outstanding analysis batches
	// before destroying the provider.
	BeginVideoProviderRetirement();
	WaitForLeaseDrain();
	video_provider.reset();
	CompleteVideoProviderRetirement();
	can_generate_scene_change_keyframes = false;
	SetPath(video_file, "?video", "", "");
	core.ass->Properties.ar_mode = 0;
	core.ass->Properties.ar_value = 0.0;
	core.ass->Properties.video_position = 0;
}

void Project::DoLoadTimecodes(agi::fs::path const& path) {
	timecodes = agi::vfr::Framerate(path);
	SetPath(timecodes_file, "", "Timecodes", path);
	RefreshVideoFrameForTimecodesChange();
	AnnounceTimecodesModified(timecodes);
}

void Project::LoadTimecodes(agi::fs::path path) {
	try {
		DoLoadTimecodes(path);
	}
	catch (agi::fs::FileSystemError const& e) {
		ShowError(e.GetMessage());
		config::mru->Remove("Timecodes", path);
	}
	catch (agi::vfr::Error const& e) {
		ShowError(agi::format("Failed to parse timecodes file: %s", e.GetMessage()));
		config::mru->Remove("Timecodes", path);
	}
}

void Project::CloseTimecodes() {
	timecodes = video_provider ? video_provider->GetFPS() : agi::vfr::Framerate{};
	SetPath(timecodes_file, "", "", "");
	RefreshVideoFrameForTimecodesChange();
	AnnounceTimecodesModified(timecodes);
}

#ifdef WITH_SCENECHANGE
bool Project::TryLoadSceneChangeKeyframes(agi::fs::path const& video_path,
										  bool prompt_if_missing,
										  scenechange::KeyframeRequest const& request) {
	if (!video_provider || video_provider->GetDecoderName() != "LsmasNative")
		return false;

	auto cache_path = GetSceneChangeKeyframeCacheFilename(video_path, request.GetCacheToken());
	if (agi::fs::FileExists(cache_path)) {
		try {
			DoLoadKeyframes(cache_path);
			agi::fs::Touch(cache_path);
			RecordSceneChangeKeyframeCache(cache_path, video_path);
			CleanSceneChangeKeyframeCache();
			return true;
		}
		catch (agi::Exception const& err) {
			LOG_W("project/scenechange")
				<< "Ignoring invalid SceneChange keyframe cache "
				<< agi::fs::PathToString(cache_path)
				<< ": " << err.GetMessage();
			RemoveSceneChangeKeyframeCacheFile(cache_path);
		}
		catch (std::exception const& err) {
			LOG_W("project/scenechange")
				<< "Ignoring invalid SceneChange keyframe cache "
				<< agi::fs::PathToString(cache_path)
				<< ": " << err.what();
			RemoveSceneChangeKeyframeCacheFile(cache_path);
		}
	}

	if (!prompt_if_missing || !CanGenerateSceneChangeKeyframes())
		return false;

	return PromptAndGenerateSceneChangeKeyframes(request, cache_path, false);
}

bool Project::PromptAndGenerateSceneChangeKeyframes(scenechange::KeyframeRequest const& request,
													agi::fs::path const& cache_path, bool cache_exists) {
	if (!video_provider || !CanGenerateSceneChangeKeyframes())
		return false;

	auto answer = context->RequestInteraction({
		from_wx(_("Generate keyframes?")),
		from_wx(cache_exists
			? _("A cached SceneChange keyframe file already exists for this video.\n\nRegenerating it may use a lot of CPU and take a long time. Regenerate it now?")
			: _("No cached SceneChange keyframe file was found for this video.\n\nGenerating it may use a lot of CPU and take a long time. Generate it now?")),
		agi::InteractionButtons::YesNo,
		agi::InteractionIcon::Question
	});
	if (answer != agi::InteractionResult::Yes)
		return false;

	try {
		video_provider->GenerateSceneChangeKeyframes(request, cache_path,
													 GetProgressRunner("Generating keyframes", "Scanning scene changes"));
		RecordSceneChangeKeyframeCache(cache_path, video_file);
		CleanSceneChangeKeyframeCache();
		DoLoadKeyframes(cache_path);
		return true;
	}
	catch (agi::UserCancelException const&) {
		return false;
	}
	catch (agi::Exception const& err) {
		ShowError(err.GetMessage(), "Error generating keyframes");
	}
	catch (std::exception const& err) {
		ShowError(err.what(), "Error generating keyframes");
	}

	return false;
}

void Project::ReloadSceneChangeKeyframes() {
	if (!video_provider || video_file.empty())
		return;

	// SceneChange backend is only meaningful for LsmasNative; other providers
	// must not lose manually loaded keyframe files when this preference changes.
	if (video_provider->GetDecoderName() != "LsmasNative")
		return;

	auto const request = video_provider->PrepareSceneChangeKeyframes();
	can_generate_scene_change_keyframes = request.has_value();

	if (!keyframes_file.empty() && !IsSceneChangeKeyframeCachePath(keyframes_file))
		return;

	// Preference Apply must not prompt or start a full-file scan. Only adopt an
	// existing backend-specific cache; generation stays on the menu command.
	if (request && TryLoadSceneChangeKeyframes(video_file, false, *request))
		return;

	// Drop only SceneChange-sourced keyframes that no longer match the selected
	// backend. Leave user-opened .kf.txt files alone.
	if (IsSceneChangeKeyframeCachePath(keyframes_file))
		CloseKeyframes();
}
#endif

bool Project::CanGenerateSceneChangeKeyframes() const {
	return can_generate_scene_change_keyframes;
}

bool Project::GenerateSceneChangeKeyframes() {
#ifdef WITH_SCENECHANGE
	if (video_file.empty() || !video_provider || !CanGenerateSceneChangeKeyframes())
		return false;

	auto const request = video_provider->PrepareSceneChangeKeyframes();
	if (!request)
		return false;

	auto cache_path = GetSceneChangeKeyframeCacheFilename(video_file, request->GetCacheToken());
	return PromptAndGenerateSceneChangeKeyframes(*request, cache_path, agi::fs::FileExists(cache_path));
#else
	return false;
#endif
}

void Project::DoLoadKeyframes(agi::fs::path const& path) {
	keyframes = agi::keyframe::Load(path);
	SetPath(keyframes_file, "", "Keyframes", path);
	AnnounceKeyframesModified(keyframes);
}

void Project::LoadKeyframes(agi::fs::path path) {
	try {
		DoLoadKeyframes(path);
	}
	catch (agi::fs::FileSystemError const& e) {
		ShowError(e.GetMessage());
		config::mru->Remove("Keyframes", path);
	}
	catch (agi::keyframe::Error const& e) {
		ShowError(agi::format("Failed to parse keyframes file: %s", e.GetMessage()));
		config::mru->Remove("Keyframes", path);
	}
}

void Project::CloseKeyframes() {
	keyframes = video_provider ? video_provider->GetKeyFrames() : std::vector<int>{};
	SetPath(keyframes_file, "", "", "");
	AnnounceKeyframesModified(keyframes);
}

void Project::LoadList(std::vector<agi::fs::path> const& files) {
	// Keep these lists sorted

	// Video formats
	const char *videoList[] = {
		".asf",
		".avi",
		".avs",
		".d2v",
		".h264",
		".hevc",
		".m2ts",
		".m4v",
		".mkv",
		".mov",
		".mp4",
		".mpeg",
		".mpg",
		".ogm",
		".rm",
		".rmvb",
		".ts",
		".webm"
		".wmv",
		".y4m",
		".yuv"
	};

	// Subtitle formats
	const char *subsList[] = {
		".ass",
		".srt",
		".ssa",
		".sub",
		".ttxt"
	};

	// Audio formats
	const char *audioList[] = {
		".aac",
		".ac3",
		".ape",
		".dts",
		".eac3",
		".flac",
		".m4a",
		".mka",
		".mp3",
		".ogg",
		".opus",
		".w64",
		".wav",
		".wma"
	};

	auto search = [](const char **begin, const char **end, std::string const& str) {
		return std::binary_search(begin, end, str.c_str(), [](const char *a, const char *b) {
			return strcmp(a, b) < 0;
		});
	};

	agi::fs::path audio, video, subs, timecodes, keyframes;
	for (auto file : files) {
		if (file.is_relative()) file = absolute(file);
		if (!agi::fs::FileExists(file)) continue;

		auto ext = agi::fs::PathToString(file.extension());
		agi::util::strings::to_lower_inplace(ext);

		// Could be subtitles, keyframes or timecodes, so try loading as each
		if (ext == ".txt" || ext == ".log") {
			if (timecodes.empty()) {
				try {
					DoLoadTimecodes(file);
					timecodes = file;
					continue;
				} catch (...) { }
			}

			if (keyframes.empty()) {
				try {
					DoLoadKeyframes(file);
					keyframes = file;
					continue;
				} catch (...) { }
			}

			if (subs.empty() && ext != ".log")
				subs = file;
			continue;
		}

		if (subs.empty() && search(std::begin(subsList), std::end(subsList), ext))
			subs = file;
		if (video.empty() && search(std::begin(videoList), std::end(videoList), ext))
			video = file;
		if (audio.empty() && search(std::begin(audioList), std::end(audioList), ext))
			audio = file;
	}

	ProjectProperties properties;
	if (!subs.empty()) {
		if (!DoLoadSubtitles(subs, "", properties))
			subs.clear();
	}

	if (!audio.empty())
		DoLoadAudio(audio, false);

	aegisub::video_session_ops::OpenedVideoSummary opened_video;
	if (!video.empty() && DoLoadVideo(video, &opened_video)) {
		auto plan = aegisub::video_session_ops::PlanPostOpen(
			opened_video,
			OPT_GET("Video/Open Audio")->GetBool(),
			audio_file,
			video_file);
		ApplyPostOpenVideoPlan(context, plan);

		// We loaded these earlier, but loading video unloaded them
		// Non-Do version of Load in case they've vanished or changed between
		// then and now
		if (!timecodes.empty())
			LoadTimecodes(timecodes);
		if (!keyframes.empty())
			LoadKeyframes(keyframes);

		// Load audio from video
		if (audio.empty() && plan.auto_load_linked_audio)
			DoLoadAudio(video_file, true);
	}

	if (!subs.empty())
		LoadUnloadFiles(properties);
}
