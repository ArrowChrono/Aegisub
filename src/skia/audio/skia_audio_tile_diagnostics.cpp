#include "skia_audio_tile_diagnostics.h"

#include "../../audio_tile_diagnostics_enabled.h"

#include "skia_audio_content.h"
#include "skia_audio_upload_payload.h"

#include <algorithm>
#include <cmath>

namespace aegisub::skia::audio {
namespace {

constexpr std::uint64_t kHashOffset = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

std::uint64_t AppendHash(std::uint64_t hash, std::span<std::uint8_t const> bytes) noexcept {
	for (auto byte : bytes)
		hash = (hash ^ byte) * kHashPrime;
	return hash;
}

void ObserveFinite(TileDataSummary& summary, double value, bool& has_finite) noexcept {
	if (!has_finite) {
		summary.minimum = summary.maximum = value;
		has_finite = true;
	}
	else {
		summary.minimum = std::min(summary.minimum, value);
		summary.maximum = std::max(summary.maximum, value);
	}
}

}

bool TileDiagnosticsEnabled() noexcept {
	return aegisub::AudioTileDiagnosticsEnabled();
}

std::uint64_t HashDiagnosticBytes(std::span<std::uint8_t const> bytes) noexcept {
	return AppendHash(kHashOffset, bytes);
}

std::string FormatDiagnosticHash(std::uint64_t hash) {
	constexpr char digits[] = "0123456789abcdef";
	std::string result(16, '0');
	for (std::size_t i = result.size(); i > 0; --i) {
		result[i - 1] = digits[hash & 0xF];
		hash >>= 4;
	}
	return result;
}

TileDataSummary SummarizeSpectrumTile(ContentTile const& tile) noexcept {
	TileDataSummary summary;
	summary.hash = HashDiagnosticBytes({
		reinterpret_cast<std::uint8_t const *>(tile.spectrum_power.data()),
		tile.spectrum_power.size() * sizeof(float),
	});
	summary.element_count = tile.spectrum_power.size();
	bool has_finite = false;
	bool column_nonzero = false;
	for (std::size_t i = 0; i < tile.spectrum_power.size(); ++i) {
		auto const value = tile.spectrum_power[i];
		if (std::isfinite(value)) {
			ObserveFinite(summary, value, has_finite);
			column_nonzero |= value != 0.f;
		}
		else {
			++summary.nonfinite_count;
		}
		if (tile.key.spectrum_bin_count && ((i + 1) % tile.key.spectrum_bin_count == 0 || i + 1 == tile.spectrum_power.size())) {
			summary.nonzero_columns += column_nonzero ? 1 : 0;
			column_nonzero = false;
		}
	}
	return summary;
}

TileDataSummary SummarizeUploadPayload(ContentUploadPayload const& payload) noexcept {
	TileDataSummary summary;
	summary.hash = AppendHash(HashDiagnosticBytes(payload.primary), payload.secondary);
	bool has_finite = false;
	if (payload.key.tile.kind != ContentKind::Spectrum) {
		summary.element_count = payload.primary.size() + payload.secondary.size();
		for (auto byte : payload.primary)
			ObserveFinite(summary, byte, has_finite);
		for (auto byte : payload.secondary)
			ObserveFinite(summary, byte, has_finite);
		return summary;
	}

	summary.element_count = payload.primary.size() / 4;
	for (std::size_t x = 0; x < std::min<std::size_t>(payload.width, summary.element_count); ++x) {
		bool column_nonzero = false;
		for (std::size_t pixel = x; pixel < summary.element_count; pixel += payload.width) {
			auto const *rgba = payload.primary.data() + pixel * 4;
			auto const power = (static_cast<std::uint32_t>(rgba[0]) << 16) | (static_cast<std::uint32_t>(rgba[1]) << 8) | rgba[2];
			ObserveFinite(summary, power, has_finite);
			column_nonzero |= power != 0;
		}
		summary.nonzero_columns += column_nonzero ? 1 : 0;
	}
	return summary;
}

}
