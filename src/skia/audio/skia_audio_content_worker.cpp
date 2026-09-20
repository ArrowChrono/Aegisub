#include "skia_audio_content_worker.h"

#include "skia_audio_tile_diagnostics.h"

#include "../../audio_display_source.h"
#include "../../perf_trace.h"

#include <libaegisub/audio/provider.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace aegisub::skia::audio {
namespace {

std::uint64_t NextGeneration(std::uint64_t value) noexcept {
	++value;
	return value ? value : 1;
}

perf_trace::AudioContentTileEvent MakeTileEvent(
	char const *stage,
	ContentTileKey const& key) noexcept {
	perf_trace::AudioContentTileEvent event;
	event.stage = stage;
	event.spectrum = key.kind == ContentKind::Spectrum;
	event.provider_generation = key.generation.provider;
	event.analysis_generation = key.generation.analysis;
	event.tile_index = key.tile_index;
	event.column_count = key.column_count;
	event.spectrum_bin_count = key.spectrum_bin_count;
	return event;
}

perf_trace::AudioContentTileEvent MakePayloadEvent(
	char const *stage,
	ContentUploadPayloadKey const& key) noexcept {
	auto event = MakeTileEvent(stage, key.tile);
	event.variant_revision = key.variant_revision;
	return event;
}

void ObserveTileSummary(
	perf_trace::AudioContentTileEvent event,
	TileDataSummary const& summary,
	std::uint64_t request_serial,
	bool visible) noexcept {
	event.request_serial = request_serial;
	event.visible = visible ? 1 : 0;
	event.include_diagnostics = true;
	event.diagnostic_hash = summary.hash;
	event.diagnostic_elements = summary.element_count;
	event.diagnostic_nonfinite = summary.nonfinite_count;
	event.diagnostic_nonzero_columns = summary.nonzero_columns;
	event.diagnostic_minimum = summary.minimum;
	event.diagnostic_maximum = summary.maximum;
	perf_trace::ObserveAudioContentTileEvent(event);
}

std::uint64_t CounterDelta(std::uint64_t after, std::uint64_t before) noexcept {
	return after >= before ? after - before : 0;
}

char const *BuildStatusName(ContentBuildStatus status) noexcept {
	switch (status) {
		case ContentBuildStatus::Ready: return "ready";
		case ContentBuildStatus::Cancelled: return "cancelled";
		case ContentBuildStatus::InvalidRequest: return "invalid";
	}
	return "unknown";
}

char const *PayloadBuildStatusName(ContentUploadPayloadBuildStatus status) noexcept {
	switch (status) {
		case ContentUploadPayloadBuildStatus::Ready: return "ready";
		case ContentUploadPayloadBuildStatus::Cancelled: return "cancelled";
		case ContentUploadPayloadBuildStatus::InvalidRequest: return "invalid";
	}
	return "unknown";
}

char const *PublishResultName(ContentPublishResult result) noexcept {
	switch (result) {
		case ContentPublishResult::Accepted: return "accepted";
		case ContentPublishResult::Duplicate: return "duplicate";
		case ContentPublishResult::Stale: return "stale";
		case ContentPublishResult::Invalid: return "invalid";
		case ContentPublishResult::OverBudget: return "over_budget";
	}
	return "unknown";
}

struct WorkPlan {
	std::uint64_t serial = 0;
	ContentGeneration generation;
	ContentAnalysisConfig analysis;
	std::size_t visible_tile_count = 0;
	std::uint64_t content_budget_revision = 0;
	std::shared_ptr<SpectrumBandPlan const> spectrum_band_plan;
	std::vector<ContentTileKey> tiles;
};

std::optional<std::uint64_t> LastAbsoluteColumn(ContentTileKey const& key) noexcept {
	if (key.column_count == 0
		|| key.tile_index > std::numeric_limits<std::uint64_t>::max() / key.column_count) {
		return {};
	}
	auto const first_column = key.tile_index * key.column_count;
	auto const column_offset = static_cast<std::uint64_t>(key.column_count - 1);
	if (first_column > std::numeric_limits<std::uint64_t>::max() - column_offset)
		return {};
	return first_column + column_offset;
}

std::optional<std::int64_t> ColumnSampleStart(
	std::uint64_t column,
	long double samples_per_pixel) noexcept {
	auto const value = static_cast<long double>(column) * samples_per_pixel;
	if (!std::isfinite(value)
		|| value < 0.0L
		|| value > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
		return {};
	}
	return static_cast<std::int64_t>(value);
}

std::optional<std::int64_t> RequiredWaveformDecodedSamples(
	ContentTileKey const& key,
	ContentAnalysisConfig const& analysis,
	agi::AudioProvider const& provider) noexcept {
	auto const last_column = LastAbsoluteColumn(key);
	if (!last_column || provider.GetSampleRate() <= 0 || provider.GetNumSamples() <= 0)
		return {};
	auto const samples_per_pixel_exact = static_cast<long double>(analysis.milliseconds_per_pixel)
		* provider.GetSampleRate() / 1000.0L;
	if (!std::isfinite(samples_per_pixel_exact)
		|| samples_per_pixel_exact <= 0.0L
		|| samples_per_pixel_exact
			> static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
		return {};
	}
	auto const first_column = key.tile_index * key.column_count;
	auto const first_sample = ColumnSampleStart(first_column, samples_per_pixel_exact);
	auto const last_sample = ColumnSampleStart(*last_column, samples_per_pixel_exact);
	if (!first_sample || !last_sample)
		return {};
	if (*first_sample >= provider.GetNumSamples())
		return 0;
	auto const samples_per_pixel = std::max<std::int64_t>(
		1,
		static_cast<std::int64_t>(samples_per_pixel_exact));
	if (*last_sample > std::numeric_limits<std::int64_t>::max() - samples_per_pixel)
		return {};
	return std::min(provider.GetNumSamples(), *last_sample + samples_per_pixel);
}

std::optional<std::int64_t> RequiredSpectrumDecodedSamples(
	ContentTileKey const& key,
	ContentAnalysisConfig const& analysis,
	agi::AudioProvider const& provider) noexcept {
	auto const last_column = LastAbsoluteColumn(key);
	if (!last_column || provider.GetSampleRate() <= 0 || provider.GetNumSamples() <= 0)
		return {};
	auto const samples_per_pixel = static_cast<long double>(analysis.milliseconds_per_pixel)
		* provider.GetSampleRate() / 1000.0L;
	if (!std::isfinite(samples_per_pixel) || samples_per_pixel <= 0.0L)
		return {};
	auto const last_sample = ColumnSampleStart(*last_column, samples_per_pixel);
	if (!last_sample)
		return {};
	auto block_index = static_cast<std::uint64_t>(*last_sample)
		>> analysis.spectrum_derivation_distance;
	auto const total_samples = static_cast<std::uint64_t>(provider.GetNumSamples());
	auto const hop_samples = std::uint64_t { 1 }
		<< analysis.spectrum_derivation_distance;
	auto const half_window = std::uint64_t { 1 }
		<< analysis.spectrum_derivation_size;
	auto const block_count = (total_samples - 1) / hop_samples + 1;
	block_index = std::min(block_index, block_count - 1);
	auto const center_sample = block_index * hop_samples;
	auto const remaining_samples = total_samples - center_sample;
	auto const required_samples = half_window >= remaining_samples
		? total_samples : center_sample + half_window;
	return static_cast<std::int64_t>(required_samples);
}

bool TileWaitsForDecodedSamples(
	ContentTileKey const& key,
	ContentAnalysisConfig const& analysis,
	agi::AudioProvider const& provider) noexcept {
	auto const total_samples = provider.GetNumSamples();
	if (total_samples <= 0)
		return false;
	auto const decoded_samples = std::clamp<std::int64_t>(
		provider.GetDecodedSamples(),
		0,
		total_samples);
	if (decoded_samples >= total_samples)
		return false;
	auto const required_samples = analysis.kind == ContentKind::Waveform
		? RequiredWaveformDecodedSamples(key, analysis, provider)
		: RequiredSpectrumDecodedSamples(key, analysis, provider);
	return required_samples && decoded_samples < *required_samples;
}

void AppendPrefetchTiles(
	ContentViewportRequest const& request,
	std::size_t content_budget_bytes,
	std::size_t payload_budget_bytes,
	std::uint32_t spectrum_output_height,
	std::vector<ContentTileKey>& tiles) {
	if (tiles.empty() || request.prefetch_tile_count == 0)
		return;

	auto const tile_bytes = EstimateContentTileBytes(tiles.front());
	auto const payload_bytes = EstimateContentUploadPayloadBytes(
		tiles.front(),
		spectrum_output_height);
	if (tile_bytes == 0
		|| payload_bytes == 0
		|| tile_bytes > content_budget_bytes
		|| payload_bytes > payload_budget_bytes) {
		return;
	}
	auto const maximum_resident_tiles = content_budget_bytes / tile_bytes;
	auto const maximum_payload_tiles = payload_budget_bytes / payload_bytes;
	auto const maximum_complete_tiles = std::min(
		maximum_resident_tiles,
		maximum_payload_tiles);
	if (tiles.size() >= maximum_complete_tiles)
		return;

	auto const count = std::min(
		request.prefetch_tile_count,
		kMaximumContentPrefetchTileCount);
	auto const first_tile = tiles.front().tile_index;
	auto const last_tile = tiles.back().tile_index;
	for (std::uint64_t distance = 1; distance <= count; ++distance) {
		if (tiles.size() < maximum_complete_tiles
			&& last_tile <= std::numeric_limits<std::uint64_t>::max() - distance) {
			auto key = tiles.front();
			key.tile_index = last_tile + distance;
			tiles.push_back(key);
		}
		if (tiles.size() < maximum_complete_tiles && first_tile >= distance) {
			auto key = tiles.front();
			key.tile_index = first_tile - distance;
			tiles.push_back(key);
		}
	}
}

}

bool ContentAnalysisConfig::IsValid() const noexcept {
	if (!std::isfinite(milliseconds_per_pixel) || milliseconds_per_pixel <= 0.0)
		return false;
	if (kind == ContentKind::Waveform)
		return spectrum_derivation_size == 0 && spectrum_derivation_distance == 0;
	return spectrum_derivation_size >= 4
		&& spectrum_derivation_size <= 12
		&& spectrum_derivation_distance <= spectrum_derivation_size;
}

struct ContentWorker::Impl {
	explicit Impl(
		ReadyCallback ready_callback,
		FailureCallback failure_callback,
		std::size_t content_budget_bytes)
	: store(
		content_budget_bytes,
		[](ContentTileKey const& key, std::size_t bytes) {
			auto event = MakeTileEvent("cpu_evict", key);
			event.outcome = "budget";
			event.bytes = bytes;
			perf_trace::ObserveAudioContentTileEvent(event);
		})
	, payload_store(
		kDefaultUploadPayloadCacheBudgetBytes,
		[](ContentUploadPayloadKey const& key, std::size_t bytes) {
			auto event = MakePayloadEvent("payload_evict", key);
			event.outcome = "budget";
			event.bytes = bytes;
			perf_trace::ObserveAudioContentTileEvent(event);
		})
	, content_budget_bytes(std::max<std::size_t>(1, content_budget_bytes))
	, ready_callback(std::move(ready_callback))
	, failure_callback(std::move(failure_callback)) {
		analysis_metrics.configured_spectrum_budget_bytes = spectrum_analysis_budget_bytes;
	}

	mutable std::mutex mutex;
	std::mutex budget_control_mutex;
	std::condition_variable wake;
	ContentTileStore store;
	ContentUploadPayloadStore payload_store;
	std::size_t content_budget_bytes = kDefaultContentCacheBudgetBytes;
	std::size_t payload_budget_bytes = kDefaultUploadPayloadCacheBudgetBytes;
	std::uint64_t content_budget_revision = 1;
	std::size_t spectrum_analysis_budget_bytes = kMinimumSpectrumAnalysisBudgetBytes;
	std::uint64_t spectrum_analysis_budget_revision = 1;
	ContentAnalysisCacheMetrics analysis_metrics;
	ReadyCallback ready_callback;
	FailureCallback failure_callback;
	std::thread thread;
	agi::AudioProvider *provider = nullptr;
	ContentGeneration generation;
	ContentAnalysisConfig analysis;
	ContentWorkerMetrics metrics;
	std::optional<WorkPlan> latest;
	std::uint64_t request_serial = 0;
	std::uint64_t active_serial = 0;
	bool stop = false;

	bool IsGenerationCurrent(ContentGeneration candidate) const {
		std::lock_guard<std::mutex> lock(mutex);
		return !stop
			&& candidate == generation
			&& provider;
	}

	bool IsRequestCurrent(std::uint64_t serial, ContentGeneration candidate) const {
		std::lock_guard<std::mutex> lock(mutex);
		return !stop
			&& serial == request_serial
			&& candidate == generation
			&& provider;
	}

	bool IsContentBudgetCurrent(std::uint64_t revision) const {
		std::lock_guard<std::mutex> lock(mutex);
		return !stop && revision == content_budget_revision;
	}

	void NotifyVisiblePayloadReady(bool visible, ContentGeneration candidate) {
		if (!visible || !ready_callback || !IsGenerationCurrent(candidate))
			return;
		ready_callback(candidate);
		std::lock_guard<std::mutex> lock(mutex);
		++metrics.ready_notifications;
	}

	void NotifyFailure(ContentWorkerFailure failure, bool visible, std::string message) {
		{
			std::scoped_lock lock(mutex);
			++metrics.builds_invalid;
		}
		// A speculative tile failure must not replace healthy visible content.
		if (!visible || !failure_callback || !IsRequestCurrent(failure.request_serial, failure.generation))
			return;
		failure.message = std::move(message);
		failure_callback(std::move(failure));
	}

	void PublishAnalysisMetrics(ContentAnalyzer const& analyzer) {
		auto snapshot = analyzer.Metrics();
		std::lock_guard<std::mutex> lock(mutex);
		snapshot.configured_spectrum_budget_bytes = spectrum_analysis_budget_bytes;
		analysis_metrics = snapshot;
	}

	void StopAndJoin() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = true;
			latest.reset();
			++request_serial;
			metrics.request_pending = false;
		}
		wake.notify_all();
		if (thread.joinable())
			thread.join();
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = false;
			active_serial = 0;
			metrics.build_active = false;
			analysis_metrics = {};
			analysis_metrics.configured_spectrum_budget_bytes = spectrum_analysis_budget_bytes;
		}
	}

	std::unique_ptr<AudioDisplaySource> CreateSource(
		agi::AudioProvider *worker_provider,
		ContentSourceMode mode) const {
		if (mode == ContentSourceMode::Int16Mono)
			return CreateInt16MonoAudioDisplaySource(worker_provider);
		return CreateAudioDisplaySource(worker_provider);
	}

	void WorkerLoop(agi::AudioProvider *worker_provider) noexcept {
		std::unique_ptr<AudioDisplaySource> source;
		std::unique_ptr<ContentAnalyzer> analyzer;
		std::optional<ContentSourceMode> source_mode;
		std::uint64_t applied_budget_revision = 0;

		for (;;) {
			std::optional<WorkPlan> plan;
			ContentWorkerFailure failure;
			bool visible = true;
			std::size_t requested_analysis_budget = 0;
			std::uint64_t requested_budget_revision = 0;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&] {
					return stop
						|| latest.has_value()
						|| spectrum_analysis_budget_revision != applied_budget_revision;
				});
				if (stop)
					break;
				failure.generation = generation;
				failure.request_serial = request_serial;
				requested_analysis_budget = spectrum_analysis_budget_bytes;
				requested_budget_revision = spectrum_analysis_budget_revision;
				if (latest) {
					plan = std::move(*latest);
					latest.reset();
					active_serial = plan->serial;
					metrics.request_pending = false;
					metrics.build_active = true;
				}
			}

			try {
				if (plan && (!source_mode || *source_mode != plan->analysis.source_mode)) {
					analyzer.reset();
					source = CreateSource(worker_provider, plan->analysis.source_mode);
					analyzer = source ? std::make_unique<ContentAnalyzer>(*source) : nullptr;
					source_mode = plan->analysis.source_mode;
					applied_budget_revision = 0;
				}
				if (analyzer && applied_budget_revision != requested_budget_revision) {
					analyzer->SetSpectrumCacheBudget(requested_analysis_budget);
					applied_budget_revision = requested_budget_revision;
					PublishAnalysisMetrics(*analyzer);
				}
				else if (!analyzer) {
					applied_budget_revision = requested_budget_revision;
				}
				if (!plan)
					continue;

				for (std::size_t tile_offset = 0; tile_offset < plan->tiles.size(); ++tile_offset) {
					visible = tile_offset < plan->visible_tile_count;
					{
						std::lock_guard<std::mutex> lock(mutex);
						requested_analysis_budget = spectrum_analysis_budget_bytes;
						requested_budget_revision = spectrum_analysis_budget_revision;
					}
					if (analyzer && applied_budget_revision != requested_budget_revision) {
						analyzer->SetSpectrumCacheBudget(requested_analysis_budget);
						applied_budget_revision = requested_budget_revision;
						PublishAnalysisMetrics(*analyzer);
					}
					if (tile_offset >= plan->visible_tile_count
						&& !IsContentBudgetCurrent(plan->content_budget_revision)) {
						break;
					}

					auto const& key = plan->tiles[tile_offset];
					if (!IsRequestCurrent(plan->serial, plan->generation))
						break;
					auto const payload_key = MakeContentUploadPayloadKey(
						key,
						plan->spectrum_band_plan.get());
					if (!payload_key.variant_revision) {
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.payload_builds_invalid;
						continue;
					}
					// A payload can have been published by an older viewport request
					// while this request was waiting in the queue. The data is ready,
					// but the old request deliberately did not notify for its prefetch
					// tile. Notify for the visible cache hit so the display repaints and
					// picks up the now-complete viewport.
					if (payload_store.Find(payload_key)) {
						NotifyVisiblePayloadReady(visible, plan->generation);
						continue;
					}

					auto const trace_tile = perf_trace::IsCategoryEnabled(perf_trace::Category::Audio);
					auto tile = store.Find(key);
					if (!tile && TileWaitsForDecodedSamples(key, plan->analysis, *worker_provider)) {
						{
							std::lock_guard<std::mutex> lock(mutex);
							++metrics.decode_deferred_tiles;
						}
						if (trace_tile) {
							auto event = MakeTileEvent("worker_decode_deferred", key);
							event.outcome = "decoded_frontier";
							event.request_serial = plan->serial;
							event.visible = visible ? 1 : 0;
							perf_trace::ObserveAudioContentTileEvent(event);
						}
						continue;
					}
					if (!tile) {
						{
							std::lock_guard<std::mutex> lock(mutex);
							++metrics.builds_started;
						}
						ContentAnalysisCacheMetrics analysis_before;
						if (trace_tile && analyzer && plan->analysis.kind == ContentKind::Spectrum)
							analysis_before = analyzer->Metrics();
						if (trace_tile) {
							auto event = MakeTileEvent("worker_build_start", key);
							event.request_serial = plan->serial;
							event.visible = visible ? 1 : 0;
							perf_trace::ObserveAudioContentTileEvent(event);
						}

						ContentBuildResult built;
						perf_trace::AudioUiDurationScope build_trace(
							"audio_display.content_build", static_cast<int>(plan->analysis.kind));
						if (analyzer && plan->analysis.kind == ContentKind::Waveform) {
							WaveformBuildRequest request;
							request.key = key;
							request.milliseconds_per_pixel = plan->analysis.milliseconds_per_pixel;
							request.mix_policy = plan->analysis.mix_policy;
							built = analyzer->BuildWaveform(request, [this](ContentGeneration value) {
								return IsGenerationCurrent(value);
							});
						}
						else if (analyzer) {
							SpectrumBuildRequest request;
							request.key = key;
							request.milliseconds_per_pixel = plan->analysis.milliseconds_per_pixel;
							request.mix_policy = plan->analysis.mix_policy;
							request.channel_mode = plan->analysis.spectrum_channel_mode;
							request.derivation_size = plan->analysis.spectrum_derivation_size;
							request.derivation_distance = plan->analysis.spectrum_derivation_distance;
							built = analyzer->BuildSpectrum(request, [this](ContentGeneration value) {
								return IsGenerationCurrent(value);
							});
						}
						build_trace.SetDetails(
							static_cast<int>(plan->analysis.kind), static_cast<int>(built.status));
						if (trace_tile) {
							auto event = MakeTileEvent("worker_build_end", key);
							event.outcome = BuildStatusName(built.status);
							event.request_serial = plan->serial;
							event.visible = visible ? 1 : 0;
							event.bytes = built.tile ? built.tile->DataBytes() : 0;
							if (analyzer && plan->analysis.kind == ContentKind::Spectrum) {
								auto const analysis_after = analyzer->Metrics();
								event.include_fft_deltas = true;
								event.fft_cache_hits_delta = CounterDelta(
									analysis_after.spectrum_cache_hits,
									analysis_before.spectrum_cache_hits);
								event.fft_cache_misses_delta = CounterDelta(
									analysis_after.spectrum_cache_misses,
									analysis_before.spectrum_cache_misses);
								event.fft_visible_builds_delta = CounterDelta(
									analysis_after.spectrum_visible_builds,
									analysis_before.spectrum_visible_builds);
								event.fft_cache_evictions_delta = CounterDelta(
									analysis_after.spectrum_cache_evictions,
									analysis_before.spectrum_cache_evictions);
							}
							perf_trace::ObserveAudioContentTileEvent(event);
						}

						if (built.status == ContentBuildStatus::Cancelled) {
							std::lock_guard<std::mutex> lock(mutex);
							++metrics.builds_cancelled;
							break;
						}
						if (built.status != ContentBuildStatus::Ready || !built.tile) {
							std::lock_guard<std::mutex> lock(mutex);
							++metrics.builds_invalid;
							continue;
						}

						tile = built.tile;
						if (trace_tile && key.kind == ContentKind::Spectrum && TileDiagnosticsEnabled()) {
							ObserveTileSummary(MakeTileEvent("raw_summary", key),
											   SummarizeSpectrumTile(*tile), plan->serial, visible);
						}
						auto const tile_bytes = tile->DataBytes();
						auto const published = store.Publish(tile);
						if (trace_tile) {
							auto event = MakeTileEvent("cpu_publish", key);
							event.outcome = PublishResultName(published);
							event.request_serial = plan->serial;
							event.visible = visible ? 1 : 0;
							event.bytes = tile_bytes;
							perf_trace::ObserveAudioContentTileEvent(event);
						}
						if (published != ContentPublishResult::Accepted)
							continue;
						{
							std::lock_guard<std::mutex> lock(mutex);
							++metrics.builds_ready;
						}
						if (analyzer
							&& plan->analysis.kind == ContentKind::Spectrum
							&& trace_tile) {
							PublishAnalysisMetrics(*analyzer);
						}
					}

					{
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.payload_builds_started;
					}
					if (trace_tile) {
						auto event = MakePayloadEvent("payload_build_start", payload_key);
						event.request_serial = plan->serial;
						event.visible = visible ? 1 : 0;
						perf_trace::ObserveAudioContentTileEvent(event);
					}
					ContentUploadPayloadBuildResult payload_built;
					perf_trace::AudioUiDurationScope payload_trace(
						"audio_display.payload_build", static_cast<int>(plan->analysis.kind));
					if (plan->analysis.kind == ContentKind::Waveform) {
						payload_built = BuildWaveformUploadPayload(
							*tile,
							[this](ContentGeneration value) { return IsGenerationCurrent(value); });
					}
					else if (plan->spectrum_band_plan) {
						payload_built = BuildSpectrumUploadPayload(
							*tile,
							*plan->spectrum_band_plan,
							[this](ContentGeneration value) { return IsGenerationCurrent(value); });
					}
					payload_trace.SetDetails(
						static_cast<int>(plan->analysis.kind),
						static_cast<int>(payload_built.status));
					if (trace_tile) {
						auto event = MakePayloadEvent("payload_build_end", payload_key);
						event.outcome = PayloadBuildStatusName(payload_built.status);
						event.request_serial = plan->serial;
						event.visible = visible ? 1 : 0;
						event.bytes = payload_built.payload ? payload_built.payload->DataBytes() : 0;
						perf_trace::ObserveAudioContentTileEvent(event);
					}
					if (payload_built.status == ContentUploadPayloadBuildStatus::Cancelled) {
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.payload_builds_cancelled;
						break;
					}
					if (payload_built.status != ContentUploadPayloadBuildStatus::Ready
						|| !payload_built.payload) {
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.payload_builds_invalid;
						continue;
					}

					if (trace_tile && key.kind == ContentKind::Spectrum && TileDiagnosticsEnabled()) {
						ObserveTileSummary(MakePayloadEvent("payload_summary", payload_key),
										   SummarizeUploadPayload(*payload_built.payload), plan->serial, visible);
					}
					auto const payload_bytes = payload_built.payload->DataBytes();
					auto const payload_published = payload_store.Publish(
						std::move(payload_built.payload));
					if (trace_tile) {
						auto event = MakePayloadEvent("payload_publish", payload_key);
						event.outcome = PublishResultName(payload_published);
						event.request_serial = plan->serial;
						event.visible = visible ? 1 : 0;
						event.bytes = payload_bytes;
						perf_trace::ObserveAudioContentTileEvent(event);
					}
					if (payload_published != ContentPublishResult::Accepted)
						continue;
					{
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.payload_builds_ready;
					}
					NotifyVisiblePayloadReady(visible, plan->generation);
				}
				visible = true;
				if (analyzer && plan->analysis.kind == ContentKind::Spectrum)
					PublishAnalysisMetrics(*analyzer);
			}
			catch (agi::Exception const& err) {
				NotifyFailure(std::move(failure), visible, err.GetMessage());
			}
			catch (std::exception const& err) {
				NotifyFailure(std::move(failure), visible, err.what());
			}
			catch (...) {
				NotifyFailure(std::move(failure), visible, "an unknown exception escaped the Audio content worker");
			}

			{
				std::lock_guard<std::mutex> lock(mutex);
				if (plan && active_serial == plan->serial)
					active_serial = 0;
				metrics.build_active = active_serial != 0;
			}
		}
	}
};

ContentWorker::ContentWorker(
	ReadyCallback ready_callback,
	FailureCallback failure_callback,
	std::size_t content_budget_bytes)
: impl(std::make_unique<Impl>(
	std::move(ready_callback),
	std::move(failure_callback),
	content_budget_bytes)) {
}

ContentWorker::~ContentWorker() {
	impl->StopAndJoin();
}

ContentGeneration ContentWorker::SetProvider(agi::AudioProvider *provider) {
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->provider == provider)
			return impl->generation;
	}

	impl->StopAndJoin();
	ContentGeneration generation;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->provider = provider;
		impl->generation.provider = NextGeneration(impl->generation.provider);
		impl->generation.analysis = NextGeneration(impl->generation.analysis);
		generation = impl->generation;
		++impl->request_serial;
		++impl->metrics.provider_resets;
		impl->metrics.provider_attached = provider != nullptr;
	}
	impl->store.ResetGeneration(generation);
	impl->payload_store.ResetGeneration(generation);
	if (provider)
		impl->thread = std::thread([state = impl.get(), provider] { state->WorkerLoop(provider); });
	return generation;
}

ContentGeneration ContentWorker::SetAnalysis(ContentAnalysisConfig config) {
	ContentGeneration generation;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->analysis == config)
			return impl->generation;
		impl->analysis = config;
		impl->generation.analysis = NextGeneration(impl->generation.analysis);
		generation = impl->generation;
		impl->latest.reset();
		++impl->request_serial;
		++impl->metrics.analysis_resets;
		impl->metrics.request_pending = false;
	}
	impl->store.ResetGeneration(generation);
	impl->payload_store.ResetGeneration(generation);
	impl->wake.notify_all();
	return generation;
}

bool ContentWorker::SetCacheBudgets(ContentCacheBudgets budgets) {
	budgets.content_bytes = std::max<std::size_t>(1, budgets.content_bytes);
	budgets.upload_payload_bytes = std::max<std::size_t>(1, budgets.upload_payload_bytes);
	budgets.spectrum_analysis_bytes = std::max<std::size_t>(1, budgets.spectrum_analysis_bytes);
	std::lock_guard<std::mutex> budget_lock(impl->budget_control_mutex);
	impl->store.SetBudget(budgets.content_bytes);
	impl->payload_store.SetBudget(budgets.upload_payload_bytes);

	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->content_budget_bytes != budgets.content_bytes) {
			impl->content_budget_bytes = budgets.content_bytes;
			impl->content_budget_revision = NextGeneration(impl->content_budget_revision);
			changed = true;
		}
		if (impl->payload_budget_bytes != budgets.upload_payload_bytes) {
			impl->payload_budget_bytes = budgets.upload_payload_bytes;
			impl->content_budget_revision = NextGeneration(impl->content_budget_revision);
			changed = true;
		}
		if (impl->spectrum_analysis_budget_bytes != budgets.spectrum_analysis_bytes) {
			impl->spectrum_analysis_budget_bytes = budgets.spectrum_analysis_bytes;
			impl->spectrum_analysis_budget_revision =
				NextGeneration(impl->spectrum_analysis_budget_revision);
			impl->analysis_metrics.configured_spectrum_budget_bytes =
				budgets.spectrum_analysis_bytes;
			changed = true;
		}
	}
	if (changed)
		impl->wake.notify_all();
	return changed;
}

ContentGeneration ContentWorker::Generation() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->generation;
}

bool ContentWorker::IsFailureCurrent(ContentWorkerFailure const& failure) const {
	return impl->IsRequestCurrent(failure.request_serial, failure.generation);
}

void ContentWorker::Request(
	ContentViewportRequest request,
	std::shared_ptr<SpectrumBandPlan const> spectrum_band_plan) {
	auto tiles = PlanVisibleContentTiles(request);
	if (tiles.empty())
		return;
	auto const visible_tile_count = tiles.size();

	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (!impl->provider
			|| !impl->analysis.IsValid()
			|| request.generation != impl->generation
			|| request.kind != impl->analysis.kind)
			return;
		if (request.kind == ContentKind::Waveform && request.spectrum_bin_count != 0)
			return;
		if (request.kind == ContentKind::Spectrum) {
			auto const bins = static_cast<std::size_t>(1) << impl->analysis.spectrum_derivation_size;
			if (request.spectrum_bin_count != bins
				|| !spectrum_band_plan
				|| !spectrum_band_plan->IsValid()
				|| spectrum_band_plan->bin_count != request.spectrum_bin_count) {
				return;
			}
		}
		AppendPrefetchTiles(
			request,
			impl->content_budget_bytes,
			impl->payload_budget_bytes,
			spectrum_band_plan
				? static_cast<std::uint32_t>(spectrum_band_plan->output_height) : 0,
			tiles);

		if (impl->latest || impl->active_serial)
			++impl->metrics.superseded_requests;
		WorkPlan plan;
		plan.serial = ++impl->request_serial;
		plan.generation = impl->generation;
		plan.analysis = impl->analysis;
		plan.visible_tile_count = visible_tile_count;
		plan.content_budget_revision = impl->content_budget_revision;
		plan.spectrum_band_plan = std::move(spectrum_band_plan);
		plan.tiles = std::move(tiles);
		impl->latest = std::move(plan);
		++impl->metrics.requests;
		impl->metrics.request_pending = true;
	}
	impl->wake.notify_one();
}

std::shared_ptr<ContentTile const> ContentWorker::Find(ContentTileKey const& key) {
	return impl->store.Find(key);
}

std::shared_ptr<ContentUploadPayload const> ContentWorker::FindPayload(
	ContentUploadPayloadKey const& key) {
	return impl->payload_store.Find(key);
}

ContentWorkerMetrics ContentWorker::Metrics() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->metrics;
}

ContentStoreMetrics ContentWorker::StoreMetrics() const {
	return impl->store.Metrics();
}

ContentStoreMetrics ContentWorker::PayloadMetrics() const {
	return impl->payload_store.Metrics();
}

ContentAnalysisCacheMetrics ContentWorker::AnalysisMetrics() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->analysis_metrics;
}

}
