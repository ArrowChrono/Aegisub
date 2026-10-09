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

#include <libaegisub/fs_fwd.h>
#include <libaegisub/signal.h>
#include <libaegisub/vfr.h>

#include "motion_track/video_provider_lease.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class AsyncVideoProvider;
class wxString;
namespace agi { class AudioProvider; }
namespace agi { class BackgroundRunner; }
namespace agi { struct Context; }
namespace aegisub::video_session_ops { struct OpenedVideoSummary; }
namespace scenechange {
class KeyframeRequest;
}
enum class MkvBitmapSubtitleCodec;
struct ProjectProperties;

class Project {
	std::unique_ptr<agi::AudioProvider> audio_provider;
	std::unique_ptr<AsyncVideoProvider> video_provider;
	std::shared_ptr<aegisub::motion_track::VideoProviderLeaseState> video_lease
	    = aegisub::motion_track::VideoProviderLeaseState::Create();
	agi::vfr::Framerate timecodes;
	std::vector<int> keyframes;

	agi::fs::path audio_file;
	agi::fs::path video_file;
	agi::fs::path timecodes_file;
	agi::fs::path keyframes_file;

	agi::signal::Signal<agi::AudioProvider *> AnnounceAudioProviderModified;
	agi::signal::Signal<AsyncVideoProvider *> AnnounceVideoProviderModified;
	agi::signal::Signal<agi::vfr::Framerate const&> AnnounceTimecodesModified;
	agi::signal::Signal<std::vector<int> const&> AnnounceKeyframesModified;
	std::vector<agi::signal::Connection> option_connections;

	bool video_has_subtitles = false;
	bool video_has_bitmap_subtitles = false;
	bool video_has_pgs_subtitles = false;
	bool video_has_vobsub_subtitles = false;
	bool can_generate_scene_change_keyframes = false;
	std::unique_ptr<agi::BackgroundRunner> progress_runner;
	agi::Context *context = nullptr;

	agi::BackgroundRunner *GetProgressRunner(std::string const& title = "", std::string const& message = "");
	void ShowError(wxString const& message, std::string const& title = "Error loading file");
	void ShowError(std::string const& message, std::string const& title = "Error loading file");
	void ShowWarning(std::string const& message, std::string const& title = "Warning");

	bool DoLoadSubtitles(agi::fs::path const& path, std::string encoding, ProjectProperties &properties, bool is_reload = false);
	void DoLoadAudio(agi::fs::path const& path, bool quiet);
	bool DoLoadVideo(agi::fs::path const& path, aegisub::video_session_ops::OpenedVideoSummary* summary = nullptr);
	void DoLoadTimecodes(agi::fs::path const& path);
	void DoLoadKeyframes(agi::fs::path const& path);
#ifdef WITH_SCENECHANGE
	bool TryLoadSceneChangeKeyframes(agi::fs::path const& video_path,
									 bool prompt_if_missing, scenechange::KeyframeRequest const& request);
	bool PromptAndGenerateSceneChangeKeyframes(scenechange::KeyframeRequest const& request,
											   agi::fs::path const& cache_path, bool cache_exists);
	// SceneChange backend only affects keyframe generation/cache paths; do not
	// reopen the video provider (that would re-index LsmasNative unnecessarily).
	// Preference changes only load an existing cache — generation is via menu.
	void ReloadSceneChangeKeyframes();
#endif

	void LoadUnloadFiles(ProjectProperties properties);
	void UpdateRelativePaths();
	void RefreshSubtitlesProvider(bool recreate_provider);
	void RefreshVideoFrameForTimecodesChange();
	void ReloadAudio();
	void ReloadVideo();

	void SetPath(agi::fs::path& var, const char *token, const char *mru, agi::fs::path const& value);

public:
	Project(agi::Context *context);
	~Project();

	void LoadSubtitles(agi::fs::path path, std::string encoding="", bool load_linked=true);
	bool ReloadSubtitles(agi::fs::path path, std::string encoding="", bool load_linked=false, bool is_reload=true);
	void CloseSubtitles();
	bool CanLoadSubtitlesFromVideo() const { return video_has_subtitles; }
	bool CanLoadBitmapSubtitlesFromVideo() const { return video_has_bitmap_subtitles; }
	bool CanLoadBitmapSubtitlesFromVideo(MkvBitmapSubtitleCodec codec) const;
	bool CanLoadSecondarySubtitlesFromVideo() const { return video_has_subtitles || video_has_bitmap_subtitles; }

	void LoadAudio(agi::fs::path path);
	void CloseAudio();
	agi::AudioProvider *AudioProvider() const { return audio_provider.get(); }
	agi::fs::path const& AudioName() const { return audio_file; }

	void LoadVideo(agi::fs::path path);
	void CloseVideo();
	AsyncVideoProvider *VideoProvider() const { return video_provider.get(); }
	agi::fs::path const& VideoName() const { return video_file; }

	/// Raw-video lifecycle: acquire before running an analysis batch task;
	/// retirement denies new leases and flags outstanding ones. The Project
	/// swaps/destroys the provider only after draining outstanding leases.
	aegisub::motion_track::VideoProviderLeaseState::Handle
	AcquireVideoProviderLease() {
		return video_lease->Acquire();
	}
	void BeginVideoProviderRetirement() { video_lease->Begin(); }
	void CompleteVideoProviderRetirement() { video_lease->Complete(); }
	void WaitForLeaseDrain() { video_lease->Drain(); }
	void ReloadSubtitlesProvider();

	void LoadTimecodes(agi::fs::path path);
	void CloseTimecodes();
	bool CanCloseTimecodes() const { return !timecodes_file.empty(); }
	agi::fs::path const& TimecodesName() const { return timecodes_file; }
	agi::vfr::Framerate const& Timecodes() const { return timecodes; }

	void LoadKeyframes(agi::fs::path path);
	void CloseKeyframes();
	bool CanCloseKeyframes() const { return !keyframes_file.empty(); }
	agi::fs::path const& KeyframesName() const { return keyframes_file; }
	std::vector<int> const& Keyframes() const { return keyframes; }
	bool CanGenerateSceneChangeKeyframes() const;
	bool GenerateSceneChangeKeyframes();

	void LoadList(std::vector<agi::fs::path> const& files);

	DEFINE_SIGNAL_ADDERS(AnnounceAudioProviderModified, AddAudioProviderListener)
	DEFINE_SIGNAL_ADDERS(AnnounceVideoProviderModified, AddVideoProviderListener)
	DEFINE_SIGNAL_ADDERS(AnnounceTimecodesModified, AddTimecodesListener)
	DEFINE_SIGNAL_ADDERS(AnnounceKeyframesModified, AddKeyframesListener)
};
