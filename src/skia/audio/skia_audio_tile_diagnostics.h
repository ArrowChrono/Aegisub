#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace aegisub::skia::audio {

struct ContentTile;
struct ContentUploadPayload;

// Opt-in diagnostics only; this does not enable performance tracing itself.
bool TileDiagnosticsEnabled() noexcept;

struct TileDataSummary {
	std::uint64_t hash = 0;
	std::uint64_t element_count = 0;
	std::uint64_t nonfinite_count = 0;
	std::uint64_t nonzero_columns = 0;
	double minimum = 0;
	double maximum = 0;
};

// FNV-1a over the exact bytes, including float bit patterns and texture alpha.
std::uint64_t HashDiagnosticBytes(std::span<std::uint8_t const> bytes) noexcept;
std::string FormatDiagnosticHash(std::uint64_t hash);

// Finite extrema only; empty/all-nonfinite input has zero extrema. Nonzero
// columns contain at least one finite nonzero value in column-major FFT data.
TileDataSummary SummarizeSpectrumTile(ContentTile const& tile) noexcept;

// Spectrum extrema/counts describe packed RGB24 powers, not alpha bytes.
// Waveform payloads report byte extrema/counts without spectrum column counts.
// Both payload vectors participate in the exact-byte hash.
TileDataSummary SummarizeUploadPayload(ContentUploadPayload const& payload) noexcept;

}
