// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#pragma once

#include "async_video_provider_host.h"
#include "include/aegisub/video_provider.h"
#include "motion_track/frame_reader.h"
#include "source_frame_format_selection.h"
#include "ui_dispatch.h"
#include "video_memory_stats.h"
#include "video_render_packet.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs_fwd.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

class AssDialogue;
class AssFile;
class SubtitlesProvider;
class TransientFontSet;
class VideoProvider;
class VideoProviderError;
struct AssDialogueBase;
struct VideoFrame;
namespace agi {
	class BackgroundRunner;
	class SingleChoiceInteractionSink;
	namespace dispatch { class Queue; }
}

enum class KeyPointRangeScanStatus {
	Success,
	InvalidRequest,
	FrameUnavailable,
	AnchorMismatch
};

struct KeyPointRangeScanRequest {
	int frame = -1;
	int x = 0;
	int y = 0;
	unsigned char r = 0;
	unsigned char g = 0;
	unsigned char b = 0;
	unsigned char tolerance = 0;
	int scan_step = 2;
	int bounds_tolerance = 5;
	bool detect_fade = false;
	int max_fade_frames = 0;
};

struct KeyPointRangeScanResult {
	KeyPointRangeScanStatus status = KeyPointRangeScanStatus::InvalidRequest;
	int left = -1;
	int right = -1;
	int strict_left = -1;
	int strict_right = -1;
	int fade_in_end = -1;
	int fade_out_start = -1;
	bool fade_in_detected = false;
	bool fade_out_detected = false;
	double fade_in_confidence = 0.0;
	double fade_out_confidence = 0.0;
};

/// Process-unique identity of the raw source video behind a provider.
/// Generation changes whenever a new provider is installed; a batch holding
/// a stale generation must be discarded.
struct RawVideoIdentity {
	std::uint64_t generation = 0;
	int width = 0;
	int height = 0;
	int frame_count = 0;

	bool Matches(RawVideoIdentity const& other) const noexcept {
		return generation == other.generation && width == other.width
		    && height == other.height && frame_count == other.frame_count;
	}
};

enum class RawVideoBatchStatus : std::uint8_t {
	Completed,
	ProviderChanged,
	FrameUnavailable,
	DecodeError,
	Error,
};

struct RawVideoBatchResult {
	RawVideoBatchStatus status = RawVideoBatchStatus::Error;
	std::string message;
};

/// Batch-scoped access to raw source frames. Only AsyncVideoProvider can
/// construct one; the reference target is kept alive by the enclosing
/// RunRawVideoBatch Sync. Non-copyable, non-movable: instances cannot escape
/// the callback. RawBgraView results are valid until the next FetchBgra or
/// until the callback returns.
class RawFrameAccess {
	friend class AsyncVideoProvider;
	RawFrameAccess(VideoProvider& source, RawVideoIdentity identity) noexcept
	: source(source), identity(identity) { }

	VideoProvider& source;
	RawVideoIdentity identity;
	VideoFrame scratch;

public:
	RawFrameAccess(RawFrameAccess const&) = delete;
	RawFrameAccess& operator=(RawFrameAccess const&) = delete;

	RawVideoIdentity Identity() const noexcept { return identity; }

	/// Decodes one raw frame into `out`. Out-of-range frames yield
	/// FrameUnavailable; decode failures map to DecodeError; unexpected
	/// exceptions to Error. Never throws.
	aegisub::motion_track::FrameReadResult FetchBgra(int frame, aegisub::motion_track::RawBgraView& out) noexcept;
};

/// Asynchronous helper for video frame requests.
///
/// Drag/preview callers can supersede in-flight work. Inspection stepping is
/// serialized by VideoController so at most one frame is in flight while later
/// repeat input is coalesced to the newest target.
class AsyncVideoProvider {
	/// Asynchronous work queue
	std::unique_ptr<agi::dispatch::Queue> worker;

	/// Subtitles provider
	std::unique_ptr<SubtitlesProvider> subs_provider;
	/// Video provider
	std::unique_ptr<VideoProvider> source_provider;
	/// Event sink for frame-ready and error events
	AsyncVideoProviderEventSink event_sink;

	int frame_number = -1; ///< Last frame number requested
	double time = -1.; ///< Time of the frame for UI/display state
	agi::vfr::Framerate subtitles_timecodes;

	/// Copy of the subtitles file to avoid having to touch the project context
	std::unique_ptr<AssFile> subs;
	/// Worker-owned row index for applying incremental subtitle patches.
	std::vector<AssDialogue*> subs_lines_by_row;

	/// If >= 0, the subtitles provider current has just the lines visible on
	/// that frame loaded. If -1, the entire file is loaded. If -2, the
	/// currently loaded file is out of date.
	int single_frame = -1;

	/// Last rendered frame number
	int last_rendered = -1;
	/// Last rendered subtitles on that frame
	std::vector<AssDialogueBase> last_lines;
	/// Cached source frame for subtitle-only rerenders of the current frame
	int cached_source_frame_number = -1;
	SourceFrameOutputMode cached_source_mode = SourceFrameOutputMode::Bgra8;
	bool cached_source_force_bgra = false;
	SourceFrame cached_source_frame;
	std::shared_ptr<VideoFrame> cached_source_frame_storage;
	std::shared_ptr<void> cached_source_frame_owner;
	/// Check if we actually need to honor a frame request or if no visible
	/// lines have actually changed
	bool NeedUpdate(std::vector<AssDialogueBase const*> const& visible_lines);

	VideoRenderPacket ProcRenderPacket(int frame, double time, bool raw = false);
	VideoRenderPacket ProcRenderPacket(int frame, double time, bool raw, bool force_bgra_frame);
	void ResetCachedSourceFrame() noexcept;
	bool CanReuseCachedSourceFrame(int frame, bool raw, bool force_bgra_frame) const noexcept;
	void ReuseCachedSourceFrame(VideoRenderPacket& packet, std::shared_ptr<VideoFrame>& frame) const;
	void UpdateCachedSourceFrame(int frame, bool force_bgra_frame, VideoRenderPacket const& packet) noexcept;

	/// Monotonic counter used to identify the latest seek/drag request.
	std::atomic<std::uint64_t> request_version{ 0 };
	/// Monotonic counter used to invalidate frames when the rendered content changes.
	std::atomic<std::uint64_t> content_version{ 0 };
	/// Process-unique identity used to reject queued packets from replaced providers.
	std::uint64_t const provider_version;

	std::vector<std::shared_ptr<VideoFrame>> source_buffers;
	std::vector<std::shared_ptr<VideoFrame>> composited_buffers;
	std::vector<std::shared_ptr<SubtitleOverlayStorage>> subtitle_overlay_buffers;
	/// Continuity generation for GPU overlay upload planning.
	uint64_t overlay_continuity_generation = 1;

	/// Pending payload changes and their version increments are committed together.
	std::mutex pending_mutex;
	std::unique_ptr<AssFile> pending_subs;
	/// Sorted, unique, latest-wins snapshots of changed existing lines.
	std::vector<AssDialogueBase> pending_changed_lines;
	/// UI-side identity of the source AssFile represented by pending/worker state.
	const AssFile *subtitle_source_file = nullptr;
	std::vector<std::pair<const AssDialogue*, int>> subtitle_source_lines;
	bool pending_overlay_upload_continuity_invalidation = false;
	bool pending_check_updated = false;
	VideoSubtitleUpdateOptions pending_subtitle_update_options;
	enum class PendingFrameKind : std::uint8_t {
		None,
		CurrentContext,
		Request,
	};
	PendingFrameKind pending_frame_kind = PendingFrameKind::None;
	int pending_frame_number = -1;
	double pending_time = -1.;
	bool has_pending_color_space = false;
	std::string pending_color_space;
	bool processing_scheduled = false;
	/// Idle prefetch of upcoming frames; guarded by pending_mutex. One frame
	/// is decoded per posted job so interactive requests interleave ahead of
	/// the prefetch chain, and any new request/content version aborts it.
	int prefetch_next_frame = -1;
	int prefetch_end_frame = -1;
	std::uint64_t prefetch_request_version = 0;
	std::uint64_t prefetch_content_version = 0;
	bool prefetch_scheduled = false;
	/// Set under pending_mutex before the destructor enqueues its drain, so
	/// the self-reposting prefetch chain can never queue a link that would
	/// run after destruction. Check-and-repost must be atomic under the same
	/// mutex for the FIFO ordering argument to hold.
	bool prefetch_shutdown = false;
	std::vector<SourceFrameOutputMode> preferred_source_modes = {SourceFrameOutputMode::Bgra8};
	SourceFrameOutputMode selected_source_mode = SourceFrameOutputMode::Bgra8;
	bool has_logged_source_mode = false;

	public:
	/// Reentrancy bookkeeping for synchronous worker entries. Manipulated
	/// only by WorkerSyncTracker on the worker thread; public entries read
	/// it to reject same-thread nesting before it deadlocks Queue::Sync.
	struct WorkerSyncState {
		mutable std::mutex mutex;
		mutable std::thread::id owner;
		mutable int depth = 0;
	};

	WorkerSyncState sync_state;

private:
	void DeliverFrameReady(VideoRenderPacket packet, double time);
	void DeliverVideoError(std::string const& message);
	void DeliverSubtitlesError(std::string const& message);
	void AdvanceOverlayUploadContinuity();
	void TrimReusablePools();
	bool ReconfigureSourceOutputMode();
	void ScheduleProcessing();
	bool ProcessPending();
	void ProcessPrefetch();
	bool CanContinuePrefetchLocked() const noexcept;
	bool IsReentrantWorkerCall() const;

public:
	/// @brief Load the passed subtitle file
	/// @param subs File to load
	///
	/// This function blocks until is it is safe for the calling thread to
	/// modify subs
	void LoadSubtitles(const AssFile *subs, VideoSubtitleUpdateOptions options = {}) throw();

	/// @brief Update a previously loaded subtitle file
	/// @param subs Subtitle file which was last passed to LoadSubtitles
	/// @param changes Set of lines which have changed
	///
	/// This function only supports changes to existing lines, and not
	/// insertions or deletions.
	void UpdateSubtitles(
		const AssFile *subs,
		const AssDialogue *changes,
		VideoSubtitleUpdateOptions options = {}) throw();
	void UpdateSubtitles(
		const AssFile *subs,
		std::span<const AssDialogue *const> changes,
		VideoSubtitleUpdateOptions options = {}) throw();

	/// @brief Queue a latest-only preview request for a frame
	/// @brief frame Frame number
	/// @brief time  Exact start time of the frame in seconds
	///
	/// This is intended for seek/drag preview. Pending requests are replaced by
	/// newer ones, so there is no guarantee that every requested frame is shown.
	///
	/// If `supersede_in_flight` is true, supersedes any in-flight request and
	/// may drop a frame which finishes after a newer request arrives (latest-only).
	void RequestFrame(int frame, double time, bool supersede_in_flight = true) throw();

	/// Cancel any pending preview frame request and supersede in-flight work.
	void CancelPendingFrameRequests() noexcept;
	bool TryAdoptCachedFrame(VideoRenderPacket& packet, int frame, double time) noexcept;
	/// Align the provider's current-frame context with a frame presented from an
	/// external cache, without requesting another render.
	void SetCurrentFrameContext(int frame, double time) throw();

	/// @brief Warm the frame cache for upcoming frames while idle
	/// @brief first_frame First frame to decode
	/// @brief count     Number of frames to decode
	///
	/// Decodes frames on the worker queue purely to populate the source frame
	/// cache; no packets are rendered or delivered. One frame is decoded per
	/// posted job so interactive requests are never queued behind more than a
	/// single prefetch decode, and the prefetch aborts as soon as a newer
	/// request or content version arrives. No-op without a frame cache.
	void PrefetchFrames(int first_frame, int count) noexcept;

	/// Stop idle prefetch without invalidating an interactive frame request.
	/// A decode already running finishes, but no further frames are prefetched.
	void CancelFramePrefetch() noexcept;

	/// @brief Synchronously get a CPU-readable BGRA frame
	/// @brief frame Frame number
	/// @brief time  Exact start time of the frame in seconds
	/// @brief raw   Get raw frame without subtitles
	std::shared_ptr<VideoFrame> GetFrame(int frame, double time, bool raw = false);
	/// @brief Synchronously get a CPU-readable BGRA frame regardless of display source mode
	/// @brief frame Frame number
	/// @brief time  Exact start time of the frame in seconds
	/// @brief raw   Get raw frame without subtitles
	std::shared_ptr<VideoFrame> GetFrameBgra(int frame, double time, bool raw = false);
	KeyPointRangeScanResult FindKeyPointRange(KeyPointRangeScanRequest const& request);
	VideoRenderPacket GetRenderPacket(int frame, double time, bool raw = false);

	/// Snapshot of the raw source identity (generation + geometry). A pure
	/// read; it grants no lifetime. Holding a provider pointer/identity
	/// without a Project lease and calling into it is invalid.
	RawVideoIdentity GetRawVideoIdentity() const noexcept;

	/// Run `callback` on the worker thread with batch-scoped raw frame
	/// access, after flushing pending preview work to quiescence. The batch
	/// is rejected with ProviderChanged when the live identity no longer
	/// matches `expected_identity`. The callback must not call any other
	/// AsyncVideoProvider method; its only capability is RawFrameAccess.
	RawVideoBatchResult RunRawVideoBatch(
		RawVideoIdentity expected_identity,
		std::function<RawVideoBatchStatus(RawFrameAccess&)> const& callback);

	/// Ask the video provider to change YCbCr matricies
	void SetColorSpace(std::string const& matrix);
	bool SetPreferredSourceModes(std::vector<SourceFrameOutputMode> modes);
	void ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider> provider);
	/// Check whether a packet still represents the latest content and request.
	bool IsCurrent(VideoRenderDeliveryVersion version) const noexcept;
	/// Check both delivery version and the frame currently expected by a consumer.
	bool IsCurrent(VideoRenderPacket const& packet, int expected_frame) const noexcept;
	SourceFrameOutputMode GetSelectedSourceMode() const { return selected_source_mode; }
	AsyncVideoProviderMemoryStats CollectMemoryStats();

	int GetFrameCount() const             { return source_provider->GetFrameCount(); }
	int GetWidth() const                  { return source_provider->GetWidth(); }
	int GetHeight() const                 { return source_provider->GetHeight(); }
	SourceFrameGeometry GetFrameGeometry() const { return source_provider->GetFrameGeometry(); }
	double GetDAR() const                 { return source_provider->GetDAR(); }
	agi::vfr::Framerate GetFPS() const    { return source_provider->GetFPS(); }
	std::vector<int> GetKeyFrames() const { return source_provider->GetKeyFrames(); }
	std::string GetColorSpace() const     { return source_provider->GetColorSpace(); }
	std::string GetRealColorSpace() const { return source_provider->GetRealColorSpace(); }
	SourceFrameColorMetadata GetColorMetadata() const { return source_provider->GetColorMetadata(); }
	SourceFrameColorMetadata GetRealColorMetadata() const { return source_provider->GetRealColorMetadata(); }
	std::string GetWarning() const        { return source_provider->GetWarning(); }
	std::string GetDecoderName() const    { return source_provider->GetDecoderName(); }
	std::string GetNativeFormatDescription() const { return source_provider->GetNativeFormatDescription(); }
	bool ShouldSetVideoProperties() const { return source_provider->ShouldSetVideoProperties(); }
	bool HasAudio() const                 { return source_provider->HasAudio(); }
	bool CanGenerateSceneChangeKeyframes() const;
	std::string GetSceneChangeKeyframeCacheToken() const;
	void GenerateSceneChangeKeyframes(agi::fs::path const& output_path, agi::BackgroundRunner *br);

	/// @brief Constructor
	/// @param videoFileName File to open
	/// @param event_sink Callback sink to receive frame-ready and error notifications
	AsyncVideoProvider(agi::fs::path const& filename, std::string const& colormatrix, AsyncVideoProviderEventSink event_sink, agi::BackgroundRunner *br, std::shared_ptr<const TransientFontSet> transient_fonts = {}, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink = {});
	AsyncVideoProvider(std::unique_ptr<VideoProvider> source_provider, std::unique_ptr<SubtitlesProvider> subs_provider, AsyncVideoProviderEventSink event_sink);
	~AsyncVideoProvider();

	void SetSubtitlesTimecodes(agi::vfr::Framerate timecodes);
};

/// Create a CPU-readable BGRA frame suitable for UI rendering, compositing any
/// premultiplied subtitle overlay contained in the packet.
std::shared_ptr<VideoFrame> BakePacketForCpuReadback(VideoRenderPacket const& packet);

DEFINE_EXCEPTION(AsyncVideoProviderVideoError, agi::Exception);
DEFINE_EXCEPTION(AsyncVideoProviderSubtitlesError, agi::Exception);
