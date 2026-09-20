#include <main.h>

#include "../../src/skia/audio/skia_audio_content.h"
#include "../../src/skia/audio/skia_audio_tile_diagnostics.h"
#include "../../src/skia/audio/skia_audio_upload_payload.h"

#include <array>
#include <bit>
#include <limits>

namespace audio = aegisub::skia::audio;

TEST(skia_audio_tile_diagnostics, exact_byte_hash_has_known_fnv1a_value_and_fixed_width_hex) {
	std::array<std::uint8_t, 5> const bytes{0, 1, 255, 128, 42};
	EXPECT_EQ(0xf71486d3777c0b45ULL, audio::HashDiagnosticBytes(bytes));
	EXPECT_EQ(0xcbf29ce484222325ULL, audio::HashDiagnosticBytes({}));
	EXPECT_EQ("000000000000012a", audio::FormatDiagnosticHash(0x12a));
	EXPECT_EQ("ffffffffffffffff", audio::FormatDiagnosticHash(std::numeric_limits<std::uint64_t>::max()));
}

TEST(skia_audio_tile_diagnostics, spectrum_summary_counts_column_major_energy_and_signed_extrema) {
	audio::ContentTile tile;
	tile.key = {
		.generation = {.provider = 1, .analysis = 2},
		.kind = audio::ContentKind::Spectrum,
		.tile_index = 7,
		.column_count = 3,
		.spectrum_bin_count = 2,
	};
	tile.spectrum_power = {0.f, -0.f, 1.f, 4.f, -2.f, 0.f};
	auto const summary = audio::SummarizeSpectrumTile(tile);
	EXPECT_EQ(6u, summary.element_count);
	EXPECT_EQ(0u, summary.nonfinite_count);
	EXPECT_EQ(2u, summary.nonzero_columns);
	EXPECT_DOUBLE_EQ(-2.0, summary.minimum);
	EXPECT_DOUBLE_EQ(4.0, summary.maximum);
}

TEST(skia_audio_tile_diagnostics, spectrum_summary_preserves_float_bits_including_signed_zero_and_nan) {
	audio::ContentTile tile;
	tile.key = {
		.generation = {.provider = 1, .analysis = 2},
		.kind = audio::ContentKind::Spectrum,
		.tile_index = 7,
		.column_count = 1,
		.spectrum_bin_count = 3,
	};
	tile.spectrum_power = {0.f, 1.f, -2.f};
	auto const summary = audio::SummarizeSpectrumTile(tile);
	EXPECT_EQ(std::endian::native == std::endian::little
				  ? 0xe15a3ad3be4bd888ULL
				  : 0xcd85013d887af1caULL,
			  summary.hash);
	tile.spectrum_power[0] = -0.f;
	EXPECT_NE(summary.hash, audio::SummarizeSpectrumTile(tile).hash);
	tile.spectrum_power[0] = std::bit_cast<float>(0x7fc00001u);
	auto const first_nan_hash = audio::SummarizeSpectrumTile(tile).hash;
	tile.spectrum_power[0] = std::bit_cast<float>(0x7fc00002u);
	EXPECT_NE(first_nan_hash, audio::SummarizeSpectrumTile(tile).hash);
}

TEST(skia_audio_tile_diagnostics, spectrum_nonfinite_values_are_counted_but_not_used_as_extrema_or_energy) {
	auto const inf = std::numeric_limits<float>::infinity();
	auto const nan = std::numeric_limits<float>::quiet_NaN();
	audio::ContentTile tile;
	tile.key = {
		.generation = {.provider = 1, .analysis = 2},
		.kind = audio::ContentKind::Spectrum,
		.tile_index = 7,
		.column_count = 2,
		.spectrum_bin_count = 3,
	};
	tile.spectrum_power = {nan, inf, -inf, 0.f, 2.f, 3.f};
	auto const summary = audio::SummarizeSpectrumTile(tile);
	EXPECT_EQ(6u, summary.element_count);
	EXPECT_EQ(3u, summary.nonfinite_count);
	EXPECT_EQ(1u, summary.nonzero_columns);
	EXPECT_DOUBLE_EQ(0.0, summary.minimum);
	EXPECT_DOUBLE_EQ(3.0, summary.maximum);

	tile.key.column_count = 1;
	tile.spectrum_power = {nan, inf, -inf};
	auto const all_nonfinite = audio::SummarizeSpectrumTile(tile);
	EXPECT_EQ(3u, all_nonfinite.nonfinite_count);
	EXPECT_EQ(0u, all_nonfinite.nonzero_columns);
	EXPECT_DOUBLE_EQ(0.0, all_nonfinite.minimum);
	EXPECT_DOUBLE_EQ(0.0, all_nonfinite.maximum);
}

TEST(skia_audio_tile_diagnostics, empty_spectrum_and_zero_energy_remain_distinguishable_by_hash_and_count) {
	audio::ContentTile tile;
	auto const empty = audio::SummarizeSpectrumTile(tile);
	EXPECT_EQ(0xcbf29ce484222325ULL, empty.hash);
	EXPECT_EQ(0u, empty.element_count);
	EXPECT_EQ(0u, empty.nonfinite_count);
	EXPECT_EQ(0u, empty.nonzero_columns);
	EXPECT_DOUBLE_EQ(0.0, empty.minimum);
	EXPECT_DOUBLE_EQ(0.0, empty.maximum);
	tile.key = {
		.generation = {.provider = 1, .analysis = 2},
		.kind = audio::ContentKind::Spectrum,
		.tile_index = 7,
		.column_count = 2,
		.spectrum_bin_count = 2,
	};
	tile.spectrum_power.assign(4, 0.f);
	auto const zero = audio::SummarizeSpectrumTile(tile);
	EXPECT_NE(empty.hash, zero.hash);
	EXPECT_EQ(4u, zero.element_count);
	EXPECT_EQ(0u, zero.nonfinite_count);
	EXPECT_EQ(0u, zero.nonzero_columns);
	EXPECT_DOUBLE_EQ(0.0, zero.minimum);
	EXPECT_DOUBLE_EQ(0.0, zero.maximum);
}

TEST(skia_audio_tile_diagnostics, spectrum_payload_counts_rgb_power_per_column_not_opaque_alpha) {
	audio::ContentUploadPayload payload;
	payload.key = {
		.tile = {
			.generation = {.provider = 1, .analysis = 2},
			.kind = audio::ContentKind::Spectrum,
			.tile_index = 7,
			.column_count = 2,
			.spectrum_bin_count = 3,
		},
		.variant_revision = 5,
	};
	payload.width = 2;
	payload.height = 2;
	payload.primary = {
		0,
		0,
		0,
		255,
		0,
		0,
		1,
		255,
		0,
		0,
		0,
		255,
		1,
		2,
		3,
		255,
	};
	auto const summary = audio::SummarizeUploadPayload(payload);
	EXPECT_EQ(0x0d99993a9fbf49e0ULL, summary.hash);
	EXPECT_EQ(4u, summary.element_count);
	EXPECT_EQ(0u, summary.nonfinite_count);
	EXPECT_EQ(1u, summary.nonzero_columns);
	EXPECT_DOUBLE_EQ(0.0, summary.minimum);
	EXPECT_DOUBLE_EQ(66051.0, summary.maximum);

	// Alpha contributes to byte identity, but not power or energized columns.
	payload.primary[3] = 0;
	auto const transparent = audio::SummarizeUploadPayload(payload);
	EXPECT_NE(summary.hash, transparent.hash);
	EXPECT_EQ(summary.nonzero_columns, transparent.nonzero_columns);
	EXPECT_DOUBLE_EQ(summary.maximum, transparent.maximum);
	payload.primary[6] = 0;
	payload.primary[12] = payload.primary[13] = payload.primary[14] = 0;
	auto const black = audio::SummarizeUploadPayload(payload);
	EXPECT_EQ(0u, black.nonzero_columns);
	EXPECT_DOUBLE_EQ(0.0, black.minimum);
	EXPECT_DOUBLE_EQ(0.0, black.maximum);

	payload.secondary.push_back(42);
	EXPECT_NE(black.hash, audio::SummarizeUploadPayload(payload).hash);
}

TEST(skia_audio_tile_diagnostics, waveform_payload_reports_bytes_without_interpreting_them_as_spectrum) {
	audio::ContentUploadPayload payload;
	payload.primary = {128, 1};
	payload.secondary = {255, 0};
	auto const summary = audio::SummarizeUploadPayload(payload);
	EXPECT_EQ(4u, summary.element_count);
	EXPECT_EQ(0u, summary.nonzero_columns);
	EXPECT_DOUBLE_EQ(0.0, summary.minimum);
	EXPECT_DOUBLE_EQ(255.0, summary.maximum);
}
