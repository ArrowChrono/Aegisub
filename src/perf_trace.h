#pragma once

#include <libaegisub/fs_fwd.h>

#include "video_memory_stats.h"

#include <cstdint>
#include <string>

namespace aegisub::async_video_trace {
struct PipelineEvent;
}

namespace perf_trace {

enum class Category {
	Ops,
	Video,
	Audio,
	Memory,
	UiWindow,
	LuaDialog,
	Log,
};

class AudioUiDurationScope {
	char const* phase = nullptr;
	int64_t started_ns = 0;
	int detail_a = -1;
	int detail_b = -1;

public:
	explicit AudioUiDurationScope(char const* phase, int detail_a = -1, int detail_b = -1) noexcept;
	~AudioUiDurationScope() noexcept;

	AudioUiDurationScope(AudioUiDurationScope const&) = delete;
	AudioUiDurationScope& operator=(AudioUiDurationScope const&) = delete;

	bool IsActive() const noexcept { return started_ns != 0; }
	void SetDetails(int first, int second = -1) noexcept;
	void Cancel() noexcept { started_ns = 0; }
};

class VideoUiDurationScope {
	char const* phase = nullptr;
	int64_t started_ns = 0;
	int detail_a = -1;
	int detail_b = -1;

public:
	explicit VideoUiDurationScope(char const* phase, int detail_a = -1, int detail_b = -1) noexcept;
	~VideoUiDurationScope() noexcept;

	VideoUiDurationScope(VideoUiDurationScope const&) = delete;
	VideoUiDurationScope& operator=(VideoUiDurationScope const&) = delete;

	bool IsActive() const noexcept { return started_ns != 0; }
	void SetDetails(int first, int second = -1) noexcept;
	void Cancel() noexcept { started_ns = 0; }
};

struct AudioOutputSnapshot {
	std::string backend_name;
	std::string reason;
	int64_t queued_buffers = -1;
	double queued_ms = -1.0;
	int64_t submitted_buffers = -1;
	int64_t submitted_frames = -1;
	int64_t submitted_bytes = -1;
	double submitted_ms = -1.0;
	double fill_duration_ms = -1.0;
	int64_t played_frames = -1;
	int64_t engine_latency_frames = -1;
	int64_t glitch_count = -1;
	int source_rate_hz = -1;
	int mastering_rate_hz = -1;
	bool low_water = false;
	bool starved = false;
	bool recovered = false;
	bool end_of_stream = false;
};

struct AudioDisplaySnapshot {
	std::string renderer_name;
	std::string content_kind;
	std::uint64_t frame_id = 0;
	std::uint64_t provider_generation = 0;
	std::uint64_t analysis_generation = 0;
	std::uint64_t marker_revision = 0;
	std::uint64_t chrome_revision = 0;
	std::uint64_t presentation_revision = 0;
	double content_scale = 1.0;
	std::uint64_t viewport_first_column = 0;
	std::uint64_t viewport_column_count = 0;
	std::uint64_t target_width = 0;
	std::uint64_t target_height = 0;
	std::uint64_t visible_tile_count = 0;
	std::uint64_t ready_tile_count = 0;
	bool complete_content_viewport = false;
	bool retained_content_frame = false;
	bool cursor_only = false;
	bool retained_layers_reused = false;
	bool focused = false;
	bool middle_seek_active = false;
	std::string cursor_source = "none";
	std::int64_t cursor_position_ms = -1;
	double cursor_device_x = -1.0;
	bool cursor_label_visible = false;
	bool visible_content_request_called = false;
	bool content_lookup_performed = false;
	std::uint64_t content_tiles_drawn_this_frame = 0;
	std::uint64_t gpu_tile_uploads_this_frame = 0;
	bool swap_attempted = true;
	bool swapped = false;

	std::uint64_t bitmap_cache_hits = 0;
	std::uint64_t bitmap_cache_misses = 0;
	std::uint64_t source_cache_budget_bytes = 0;
	std::uint64_t source_cache_bytes = 0;
	std::uint64_t source_cache_entries = 0;
	std::uint64_t source_cache_hits = 0;
	std::uint64_t source_cache_misses = 0;
	std::uint64_t source_cache_visible_builds = 0;
	std::uint64_t source_cache_visible_lock_contention = 0;
	std::uint64_t source_cache_prefetch_requests = 0;
	std::uint64_t source_cache_prefetch_builds = 0;
	std::uint64_t source_cache_prefetch_busy_skips = 0;
	std::uint64_t source_cache_stale_drops = 0;
	std::uint64_t source_cache_evictions = 0;
	bool source_cache_prefetch_enabled = false;

	std::uint64_t cpu_tile_budget_bytes = 0;
	std::uint64_t cpu_tile_bytes = 0;
	std::uint64_t cpu_tile_entries = 0;
	std::uint64_t cpu_tile_hits = 0;
	std::uint64_t cpu_tile_misses = 0;
	std::uint64_t cpu_tile_evictions = 0;

	std::uint64_t cpu_payload_budget_bytes = 0;
	std::uint64_t cpu_payload_bytes = 0;
	std::uint64_t cpu_payload_entries = 0;
	std::uint64_t cpu_payload_hits = 0;
	std::uint64_t cpu_payload_misses = 0;
	std::uint64_t cpu_payload_evictions = 0;

	std::uint64_t fft_budget_bytes = 0;
	std::uint64_t fft_active_cache_budget_bytes = 0;
	std::uint64_t fft_bytes = 0;
	std::uint64_t fft_entries = 0;
	std::uint64_t fft_hits = 0;
	std::uint64_t fft_misses = 0;
	std::uint64_t fft_visible_builds = 0;
	std::uint64_t fft_evictions = 0;

	std::uint64_t gpu_tile_budget_bytes = 0;
	std::uint64_t gpu_tile_bytes = 0;
	std::uint64_t gpu_tile_entries = 0;
	std::uint64_t gpu_tile_hits = 0;
	std::uint64_t gpu_tile_misses = 0;
	std::uint64_t gpu_tile_uploads = 0;
	std::uint64_t gpu_tile_upload_bytes = 0;
	std::uint64_t gpu_tile_evictions = 0;
	std::uint64_t gpu_palette_uploads = 0;

	std::uint64_t worker_builds_started = 0;
	std::uint64_t worker_builds_ready = 0;
	std::uint64_t worker_builds_cancelled = 0;
	std::uint64_t worker_payload_builds_started = 0;
	std::uint64_t worker_payload_builds_ready = 0;
	std::uint64_t worker_payload_builds_cancelled = 0;
	std::uint64_t worker_superseded_requests = 0;
};

struct AudioContentTileEvent {
	char const *stage = nullptr;
	char const *outcome = nullptr;
	bool spectrum = false;
	std::uint64_t provider_generation = 0;
	std::uint64_t analysis_generation = 0;
	std::uint64_t tile_index = 0;
	std::uint64_t column_count = 0;
	std::uint64_t spectrum_bin_count = 0;
	std::uint64_t request_serial = 0;
	std::uint64_t bytes = 0;
	std::uint64_t variant_revision = 0;
	int visible = -1;
	bool include_fft_deltas = false;
	std::uint64_t fft_cache_hits_delta = 0;
	std::uint64_t fft_cache_misses_delta = 0;
	std::uint64_t fft_visible_builds_delta = 0;
	std::uint64_t fft_cache_evictions_delta = 0;
	bool include_diagnostics = false;
	std::uint64_t diagnostic_hash = 0;
	std::uint64_t diagnostic_elements = 0;
	std::uint64_t diagnostic_nonfinite = 0;
	std::uint64_t diagnostic_nonzero_columns = 0;
	double diagnostic_minimum = 0;
	double diagnostic_maximum = 0;
	bool diagnostic_gl = false;
	bool diagnostic_gl_backend_available = false;
	std::uint64_t diagnostic_gl_texture_id = 0;
	std::uint64_t diagnostic_gl_texture_target = 0;
	std::uint64_t diagnostic_gl_owner_context = 0;
	std::uint64_t diagnostic_gl_current_context = 0;
	bool diagnostic_gl_is_texture = false;
	int diagnostic_gl_width = -1;
	int diagnostic_gl_height = -1;
};

bool IsEnabled();
bool IsCategoryEnabled(Category category);
bool ShouldSampleVideoMemory(bool force = false);
agi::fs::path GetSessionDirectory();

void Initialize(std::string const& build_label = {});
void InitializeAt(agi::fs::path const& session_dir, std::string const& build_label = {}, std::string const& source_tag = {});
void Shutdown();

void ResetAudioUiTimerInterval();
void ResetVideoPlaybackInterval();

void TraceVideoOpen(agi::fs::path const& path, int width, int height, int frame_count, bool has_audio, std::string const& decoder_name, double duration_ms);
void TracePlayStart(int frame, int start_ms);
void TracePlayStop(int frame);
void TraceSeek(int frame, bool was_playing);
void TraceAudioMiddleSeek(char const* phase, int time_ms, int frame);
void TraceVideoStepPreviewConfig(bool enabled, int interval_ms, int interval_backward_ms, int burst_window_ms, int burst_threshold, int release_delay_ms);
void TraceVideoStepPreviewBegin(int start_frame, int delta, int burst_count, int burst_threshold);
void TraceVideoStepPreviewCancel(int frame);
void TraceVideoStepPreviewRelease(int final_target_frame);

void ObserveFrameRequest(int frame, double time, bool immediate);
void ObserveFrameResult(int frame, double time, bool delivered, bool immediate);
void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms);
void ObserveVideoPipelineEvent(aegisub::async_video_trace::PipelineEvent const& event);
void ObserveVideoRenderPacketCacheLookup(int frame, bool hit, char const* source);
void ObserveVideoUiDuration(char const* phase, double duration_ms, int detail_a = -1, int detail_b = -1, bool immediate = false);
void ObserveAudioUiTimerPosition(int ms);
void ObserveAudioUiDuration(char const* phase, double duration_ms, int detail_a = -1, int detail_b = -1, bool immediate = false);
void ObserveAudioOutputSnapshot(AudioOutputSnapshot const& snapshot);
void ObserveAudioDisplaySnapshot(AudioDisplaySnapshot const& snapshot);
void ObserveAudioContentTileEvent(AudioContentTileEvent const& event) noexcept;
void ObserveVideoPlaybackTick(int frame);

void TraceWindowOpenBegin(char const* window_kind);
void ObserveWindowOpenPhase(char const* window_kind, char const* phase, double duration_ms);
void TraceWindowOpenEnd(char const* window_kind, double duration_ms, bool succeeded);

void TraceLuaDialogOpenBegin();
void ObserveLuaDialogPhase(char const* phase, int control_count, int button_count, double duration_ms);
void ObserveLuaDialogControlTypeSummary(char const* control_type, int control_count, int button_count, int instance_count, int item_count_total, int item_count_max, double duration_ms);
void ObserveLuaDialogControlStepSummary(char const* control_type, char const* step, int control_count, int button_count, int instance_count, int item_count_total, int item_count_max, double duration_ms);
void TraceLuaDialogOpenEnd(int control_count, int button_count, double duration_ms, bool succeeded);
void ObserveVideoMemorySnapshot(char const* reason, VideoMemorySnapshot const& snapshot, bool force = false);

}
