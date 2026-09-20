#include <main.h>

#include "../../src/perf_trace.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>

#include <fstream>
#include <locale>
#include <sstream>

namespace {
class DiagnosticTestNumpunct final : public std::numpunct<char> {
	[[nodiscard]] char do_decimal_point() const override { return ','; }
	[[nodiscard]] char do_thousands_sep() const override { return '_'; }
	[[nodiscard]] std::string do_grouping() const override { return "\3"; }
};

std::string ReadAll(agi::fs::path const& path) {
	std::ifstream in(path, std::ios::in | std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

std::string Utf8PathSegment() {
	return "\xE8\xB7\xAF\xE5\xBE\x84";
}
}

TEST(PerfTrace, TileDiagnosticSummaryPreservesHashAndIsExplicitlyOptIn) {
	auto const previous_locale = std::locale();
	auto restore_locale = agi::make_scope_exit([&] { std::locale::global(previous_locale); });
	std::locale::global(std::locale(previous_locale, new DiagnosticTestNumpunct));
	agi::Path path_helper;
	auto const directory = agi::fs::UniquePath(path_helper.Decode("?temp/tile_diagnostic_trace_%%%%%%%%"));
	perf_trace::InitializeAt(directory, "test-build", "audio");
	perf_trace::AudioContentTileEvent event;
	event.stage = "raw_summary";
	event.spectrum = true;
	event.provider_generation = 1;
	event.analysis_generation = 3;
	event.tile_index = 267;
	event.diagnostic_hash = 0xfedcba9876543210ULL;
	event.diagnostic_elements = 262144;
	event.diagnostic_nonfinite = 2;
	event.diagnostic_nonzero_columns = 255;
	event.diagnostic_minimum = -0.5;
	event.diagnostic_maximum = 0.75;
	perf_trace::ObserveAudioContentTileEvent(event);
	event.include_diagnostics = true;
	perf_trace::ObserveAudioContentTileEvent(event);
	perf_trace::Shutdown();

	std::istringstream lines(ReadAll(directory / "trace.ndjson"));
	std::string ordinary;
	std::string diagnostic;
	ASSERT_TRUE(static_cast<bool>(std::getline(lines, ordinary)));
	ASSERT_TRUE(static_cast<bool>(std::getline(lines, diagnostic)));
	EXPECT_EQ(std::string::npos, ordinary.find("\"diagnostic_"));
	EXPECT_NE(std::string::npos, diagnostic.find("\"diagnostic_hash\":\"fedcba9876543210\""));
	EXPECT_NE(std::string::npos, diagnostic.find("\"tile_index\":267"));
	EXPECT_NE(std::string::npos, diagnostic.find("\"diagnostic_elements\":262144"));
	EXPECT_NE(std::string::npos, diagnostic.find("\"diagnostic_nonfinite\":2"));
	EXPECT_NE(std::string::npos, diagnostic.find("\"diagnostic_nonzero_columns\":255"));
	EXPECT_NE(std::string::npos, diagnostic.find("\"diagnostic_minimum\":-0.5"));
	EXPECT_NE(std::string::npos, diagnostic.find("\"diagnostic_maximum\":0.75"));
	std::string extra;
	EXPECT_FALSE(static_cast<bool>(std::getline(lines, extra)));
}

TEST(PerfTrace, WritesExpectedSessionFiles) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(path_helper.Decode("?temp/perf_trace_session_%%%%%%%%"));

	perf_trace::InitializeAt(session_dir, "test-build", "unit-test");
	ASSERT_TRUE(perf_trace::IsEnabled());

	perf_trace::TraceSeek(12, false);
	perf_trace::ObserveFrameRequest(12, 0.5, false);
	perf_trace::ObserveFrameResult(12, 0.5, true, false);
	perf_trace::ObserveFrameResult(11, 0.458333, false, false);
	perf_trace::ResetAudioUiTimerInterval();
	perf_trace::ObserveAudioUiTimerPosition(100);
	perf_trace::ObserveAudioUiTimerPosition(120);
	perf_trace::ObserveAudioUiDuration("audio_display.paint", 3.5, 640, 2, false);
	{
		perf_trace::AudioUiDurationScope trace("audio_display.scoped_test", 7, 9);
	}
	{
		perf_trace::AudioUiDurationScope trace("audio_display.cancelled_test");
		trace.Cancel();
	}
	{
		perf_trace::VideoUiDurationScope trace("video_display.scoped_test", 3, 4);
	}
	perf_trace::AudioOutputSnapshot output_snapshot;
	output_snapshot.backend_name = "xaudio2";
	output_snapshot.reason = "unit_test";
	output_snapshot.queued_buffers = 2;
	output_snapshot.queued_ms = 40.0;
	output_snapshot.submitted_buffers = 1;
	output_snapshot.submitted_frames = 960;
	output_snapshot.submitted_bytes = 3840;
	output_snapshot.submitted_ms = 20.0;
	output_snapshot.fill_duration_ms = 1.25;
	output_snapshot.played_frames = 1440;
	output_snapshot.engine_latency_frames = 256;
	output_snapshot.glitch_count = 3;
	output_snapshot.source_rate_hz = 48000;
	output_snapshot.mastering_rate_hz = 48000;
	output_snapshot.low_water = true;
	perf_trace::ObserveAudioOutputSnapshot(output_snapshot);
	perf_trace::AudioDisplaySnapshot display_snapshot;
	display_snapshot.renderer_name = "skia";
	display_snapshot.content_kind = "spectrum";
	display_snapshot.frame_id = 17;
	display_snapshot.provider_generation = 3;
	display_snapshot.analysis_generation = 5;
	display_snapshot.marker_revision = 7;
	display_snapshot.chrome_revision = 9;
	display_snapshot.presentation_revision = 11;
	display_snapshot.content_scale = 1.25;
	display_snapshot.viewport_first_column = 257;
	display_snapshot.viewport_column_count = 3840;
	display_snapshot.target_width = 3840;
	display_snapshot.target_height = 320;
	display_snapshot.visible_tile_count = 16;
	display_snapshot.ready_tile_count = 16;
	display_snapshot.complete_content_viewport = true;
	display_snapshot.cursor_only = true;
	display_snapshot.retained_layers_reused = true;
	display_snapshot.focused = true;
	display_snapshot.middle_seek_active = true;
	display_snapshot.cursor_source = "playback";
	display_snapshot.cursor_position_ms = 1234;
	display_snapshot.cursor_device_x = 321.5;
	display_snapshot.cursor_label_visible = false;
	display_snapshot.content_tiles_drawn_this_frame = 4;
	display_snapshot.gpu_tile_uploads_this_frame = 2;
	display_snapshot.swapped = true;
	display_snapshot.bitmap_cache_hits = 41;
	display_snapshot.bitmap_cache_misses = 7;
	display_snapshot.source_cache_budget_bytes = 64 * 1024 * 1024;
	display_snapshot.source_cache_bytes = 6 * 1024 * 1024;
	display_snapshot.source_cache_entries = 768;
	display_snapshot.source_cache_hits = 93;
	display_snapshot.source_cache_misses = 29;
	display_snapshot.source_cache_visible_builds = 29;
	display_snapshot.source_cache_visible_lock_contention = 2;
	display_snapshot.source_cache_prefetch_requests = 8;
	display_snapshot.source_cache_prefetch_builds = 64;
	display_snapshot.source_cache_prefetch_busy_skips = 3;
	display_snapshot.source_cache_stale_drops = 4;
	display_snapshot.source_cache_evictions = 5;
	display_snapshot.source_cache_prefetch_enabled = true;
	display_snapshot.cpu_tile_budget_bytes = 40 * 1024 * 1024;
	display_snapshot.cpu_tile_bytes = 32 * 1024 * 1024;
	display_snapshot.cpu_tile_entries = 16;
	display_snapshot.cpu_tile_hits = 24;
	display_snapshot.cpu_tile_misses = 8;
	display_snapshot.cpu_tile_evictions = 2;
	display_snapshot.cpu_payload_budget_bytes = 12 * 1024 * 1024;
	display_snapshot.cpu_payload_bytes = 9 * 1024 * 1024;
	display_snapshot.cpu_payload_entries = 15;
	display_snapshot.cpu_payload_hits = 19;
	display_snapshot.cpu_payload_misses = 6;
	display_snapshot.cpu_payload_evictions = 3;
	display_snapshot.fft_budget_bytes = 88 * 1024 * 1024;
	display_snapshot.fft_active_cache_budget_bytes = 88 * 1024 * 1024;
	display_snapshot.fft_bytes = 7 * 1024 * 1024;
	display_snapshot.fft_entries = 896;
	display_snapshot.fft_hits = 120;
	display_snapshot.fft_misses = 32;
	display_snapshot.fft_visible_builds = 32;
	display_snapshot.fft_evictions = 4;
	display_snapshot.gpu_tile_budget_bytes = 32 * 1024 * 1024;
	display_snapshot.gpu_tile_bytes = 30 * 1024 * 1024;
	display_snapshot.gpu_tile_entries = 15;
	display_snapshot.gpu_tile_hits = 11;
	display_snapshot.gpu_tile_misses = 5;
	display_snapshot.gpu_tile_uploads = 5;
	display_snapshot.gpu_tile_upload_bytes = 10 * 1024 * 1024;
	display_snapshot.gpu_tile_evictions = 1;
	display_snapshot.gpu_palette_uploads = 3;
	display_snapshot.worker_builds_started = 18;
	display_snapshot.worker_builds_ready = 16;
	display_snapshot.worker_builds_cancelled = 1;
	display_snapshot.worker_payload_builds_started = 17;
	display_snapshot.worker_payload_builds_ready = 15;
	display_snapshot.worker_payload_builds_cancelled = 2;
	display_snapshot.worker_superseded_requests = 3;
	perf_trace::ObserveAudioDisplaySnapshot(display_snapshot);
	perf_trace::AudioContentTileEvent tile_event;
	tile_event.stage = "worker_build_end";
	tile_event.outcome = "ready";
	tile_event.spectrum = true;
	tile_event.provider_generation = 3;
	tile_event.analysis_generation = 5;
	tile_event.tile_index = 9;
	tile_event.column_count = 256;
	tile_event.spectrum_bin_count = 2048;
	tile_event.request_serial = 27;
	tile_event.bytes = 2 * 1024 * 1024;
	tile_event.variant_revision = 11;
	tile_event.visible = 1;
	tile_event.include_fft_deltas = true;
	tile_event.fft_cache_hits_delta = 17;
	tile_event.fft_cache_misses_delta = 23;
	tile_event.fft_visible_builds_delta = 23;
	tile_event.fft_cache_evictions_delta = 4;
	perf_trace::ObserveAudioContentTileEvent(tile_event);
	perf_trace::ResetVideoPlaybackInterval();
	perf_trace::ObserveVideoPlaybackTick(12);
	perf_trace::ObserveVideoPlaybackTick(13);
	perf_trace::TraceWindowOpenBegin("main");
	perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.paths_and_options", 4.25);
	perf_trace::TraceWindowOpenEnd("main", 8.5, true);
	VideoMemorySnapshot memory_snapshot;
	memory_snapshot.async.provider.cache_native_bytes = 4096;
	memory_snapshot.async.provider.cache_native_frames = 1;
	memory_snapshot.async.source_pool_bytes = 2048;
	memory_snapshot.async.source_pool_buffers = 1;
	memory_snapshot.display.displayed_packet_ref_bytes = 1024;
	memory_snapshot.display.primary_renderer_texture_bytes = 8192;
	memory_snapshot.display.primary_renderer_name = "libplacebo";
	memory_snapshot.audio.provider_name = "RAM";
	memory_snapshot.audio.storage_kind = "memory";
	memory_snapshot.audio.storage_bytes = 16384;
	memory_snapshot.audio.logical_bytes = 12288;
	memory_snapshot.audio.decoded_bytes = 8192;
	perf_trace::ObserveVideoMemorySnapshot("unit_test", memory_snapshot, true);
	perf_trace::TraceLuaDialogOpenBegin();
	perf_trace::ObserveLuaDialogPhase("build_model", 3, 2, 4.5);
	perf_trace::ObserveLuaDialogControlTypeSummary("dropdown", 3, 2, 1, 18, 18, 2.75);
	perf_trace::ObserveLuaDialogControlStepSummary("dropdown", "native_construct", 3, 2, 1, 18, 18, 1.25);
	perf_trace::TraceLuaDialogOpenEnd(3, 2, 12.5, true);
	LOG_W("perf_trace/test") << "warning event";
	perf_trace::Shutdown();

	EXPECT_FALSE(perf_trace::IsEnabled());
	EXPECT_TRUE(agi::fs::FileExists(session_dir / "trace.ndjson"));
	EXPECT_TRUE(agi::fs::FileExists(session_dir / "summary.txt"));
	EXPECT_TRUE(agi::fs::FileExists(session_dir / "manifest.txt"));

	auto const trace = ReadAll(session_dir / "trace.ndjson");
	auto const summary = ReadAll(session_dir / "summary.txt");
	auto const manifest = ReadAll(session_dir / "manifest.txt");

	EXPECT_NE(std::string::npos, trace.find("\"name\":\"seek\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_frame_request\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_frame_delivered\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_frame_dropped\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_ui_timer_interval\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_ui_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"phase\":\"audio_display.paint\""));
	EXPECT_NE(std::string::npos, trace.find("\"phase\":\"audio_display.scoped_test\""));
	EXPECT_NE(std::string::npos, trace.find("\"detail_a\":7"));
	EXPECT_NE(std::string::npos, trace.find("\"detail_b\":9"));
	EXPECT_EQ(std::string::npos, trace.find("audio_display.cancelled_test"));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_ui_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"phase\":\"video_display.scoped_test\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_output_snapshot\""));
	EXPECT_NE(std::string::npos, trace.find("\"played_frames\":1440"));
	EXPECT_NE(std::string::npos, trace.find("\"engine_latency_frames\":256"));
	EXPECT_NE(std::string::npos, trace.find("\"glitch_count\":3"));
	EXPECT_NE(std::string::npos, trace.find("\"source_rate_hz\":48000"));
	EXPECT_NE(std::string::npos, trace.find("\"mastering_rate_hz\":48000"));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_display_snapshot\""));
	EXPECT_NE(std::string::npos, trace.find("\"renderer\":\"skia\""));
	EXPECT_NE(std::string::npos, trace.find("\"content_kind\":\"spectrum\""));
	EXPECT_NE(std::string::npos, trace.find("\"frame_id\":17"));
	EXPECT_NE(std::string::npos, trace.find("\"marker_revision\":7"));
	EXPECT_NE(std::string::npos, trace.find("\"chrome_revision\":9"));
	EXPECT_NE(std::string::npos, trace.find("\"presentation_revision\":11"));
	EXPECT_NE(std::string::npos, trace.find("\"content_scale\":1.25"));
	EXPECT_NE(std::string::npos, trace.find("\"cursor_only\":true"));
	EXPECT_NE(std::string::npos, trace.find("\"retained_layers_reused\":true"));
	EXPECT_NE(std::string::npos, trace.find("\"focused\":true"));
	EXPECT_NE(std::string::npos, trace.find("\"middle_seek_active\":true"));
	EXPECT_NE(std::string::npos, trace.find("\"cursor_source\":\"playback\""));
	EXPECT_NE(std::string::npos, trace.find("\"cursor_position_ms\":1234"));
	EXPECT_NE(std::string::npos, trace.find("\"cursor_device_x\":321.5"));
	EXPECT_NE(std::string::npos, trace.find("\"cursor_label_visible\":false"));
	EXPECT_NE(std::string::npos, trace.find("\"visible_content_request_called\":false"));
	EXPECT_NE(std::string::npos, trace.find("\"content_lookup_performed\":false"));
	EXPECT_NE(std::string::npos, trace.find("\"content_tiles_drawn_this_frame\":4"));
	EXPECT_NE(std::string::npos, trace.find("\"gpu_tile_uploads_this_frame\":2"));
	EXPECT_NE(std::string::npos, trace.find("\"swap_attempted\":true"));
	EXPECT_NE(std::string::npos, trace.find("\"bitmap_cache_hits\":41"));
	EXPECT_NE(std::string::npos, trace.find("\"source_cache_prefetch_builds\":64"));
	EXPECT_NE(std::string::npos, trace.find("\"cpu_tile_evictions\":2"));
	EXPECT_NE(std::string::npos, trace.find("\"cpu_payload_budget_bytes\":12582912"));
	EXPECT_NE(std::string::npos, trace.find("\"cpu_payload_bytes\":9437184"));
	EXPECT_NE(std::string::npos, trace.find("\"cpu_payload_entries\":15"));
	EXPECT_NE(std::string::npos, trace.find("\"cpu_payload_hits\":19"));
	EXPECT_NE(std::string::npos, trace.find("\"cpu_payload_misses\":6"));
	EXPECT_NE(std::string::npos, trace.find("\"cpu_payload_evictions\":3"));
	EXPECT_NE(std::string::npos, trace.find("\"fft_visible_builds\":32"));
	EXPECT_NE(std::string::npos, trace.find("\"gpu_tile_uploads\":5"));
	EXPECT_NE(std::string::npos, trace.find("\"gpu_palette_uploads\":3"));
	EXPECT_NE(std::string::npos, trace.find("\"worker_payload_builds_started\":17"));
	EXPECT_NE(std::string::npos, trace.find("\"worker_payload_builds_ready\":15"));
	EXPECT_NE(std::string::npos, trace.find("\"worker_payload_builds_cancelled\":2"));
	EXPECT_NE(std::string::npos, trace.find("\"worker_superseded_requests\":3"));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_content_tile_event\""));
	EXPECT_NE(std::string::npos, trace.find("\"stage\":\"worker_build_end\""));
	EXPECT_NE(std::string::npos, trace.find("\"tile_index\":9"));
	EXPECT_NE(std::string::npos, trace.find("\"request_serial\":27"));
	EXPECT_NE(std::string::npos, trace.find("\"fft_cache_misses_delta\":23"));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_memory_snapshot\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"window_open_phase_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_phase_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_control_type_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_control_step_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_open_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"kind\":\"log\""));

	EXPECT_NE(std::string::npos, summary.find("frame.request.total=1"));
	EXPECT_NE(std::string::npos, summary.find("frame.delivered.total=1"));
	EXPECT_NE(std::string::npos, summary.find("frame.dropped.total=1"));
	EXPECT_NE(std::string::npos, summary.find("window_open.success=1"));
	EXPECT_NE(std::string::npos, summary.find("window_phase.main.startup.runtime.paths_and_options.count=1"));
	EXPECT_NE(std::string::npos, summary.find("window_phase.main.startup.runtime.paths_and_options.total_ms="));
	EXPECT_NE(std::string::npos, summary.find("lua_dialog.success=1"));
	EXPECT_NE(std::string::npos, summary.find("video_memory.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("provider_cache_native.max_bytes=4096"));
	EXPECT_NE(std::string::npos, summary.find("audio_storage.max_bytes=16384"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_timer_interval.requested_ms=20"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_timer_interval.jitter_target_ms="));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_timer_interval.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_phase.audio_display.paint.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_phase.audio_display.paint.total_ms=3.5"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_phase.audio_display.scoped_test.count=1"));
	EXPECT_NE(std::string::npos, summary.find("video_ui_phase.video_display.scoped_test.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.low_water.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.snapshot.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.content.complete_frames=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.renderer.latest=skia"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cursor.source.latest=playback"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cursor.position_ms.latest=1234"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cursor.device_x.latest=321.5"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cursor.label_visible.latest=0"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.source_cache.bytes.max=6291456"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.bitmap_cache.hits.latest=41"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.source_cache.visible_lock_contention.latest=2"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.source_cache.prefetch_builds.latest=64"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cpu_tile.entries.latest=16"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cpu_payload.bytes.max=9437184"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cpu_payload.budget_bytes.latest=12582912"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cpu_payload.entries.latest=15"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cpu_payload.hits.latest=19"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cpu_payload.misses.latest=6"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.cpu_payload.evictions.latest=3"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.fft.budget_bytes.latest=92274688"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.fft.visible_builds.latest=32"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.gpu_tile.entries.latest=15"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.gpu_tile.hits.latest=11"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.gpu_tile.misses.latest=5"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.content_scale.latest=1.25"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.gpu_tile.uploads.latest=5"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.worker.builds_ready.latest=16"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.worker.payload_builds_started.latest=17"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.worker.payload_builds_ready.latest=15"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.worker.payload_builds_cancelled.latest=2"));
	EXPECT_NE(std::string::npos, summary.find("audio_output_backend=xaudio2"));
	EXPECT_NE(std::string::npos, summary.find("audio_provider=RAM"));
	EXPECT_NE(std::string::npos, summary.find("op.seek=1"));
	EXPECT_NE(std::string::npos, summary.find("log.warning=1"));

	EXPECT_NE(std::string::npos, manifest.find("build=test-build"));
	EXPECT_NE(std::string::npos, manifest.find("source=unit-test"));
	EXPECT_NE(std::string::npos, manifest.find("trace_selection=all"));
}

TEST(PerfTrace, NonSwapAudioRendererDoesNotReportSwapFailure) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(path_helper.Decode("?temp/perf_trace_wx_audio_%%%%%%%%"));

	perf_trace::InitializeAt(session_dir, "test-build", "audio");
	ASSERT_TRUE(perf_trace::IsEnabled());
	perf_trace::AudioDisplaySnapshot snapshot;
	snapshot.renderer_name = "wx";
	snapshot.content_kind = "waveform";
	snapshot.swap_attempted = false;
	perf_trace::ObserveAudioDisplaySnapshot(snapshot);
	perf_trace::Shutdown();

	auto const trace = ReadAll(session_dir / "trace.ndjson");
	auto const summary = ReadAll(session_dir / "summary.txt");
	EXPECT_NE(std::string::npos, trace.find("\"swap_attempted\":false"));
	EXPECT_NE(std::string::npos, summary.find("audio_display.swap.failures=0"));
}

TEST(PerfTrace, SupportsNamedTraceSelection) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(path_helper.Decode("?temp/perf_trace_selection_%%%%%%%%"));

	perf_trace::InitializeAt(session_dir, "test-build", "audio,lua-dialog");
	ASSERT_TRUE(perf_trace::IsEnabled());
	EXPECT_TRUE(perf_trace::IsCategoryEnabled(perf_trace::Category::Audio));
	EXPECT_TRUE(perf_trace::IsCategoryEnabled(perf_trace::Category::LuaDialog));
	EXPECT_FALSE(perf_trace::IsCategoryEnabled(perf_trace::Category::Video));
	EXPECT_FALSE(perf_trace::IsCategoryEnabled(perf_trace::Category::Log));

	perf_trace::ObserveFrameRequest(12, 0.5, false);
	perf_trace::TraceLuaDialogOpenBegin();
	perf_trace::TraceLuaDialogOpenEnd(1, 1, 3.5, true);
	perf_trace::AudioOutputSnapshot output_snapshot;
	output_snapshot.backend_name = "directsound2";
	output_snapshot.reason = "selection_test";
	output_snapshot.queued_ms = 20.0;
	output_snapshot.submitted_buffers = 1;
	output_snapshot.submitted_frames = 480;
	output_snapshot.submitted_bytes = 960;
	output_snapshot.submitted_ms = 10.0;
	output_snapshot.fill_duration_ms = 0.5;
	output_snapshot.starved = true;
	output_snapshot.recovered = true;
	perf_trace::ObserveAudioUiDuration("audio_display.scroll", 1.25, 128, 1, false);
	perf_trace::ObserveAudioOutputSnapshot(output_snapshot);
	LOG_W("perf_trace/test") << "warning event";
	perf_trace::Shutdown();

	auto const trace = ReadAll(session_dir / "trace.ndjson");
	auto const summary = ReadAll(session_dir / "summary.txt");
	auto const manifest = ReadAll(session_dir / "manifest.txt");

	EXPECT_EQ(std::string::npos, trace.find("\"name\":\"video_frame_request\""));
	EXPECT_EQ(std::string::npos, trace.find("\"kind\":\"log\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_open_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_ui_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_output_snapshot\""));

	EXPECT_NE(std::string::npos, summary.find("trace.selection=audio,lua-dialog"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_phase.audio_display.scroll.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.starved.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.recovered.count=1"));
	EXPECT_EQ(std::string::npos, summary.find("frame.request.total=1"));
	EXPECT_EQ(std::string::npos, summary.find("log.warning=1"));

	EXPECT_NE(std::string::npos, manifest.find("source=audio,lua-dialog"));
	EXPECT_NE(std::string::npos, manifest.find("trace_selection=audio,lua-dialog"));
}

TEST(PerfTrace, ManifestUsesRelativeArtifactPaths) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(path_helper.Decode(std::string("?temp/perf_trace_") + Utf8PathSegment() + "_%%%%%%%%"));

	perf_trace::InitializeAt(session_dir, "test-build", "utf8-paths");
	ASSERT_TRUE(perf_trace::IsEnabled());
	perf_trace::Shutdown();

	auto const manifest = ReadAll(session_dir / "manifest.txt");
	auto const session_dir_text = agi::fs::PathToString(session_dir);

	EXPECT_NE(std::string::npos, session_dir_text.find(Utf8PathSegment()));
	EXPECT_EQ(std::string::npos, manifest.find(session_dir_text));
	EXPECT_NE(std::string::npos, manifest.find("session_dir=."));
	EXPECT_NE(std::string::npos, manifest.find("trace_file=trace.ndjson"));
	EXPECT_NE(std::string::npos, manifest.find("summary_file=summary.txt"));
}
