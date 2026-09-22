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

#include "async_video_provider.h"

#include "ass_fixstyle_core.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_time_projection.h"
#include "align_video_fade.h"
#include "async_video_trace.h"
#include "include/aegisub/subtitles_provider.h"
#include "key_point_color.h"
#include "motion_track/gray_convert.h"
#include "source_frame.h"
#include "subtitle_overlay.h"
#include "subtitle_overlay_blend.h"
#include "video_frame.h"
#include "video_memory_stats.h"
#include "video_provider_manager.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

enum {
	NEW_SUBS_FILE = -1,
	SUBS_FILE_ALREADY_LOADED = -2
};

namespace {
constexpr char const *kSourceModeLogTag = "video/source/mode";
constexpr char const *kSubtitleProviderUseLogTag = "subtitle/provider/use";
std::atomic<std::uint64_t> next_provider_delivery_version{1};

void MergeSubtitleUpdateOptions(
	VideoSubtitleUpdateOptions &pending,
	VideoSubtitleUpdateOptions incoming) noexcept {
	// Delivery class follows the newest subtitle operation. Force rendering is
	// sticky until this pending work is captured, so a later line update cannot
	// erase a final-frame guarantee from the same coalesced work.
	pending.delivery_class = incoming.delivery_class;
	pending.visual_interaction_id = incoming.visual_interaction_id;
	pending.force_current_frame_render |= incoming.force_current_frame_render;
}

std::vector<std::pair<const AssDialogue*, int>> CaptureSubtitleSourceLines(AssFile const& file) {
	std::vector<std::pair<const AssDialogue*, int>> lines;
	for (auto const& line : file.Events)
		lines.emplace_back(&line, line.Id);
	return lines;
}

void MergePendingChangedLines(
	std::vector<AssDialogueBase>& pending,
	std::vector<AssDialogueBase> updates) {
	if (pending.empty()) {
		pending = std::move(updates);
		return;
	}

	std::vector<AssDialogueBase> merged;
	merged.reserve(pending.size() + updates.size());
	size_t pending_index = 0;
	size_t update_index = 0;
	while (pending_index < pending.size() && update_index < updates.size()) {
		if (pending[pending_index].Row < updates[update_index].Row)
			merged.push_back(std::move(pending[pending_index++]));
		else if (updates[update_index].Row < pending[pending_index].Row)
			merged.push_back(std::move(updates[update_index++]));
		else {
			merged.push_back(std::move(updates[update_index++]));
			++pending_index;
		}
	}
	while (pending_index < pending.size())
		merged.push_back(std::move(pending[pending_index++]));
	while (update_index < updates.size())
		merged.push_back(std::move(updates[update_index++]));
	pending = std::move(merged);
}

std::string FormatSourceModeList(std::vector<SourceFrameOutputMode> const& modes) {
	std::string value = "[";
	for (size_t i = 0; i < modes.size(); ++i) {
		if (i)
			value.append(", ");
		value.append(SourceFrameOutputModeName(modes[i]));
	}
	value.push_back(']');
	return value;
}

char const *SourceFrameColorRangeName(SourceFrameColorRange range) {
	switch (range) {
		case SourceFrameColorRange::Limited:
			return "Limited";
		case SourceFrameColorRange::Full:
			return "Full";
		default:
			return "Unknown";
	}
}

std::string FormatColorMetadata(SourceFrameColorMetadata const& color) {
	return std::string("matrix=")
		+ (color.matrix.empty() ? "Unknown" : color.matrix)
		+ ", primaries="
		+ (color.primaries.empty() ? "Unknown" : color.primaries)
		+ ", transfer="
		+ (color.transfer.empty() ? "Unknown" : color.transfer)
		+ ", range="
		+ SourceFrameColorRangeName(color.range);
}

char const *SubtitleRenderModeName(SubtitleRenderMode mode) {
	switch (mode) {
		case SubtitleRenderMode::CompatibilityFrameOnly:
			return "CompatibilityFrameOnly";
		case SubtitleRenderMode::PremultipliedOverlay:
			return "PremultipliedOverlay";
		default:
			return "Unknown";
	}
}

template<typename T>
std::shared_ptr<T> acquire_buffer(std::vector<std::shared_ptr<T>>& buffers) {
	for (auto& buffer : buffers) {
		if (buffer.use_count() == 1)
			return buffer;
	}

	auto buffer = std::make_shared<T>();
	buffers.push_back(buffer);
	return buffer;
}

std::shared_ptr<VideoFrame> BakePacketForCpuReadbackImpl(VideoRenderPacket const& packet) {
	auto display_frame = packet.DisplayFrame();
	if (!display_frame)
		return nullptr;

	if (!packet.has_subtitle_overlay || !packet.subtitle_overlay.IsValid())
		return display_frame;

	auto baked = std::make_shared<VideoFrame>(*display_frame);
	if (packet.subtitle_overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8)
		return baked;

	if (packet.subtitle_overlay.composition_mode == SubtitleOverlayCompositionMode::PremultipliedAlpha
		&& packet.subtitle_overlay.premultiplied_alpha) {
		CompositePremultipliedBgraOverlayOntoVideoFrame(*baked, packet.subtitle_overlay);
	}
	else if (packet.subtitle_overlay.composition_mode == SubtitleOverlayCompositionMode::OpaqueReplace) {
		CompositeOpaqueBgraOverlayOntoVideoFrame(*baked, packet.subtitle_overlay);
	}
	return baked;
}

template<typename T>
void TrimReusableBufferPool(std::vector<std::shared_ptr<T>>& buffers) {
	if (buffers.size() <= 1)
		return;

	buffers.erase(
		std::remove_if(
			buffers.begin(),
			buffers.end(),
			[](std::shared_ptr<T> const& buffer) {
				return buffer && buffer.use_count() == 1;
			}),
		buffers.end());
}

struct KeyPointBounds {
	int left = 0;
	int right = 0;
	int up = 0;
	int down = 0;
};

// Thunk-side marker: records which thread is currently executing a
// synchronous worker section so public entries can reject same-thread
// reentrancy before deadlocking inside Queue::Sync.
class WorkerSyncTracker {
public:
	explicit WorkerSyncTracker(AsyncVideoProvider const& provider)
	: provider(provider) {
		std::lock_guard<std::mutex> lock(provider.sync_state.mutex);
		provider.sync_state.owner = std::this_thread::get_id();
		++provider.sync_state.depth;
	}
	~WorkerSyncTracker() {
		std::lock_guard<std::mutex> lock(provider.sync_state.mutex);
		if (--provider.sync_state.depth == 0)
			provider.sync_state.owner = std::thread::id{};
	}

private:
	AsyncVideoProvider const& provider;
};

// Retain only pixels used by alignment: the local template and the complete
// anchor row/column. The latter preserve exact bounds for strokes larger than
// the template ROI. Coordinates are normalized once to logical video rows.
struct KeyPointPixels {
	static constexpr int horizontal_radius = 48;
	static constexpr int vertical_radius = 32;
	int width;
	int height;
	int anchor_y;
	int left;
	int top;
	int roi_width;
	int roi_height;
	std::vector<unsigned char> data;

	KeyPointPixels(VideoFrame const& source, int x, int y)
		: width(static_cast<int>(source.width)), height(static_cast<int>(source.height)), anchor_y(y), left(std::max(0, x - horizontal_radius)), top(std::max(0, y - vertical_radius)), roi_width(std::min(width - 1, x + horizontal_radius) - left + 1), roi_height(std::min(height - 1, y + vertical_radius) - top + 1), data((static_cast<size_t>(roi_width) * roi_height + width + height) * 4) {
		auto source_row = [&](int row) {
			int const physical = aegisub::motion_track::LogicalRowToPhysicalRow(row, height, source.flipped);
			return source.data.data() + static_cast<size_t>(physical) * source.pitch;
		};
		for (int row = 0; row < roi_height; ++row)
			std::copy_n(source_row(top + row) + static_cast<size_t>(left) * 4,
						static_cast<size_t>(roi_width) * 4, data.data() + static_cast<size_t>(row) * roi_width * 4);
		auto *row_pixels = data.data() + static_cast<size_t>(roi_width) * roi_height * 4;
		std::copy_n(source_row(y), static_cast<size_t>(width) * 4, row_pixels);
		auto *column_pixels = row_pixels + static_cast<size_t>(width) * 4;
		for (int row = 0; row < height; ++row)
			std::copy_n(source_row(row) + static_cast<size_t>(x) * 4, 4, column_pixels + static_cast<size_t>(row) * 4);
	}

	[[nodiscard]] unsigned char const *Pixel(int x, int y) const {
		if (x >= left && x < left + roi_width && y >= top && y < top + roi_height)
			return data.data() + (static_cast<size_t>(y - top) * roi_width + x - left) * 4;
		auto const *row_pixels = data.data() + static_cast<size_t>(roi_width) * roi_height * 4;
		if (y == anchor_y)
			return row_pixels + static_cast<size_t>(x) * 4;
		return row_pixels + (static_cast<size_t>(width) + y) * 4;
	}
};

bool NormalizeFrameY(KeyPointPixels const& frame, int y, int& normalized_y) {
	int const height = static_cast<int>(frame.height);
	if (y < 0 || y >= height)
		return false;

	normalized_y = y;
	return normalized_y >= 0 && normalized_y < height;
}

unsigned char const *GetFramePixel(KeyPointPixels const& frame, int x, int y) {
	return frame.Pixel(x, y);
}

bool KeyPointPixelMatches(
	KeyPointPixels const& frame,
	int x,
	int y,
	aegisub::keypoint::ColorMatcher& matcher) {
	auto const* pixel = GetFramePixel(frame, x, y);
	return matcher.Matches(pixel[0], pixel[1], pixel[2]);
}

bool CalculateKeyPointBounds(
	KeyPointPixels const& frame,
	int x,
	int y,
	aegisub::keypoint::ColorMatcher& matcher,
	KeyPointBounds& bounds) {
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	if (x < 0 || x >= width)
		return false;

	int normalized_y = 0;
	if (!NormalizeFrameY(frame, y, normalized_y))
		return false;

	if (!KeyPointPixelMatches(frame, x, normalized_y, matcher))
		return false;

	int left = x;
	while (left > 0 && KeyPointPixelMatches(frame, left - 1, normalized_y, matcher))
		--left;

	int right = x;
	while (right + 1 < width && KeyPointPixelMatches(frame, right + 1, normalized_y, matcher))
		++right;

	int up = normalized_y;
	while (up > 0 && KeyPointPixelMatches(frame, x, up - 1, matcher))
		--up;

	int down = normalized_y;
	while (down + 1 < height && KeyPointPixelMatches(frame, x, down + 1, matcher))
		++down;

	bounds = { left, right, up, down };
	return true;
}

bool MatchesKeyPointBoundsWithinTolerance(
	KeyPointPixels const& frame,
	int x,
	int y,
	aegisub::keypoint::ColorMatcher& matcher,
	KeyPointBounds const& anchor_bounds,
	int bounds_tolerance) {
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	if (x < 0 || x >= width)
		return false;

	int normalized_y = 0;
	if (!NormalizeFrameY(frame, y, normalized_y))
		return false;

	if (!KeyPointPixelMatches(frame, x, normalized_y, matcher))
		return false;

	auto const matches = [&](int px, int py) {
		return KeyPointPixelMatches(frame, px, py, matcher);
	};

	int const min_left = std::max(0, anchor_bounds.left - bounds_tolerance);
	int const max_left = std::min(width - 1, anchor_bounds.left + bounds_tolerance);
	int left = x;
	while (left > min_left && matches(left - 1, normalized_y))
		--left;
	if (left > max_left)
		return false;
	if (left == min_left && min_left > 0 && matches(min_left - 1, normalized_y))
		return false;

	int const min_right = std::max(0, anchor_bounds.right - bounds_tolerance);
	int const max_right = std::min(width - 1, anchor_bounds.right + bounds_tolerance);
	int right = x;
	while (right < max_right && matches(right + 1, normalized_y))
		++right;
	if (right < min_right)
		return false;
	if (right == max_right && max_right + 1 < width && matches(max_right + 1, normalized_y))
		return false;

	int const min_up = std::max(0, anchor_bounds.up - bounds_tolerance);
	int const max_up = std::min(height - 1, anchor_bounds.up + bounds_tolerance);
	int up = normalized_y;
	while (up > min_up && matches(x, up - 1))
		--up;
	if (up > max_up)
		return false;
	if (up == min_up && min_up > 0 && matches(x, min_up - 1))
		return false;

	int const min_down = std::max(0, anchor_bounds.down - bounds_tolerance);
	int const max_down = std::min(height - 1, anchor_bounds.down + bounds_tolerance);
	int down = normalized_y;
	while (down < max_down && matches(x, down + 1))
		++down;
	if (down < min_down)
		return false;
	if (down == max_down && max_down + 1 < height && matches(x, max_down + 1))
		return false;

	return true;
}

struct FadeTemplateSample {
	int foreground_x = 0;
	int foreground_y = 0;
	int neighbour_x = 0;
	int neighbour_y = 0;
	std::array<double, 3> foreground{};
	double energy = 0.0;
};

std::vector<FadeTemplateSample> BuildFadeTemplate(
	KeyPointPixels const& frame,
	int x,
	int y,
	aegisub::keypoint::ColorMatcher& matcher,
	double core_tolerance = 1.0) {
	constexpr int horizontal_radius = KeyPointPixels::horizontal_radius;
	constexpr int vertical_radius = KeyPointPixels::vertical_radius;
	constexpr size_t maximum_samples = 96;
	constexpr double minimum_energy = 64.0;

	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	int normalized_y = 0;
	if (x < 0 || x >= width || !NormalizeFrameY(frame, y, normalized_y))
		return {};

	int const left = std::max(0, x - horizontal_radius);
	int const right = std::min(width - 1, x + horizontal_radius);
	int const top = std::max(0, normalized_y - vertical_radius);
	int const bottom = std::min(height - 1, normalized_y + vertical_radius);
	int const roi_width = right - left + 1;
	int const roi_height = bottom - top + 1;
	// The user's search tolerance also accepts pale backgrounds. Build the
	// shape from the core color of the clicked stroke, not that wider palette.
	auto const *anchor = GetFramePixel(frame, x, normalized_y);
	aegisub::keypoint::ColorMatcher core_matcher(
		anchor[0], anchor[1], anchor[2], core_tolerance * core_tolerance);
	std::vector<unsigned char> matches(static_cast<size_t>(roi_width) * roi_height, 0);
	auto mask_at = [&](int px, int py) -> unsigned char& {
		return matches[static_cast<size_t>(py - top) * roi_width + (px - left)];
	};
	for (int py = top; py <= bottom; ++py) {
		for (int px = left; px <= right; ++px)
			mask_at(px, py) = KeyPointPixelMatches(frame, px, py, matcher) && KeyPointPixelMatches(frame, px, py, core_matcher);
	}
	// Restrict foreground samples to the connected stroke containing the
	// click. Other similarly colored objects in the ROI are not anchors.
	std::vector<std::array<int, 2>> connected = {{x, normalized_y}};
	mask_at(x, normalized_y) = 2;
	for (size_t i = 0; i < connected.size(); ++i) {
		auto const [px, py] = connected[i];
		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {
				int const nx = px + dx;
				int const ny = py + dy;
				if (nx >= left && nx <= right && ny >= top && ny <= bottom && mask_at(nx, ny) == 1) {
					mask_at(nx, ny) = 2;
					connected.push_back({nx, ny});
				}
			}
		}
	}

	constexpr std::array<std::array<int, 2>, 16> offsets = {{
		{{ -1, 0 }}, {{ 1, 0 }}, {{ 0, -1 }}, {{ 0, 1 }},
		{{ -1, -1 }}, {{ 1, -1 }}, {{ -1, 1 }}, {{ 1, 1 }},
		{{ -2, 0 }}, {{ 2, 0 }}, {{ 0, -2 }}, {{ 0, 2 }},
		{{ -2, -2 }}, {{ 2, -2 }}, {{ -2, 2 }}, {{ 2, 2 }}
	}};
	std::vector<FadeTemplateSample> candidates;
	for (int py = top; py <= bottom; ++py) {
		for (int px = left; px <= right; ++px) {
			if (mask_at(px, py) != 2)
				continue;

			auto const* foreground = GetFramePixel(frame, px, py);
			FadeTemplateSample best;
			for (auto const& offset : offsets) {
				int const neighbour_x = px + offset[0];
				int const neighbour_y = py + offset[1];
				if (neighbour_x < left || neighbour_x > right
					|| neighbour_y < top || neighbour_y > bottom
					|| mask_at(neighbour_x, neighbour_y))
					continue;

				auto const* neighbour = GetFramePixel(frame, neighbour_x, neighbour_y);
				std::array<double, 3> difference = {
					static_cast<double>(foreground[0]) - neighbour[0],
					static_cast<double>(foreground[1]) - neighbour[1],
					static_cast<double>(foreground[2]) - neighbour[2]
				};
				double const energy = difference[0] * difference[0]
					+ difference[1] * difference[1]
					+ difference[2] * difference[2];
				if (energy > best.energy)
					best = {
						px,
						py,
						neighbour_x,
						neighbour_y,
						{
							static_cast<double>(foreground[0]),
							static_cast<double>(foreground[1]),
							static_cast<double>(foreground[2])
						},
						energy
					};
			}
			if (best.energy >= minimum_energy)
				candidates.push_back(best);
		}
	}

	std::sort(candidates.begin(), candidates.end(), [](auto const& left, auto const& right) {
		return left.energy > right.energy;
	});
	std::vector<FadeTemplateSample> selected;
	selected.reserve(std::min(maximum_samples, candidates.size()));
	for (auto const& candidate : candidates) {
		bool const too_close = std::any_of(selected.begin(), selected.end(), [&](auto const& existing) {
			int const dx = existing.foreground_x - candidate.foreground_x;
			int const dy = existing.foreground_y - candidate.foreground_y;
			return dx * dx + dy * dy < 4;
		});
		if (!too_close)
			selected.push_back(candidate);
		if (selected.size() == maximum_samples)
			break;
	}
	if (selected.size() < 4) {
		selected.assign(
			candidates.begin(),
			candidates.begin() + std::min(maximum_samples, candidates.size()));
	}
	return selected;
}

struct FadeVisibility {
	double level = 0.0;
	double shape_support = 0.0;
	bool valid = false;
};

FadeVisibility FadeVisibilityScore(
	KeyPointPixels const& frame,
	std::vector<FadeTemplateSample> const& samples) {
	constexpr size_t maximum_samples = 96;
	std::array<double, maximum_samples> ratios{};
	size_t ratio_count = 0;
	for (auto const& sample : samples) {
		if (ratio_count == ratios.size())
			break;
		auto const* foreground = GetFramePixel(frame, sample.foreground_x, sample.foreground_y);
		auto const* neighbour = GetFramePixel(frame, sample.neighbour_x, sample.neighbour_y);
		std::array<double, 3> const expected_difference = {
			sample.foreground[0] - neighbour[0],
			sample.foreground[1] - neighbour[1],
			sample.foreground[2] - neighbour[2]
		};
		double const expected_energy =
			expected_difference[0] * expected_difference[0]
			+ expected_difference[1] * expected_difference[1]
			+ expected_difference[2] * expected_difference[2];
		if (expected_energy < 64.0)
			continue;
		double const dot =
			(static_cast<double>(foreground[0]) - neighbour[0]) * expected_difference[0]
			+ (static_cast<double>(foreground[1]) - neighbour[1]) * expected_difference[1]
			+ (static_cast<double>(foreground[2]) - neighbour[2]) * expected_difference[2];
		// Recompute the fully-visible contrast against this frame's local
		// background. This removes temporal background drift from the alpha
		// estimate: an opaque key point remains at 1.0 even when neighbouring
		// pixels fluctuate due to motion, grain, or compression noise.
		// Keep enough headroom to recognize that the user's anchor frame was
		// itself only partially visible. A later fully-visible plateau can have
		// substantially more contrast than that initial template.
		ratios[ratio_count++] = std::clamp(dot / expected_energy, -1.0, 8.0);
	}
	if (ratio_count < 4)
		return {};

	auto begin = ratios.begin();
	auto end = begin + ratio_count;
	// A few background edges can have the right contrast after the glyph is
	// gone. Continuing the range requires support from most of the stroke.
	auto const middle = begin + ratio_count / 2;
	std::nth_element(begin, middle, end);
	double result = *middle;
	if (ratio_count % 2 == 0) {
		auto const lower = std::max_element(begin, middle);
		result = (*lower + result) * 0.5;
	}
	auto const lower_quartile = begin + ratio_count / 4;
	std::nth_element(begin, lower_quartile, middle);
	return {.level = result, .shape_support = *lower_quartile, .valid = true};
}
}

std::shared_ptr<VideoFrame> BakePacketForCpuReadback(VideoRenderPacket const& packet) {
	return BakePacketForCpuReadbackImpl(packet);
}

void AsyncVideoProvider::ResetCachedSourceFrame() noexcept {
	cached_source_frame_number = -1;
	cached_source_mode = SourceFrameOutputMode::Bgra8;
	cached_source_force_bgra = false;
	cached_source_frame = { };
	cached_source_frame_storage.reset();
	cached_source_frame_owner.reset();
}

bool AsyncVideoProvider::CanReuseCachedSourceFrame(int frame, bool raw, bool force_bgra_frame) const noexcept {
	if (raw || !subs_provider || !subs)
		return false;
	if (!cached_source_frame.IsValid())
		return false;
	if (cached_source_frame_number != frame)
		return false;
	if (cached_source_mode != selected_source_mode)
		return false;
	if (cached_source_force_bgra != force_bgra_frame)
		return false;
	if (cached_source_frame.output_mode == SourceFrameOutputMode::Native && !cached_source_frame_owner)
		return false;
	return true;
}

void AsyncVideoProvider::ReuseCachedSourceFrame(VideoRenderPacket& packet, std::shared_ptr<VideoFrame>& frame) const {
	packet.source_frame_storage = cached_source_frame_storage;
	packet.source_frame_owner = cached_source_frame_owner;
	packet.source_frame = cached_source_frame;
	frame = cached_source_frame_storage;
}

void AsyncVideoProvider::UpdateCachedSourceFrame(int frame, bool force_bgra_frame, VideoRenderPacket const& packet) noexcept {
	if (!packet.source_frame.IsValid()) {
		ResetCachedSourceFrame();
		return;
	}

	cached_source_frame_number = frame;
	cached_source_mode = selected_source_mode;
	cached_source_force_bgra = force_bgra_frame;
	cached_source_frame = packet.source_frame;
	cached_source_frame_storage = packet.source_frame_storage;
	cached_source_frame_owner = packet.source_frame_owner;
	if (!cached_source_frame_owner && cached_source_frame_storage)
		cached_source_frame_owner = cached_source_frame_storage;
}

void AsyncVideoProvider::AdvanceOverlayUploadContinuity() {
	++overlay_continuity_generation;
	if (overlay_continuity_generation == 0)
		++overlay_continuity_generation;
}

void AsyncVideoProvider::TrimReusablePools() {
	TrimReusableBufferPool(source_buffers);
	TrimReusableBufferPool(composited_buffers);
	TrimReusableBufferPool(subtitle_overlay_buffers);
}

VideoRenderPacket AsyncVideoProvider::ProcRenderPacket(int frame_number, double time, bool raw) {
	return ProcRenderPacket(frame_number, time, raw, false);
}

VideoRenderPacket AsyncVideoProvider::ProcRenderPacket(int frame_number, double time, bool raw, bool force_bgra_frame) {
	VideoRenderPacket packet;
	packet.frame_number = frame_number;
	auto const render_mode = subs_provider ? subs_provider->GetRenderMode() : SubtitleRenderMode::CompatibilityFrameOnly;

	std::shared_ptr<VideoFrame> frame;
	bool native_frame_needs_display_transform_fallback = false;
	bool const can_reuse_cached_source = CanReuseCachedSourceFrame(frame_number, raw, force_bgra_frame);
	bool used_cached_source_template_for_compatibility = false;
	if (can_reuse_cached_source && render_mode == SubtitleRenderMode::CompatibilityFrameOnly) {
		// Compatibility subtitle renderers draw directly into BGRA frames.
		// Keep a cached subtitle-free source frame and copy it into a fresh
		// buffer when subtitles change, rather than re-requesting the video.
		frame = acquire_buffer(source_buffers);
		if (cached_source_frame_storage)
			*frame = *cached_source_frame_storage;
		packet.source_frame_storage = frame;
		packet.source_frame_owner = frame;
		packet.source_frame = MakeSourceFrameView(*frame, cached_source_frame);
		used_cached_source_template_for_compatibility = true;
	}
	else if (can_reuse_cached_source) {
		ReuseCachedSourceFrame(packet, frame);
	}
	else if (selected_source_mode == SourceFrameOutputMode::Native && !force_bgra_frame) {
		try {
			if (!source_provider->GetNativeFrame(frame_number, packet.source_frame, packet.source_frame_owner))
				throw AsyncVideoProviderVideoError("Selected native source mode but provider did not return a native frame.");
		}
		catch (VideoProviderError const& err) { throw AsyncVideoProviderVideoError(err.GetMessage()); }
		if (!packet.source_frame.IsValid())
			throw AsyncVideoProviderVideoError("Provider returned an invalid native source frame.");
		native_frame_needs_display_transform_fallback =
			SourceFrameNeedsDisplayTransformFallback(packet.source_frame);
	}
	else {
		frame = acquire_buffer(source_buffers);

		try {
			source_provider->GetFrame(frame_number, *frame);
		}
		catch (VideoProviderError const& err) { throw AsyncVideoProviderVideoError(err.GetMessage()); }

		packet.source_frame_storage = frame;
		packet.source_frame_owner = frame;
		packet.source_frame = MakeSourceFrameView(*frame, source_provider->GetColorMetadata());
		packet.source_frame.geometry = source_provider->GetFrameGeometry();
		packet.source_frame.native_format = source_provider->GetNativeFormatIdentity();
	}
	packet.time = time;

	if (native_frame_needs_display_transform_fallback && !raw && subs_provider && subs) {
		frame = acquire_buffer(source_buffers);

		try {
			source_provider->GetFrame(frame_number, *frame);
		}
		catch (VideoProviderError const& err) { throw AsyncVideoProviderVideoError(err.GetMessage()); }

		packet.source_frame_storage = frame;
		packet.source_frame_owner = frame;
		packet.source_frame = MakeBakedSourceFrameView(*frame, packet.source_frame);
		packet.source_frame.native_format = source_provider->GetNativeFormatIdentity();
	}

	if (!raw && subs_provider && subs) {
		if (render_mode == SubtitleRenderMode::CompatibilityFrameOnly) {
			// Cache the subtitle-free video pixels for immediate subtitle-only rerenders.
			if (!used_cached_source_template_for_compatibility && frame) {
				if (!cached_source_frame_storage)
					cached_source_frame_storage = std::make_shared<VideoFrame>();
				*cached_source_frame_storage = *frame;
				cached_source_frame_owner = cached_source_frame_storage;
				cached_source_frame_number = frame_number;
				cached_source_mode = selected_source_mode;
				cached_source_force_bgra = force_bgra_frame;
				cached_source_frame = MakeSourceFrameView(*cached_source_frame_storage, packet.source_frame);
			}
		}
		else {
			UpdateCachedSourceFrame(frame_number, force_bgra_frame, packet);
		}
	}

	if (raw || !subs_provider || !subs) {
		packet.composited_frame_storage = frame;
		return packet;
	}

	try {
		if (single_frame != frame_number && single_frame != SUBS_FILE_ALREADY_LOADED) {
			auto const& fps = subtitles_timecodes;
			// Generally edits and seeks come in groups; if the last thing done
			// was seek it is more likely that the user will seek again and
			// vice versa. As such, if this is the first frame requested after
			// an edit, only export the currently visible lines (because the
			// other lines will probably not be viewed before the file changes
			// again), and if it's a different frame, export the entire file.
			if (single_frame != NEW_SUBS_FILE) {
				subs_provider->LoadSubtitles(subs.get(), -1, &fps);
				single_frame = SUBS_FILE_ALREADY_LOADED;
			}
			else {
				aegisub::ass_fixstyle::ReplaceMissingStylesWithDefault(subs.get());
				single_frame = frame_number;
				subs_provider->LoadSubtitles(subs.get(), static_cast<int>(time), &fps);
			}
		}
	}
	catch (agi::Exception const& err) { throw AsyncVideoProviderSubtitlesError(err.GetMessage()); }

	try {
		std::shared_ptr<VideoFrame> composited;
		if (render_mode == SubtitleRenderMode::CompatibilityFrameOnly) {
			if (!frame)
				throw AsyncVideoProviderSubtitlesError("Compatibility subtitles provider requires a BGRA source frame.");
			packet.allow_source_frame_upload_reuse = false;
			// This is already a worker-owned BGRA working copy: either decoded
			// after refreshing the subtitle-free cache above, or copied from it.
			// Draw into it directly like the legacy path to avoid a second
			// full-frame copy on every CSRI rerender.
			packet.source_frame_storage = frame;
			packet.source_frame_owner = frame;
			packet.source_frame = MakeSourceFrameView(*frame, packet.source_frame);
			subs_provider->DrawSubtitles(*frame, time / 1000.);
		}
		else if (render_mode == SubtitleRenderMode::PremultipliedOverlay) {
			auto overlay_storage = acquire_buffer(subtitle_overlay_buffers);
			overlay_storage->Reset(
				packet.source_frame.width,
				packet.source_frame.height,
				packet.source_frame.flipped,
				!subs_provider->RenderOverlayClearsTarget());
			auto subtitle_overlay = overlay_storage->MakeView(true);

			if (subs_provider->RenderOverlay(packet.source_frame, subtitle_overlay, time / 1000.)) {
				overlay_storage->has_visible_content = subtitle_overlay.has_visible_content;
				if (subs_provider->SupportsOverlayDirtyRects()) {
					if (subtitle_overlay.dirty_rects && subtitle_overlay.dirty_rect_count > 0) {
						overlay_storage->dirty_rects.assign(
							subtitle_overlay.dirty_rects,
							subtitle_overlay.dirty_rects + subtitle_overlay.dirty_rect_count);
					}
					else {
						overlay_storage->dirty_rects.clear();
					}
				}
				else {
					overlay_storage->dirty_rects.clear();
					if (subtitle_overlay.has_visible_content
						&& subtitle_overlay.width > 0
						&& subtitle_overlay.height > 0) {
						overlay_storage->dirty_rects.push_back({
							subtitle_overlay.target_x,
							subtitle_overlay.target_y,
							subtitle_overlay.width,
							subtitle_overlay.height
						});
					}
				}
				subtitle_overlay.dirty_rects = overlay_storage->dirty_rects.empty()
					? nullptr
					: overlay_storage->dirty_rects.data();
				subtitle_overlay.dirty_rect_count = static_cast<int>(overlay_storage->dirty_rects.size());
				subtitle_overlay.continuity_generation = overlay_continuity_generation;
				if (subtitle_overlay.has_visible_content) {
					packet.subtitle_overlay_storage = overlay_storage;
					packet.subtitle_overlay = subtitle_overlay;
					packet.has_subtitle_overlay = true;
				}
			}
			else {
				if (!frame)
					throw AsyncVideoProviderSubtitlesError("Subtitle provider cannot bake subtitles into native source frames.");
				composited = acquire_buffer(composited_buffers);
				*composited = *frame;
				packet.composited_frame_storage = composited;
				subs_provider->DrawSubtitles(*composited, time / 1000.);
			}
		}
		else {
			throw AsyncVideoProviderSubtitlesError("Subtitle provider reported an unknown render mode.");
		}
	}
	catch (agi::UserCancelException const&) { }
	catch (AsyncVideoProviderSubtitlesError const&) { throw; }
	catch (agi::Exception const& err) {
		throw AsyncVideoProviderSubtitlesError(err.GetMessage());
	}
	catch (std::string const& err) {
		throw AsyncVideoProviderSubtitlesError(err);
	}
	catch (std::exception const& err) {
		throw AsyncVideoProviderSubtitlesError(err.what());
	}
	catch (...) {
		throw AsyncVideoProviderSubtitlesError("Unknown subtitle renderer error.");
	}

	return packet;
}

static std::unique_ptr<SubtitlesProvider> get_subs_provider(
	AsyncVideoProviderEventSink const& event_sink,
	agi::BackgroundRunner *br,
	std::shared_ptr<const TransientFontSet> transient_fonts) {
	try {
		return SubtitlesProviderFactory::GetProvider({ br, std::move(transient_fonts) });
	}
	catch (agi::Exception const& err) {
		if (event_sink.on_subtitles_error)
			event_sink.on_subtitles_error(err.GetMessage());
		return nullptr;
	}
}

AsyncVideoProvider::AsyncVideoProvider(agi::fs::path const& video_filename, std::string const& colormatrix, AsyncVideoProviderEventSink event_sink, agi::BackgroundRunner *br, std::shared_ptr<const TransientFontSet> transient_fonts, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink)
: AsyncVideoProvider(
	VideoProviderFactory::GetProvider(
		video_filename,
		colormatrix,
		br,
		std::move(choice_sink)),
	get_subs_provider(event_sink, br, std::move(transient_fonts)),
	std::move(event_sink))
{
}

AsyncVideoProvider::AsyncVideoProvider(std::unique_ptr<VideoProvider> source_provider, std::unique_ptr<SubtitlesProvider> subs_provider, AsyncVideoProviderEventSink event_sink)
: worker(agi::dispatch::Create())
, subs_provider(std::move(subs_provider))
, source_provider(std::move(source_provider))
, event_sink(std::move(event_sink))
, provider_version(next_provider_delivery_version.fetch_add(1, std::memory_order_relaxed))
{
	subtitles_timecodes = this->source_provider->GetFPS();
	if (this->subs_provider) {
		LOG_I(kSubtitleProviderUseLogTag) << "Activated subtitles provider: "
			<< this->subs_provider->GetDebugName()
			<< " (mode=" << SubtitleRenderModeName(this->subs_provider->GetRenderMode()) << ")";
		this->subs_provider->OnActivated();
	}
	ReconfigureSourceOutputMode();
}

AsyncVideoProvider::~AsyncVideoProvider() {
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		prefetch_shutdown = true;
	}
	worker->Sync([this] {
		WorkerSyncTracker sync_tracker(*this);
		while (ProcessPending()) {
		}
	});
}
AsyncVideoProviderMemoryStats AsyncVideoProvider::CollectMemoryStats() {
	AsyncVideoProviderMemoryStats stats;
	if (IsReentrantWorkerCall()) return {};
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		stats.provider = source_provider->GetMemoryStats();
		stats.selected_source_mode = selected_source_mode;
		stats.decoder_name = source_provider->GetDecoderName();
		if (subs_provider) {
			stats.subtitles_provider_name = subs_provider->GetDebugName();
			stats.subtitles_render_mode = SubtitleRenderModeName(subs_provider->GetRenderMode());
			stats.compatibility_requires_bgra8 =
				subs_provider->GetRenderMode() == SubtitleRenderMode::CompatibilityFrameOnly;
		}
		stats.subtitles_loaded = static_cast<bool>(subs);
		{
			std::lock_guard<std::mutex> lock(pending_mutex);
			stats.pending_subtitles_update = static_cast<bool>(pending_subs) || !pending_changed_lines.empty();
		}
		if (subs)
			stats.subtitles_event_count = static_cast<int>(subs->Events.size());

		stats.source_pool_buffers = static_cast<int>(source_buffers.size());
		for (auto const& buffer : source_buffers) {
			if (buffer)
				stats.source_pool_bytes += EstimateVideoFrameStorageBytes(*buffer);
		}

		stats.composited_pool_buffers = static_cast<int>(composited_buffers.size());
		for (auto const& buffer : composited_buffers) {
			if (buffer)
				stats.composited_pool_bytes += EstimateVideoFrameStorageBytes(*buffer);
		}

		stats.subtitle_overlay_pool_buffers = static_cast<int>(subtitle_overlay_buffers.size());
		for (auto const& overlay : subtitle_overlay_buffers) {
			if (overlay)
				stats.subtitle_overlay_pool_bytes += EstimateSubtitleOverlayStorageBytes(*overlay);
		}
	});
	return stats;
}

bool AsyncVideoProvider::CanGenerateSceneChangeKeyframes() const {
	bool result = false;
	if (IsReentrantWorkerCall()) return false;
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		result = source_provider->CanGenerateSceneChangeKeyframes();
	});
	return result;
}

std::string AsyncVideoProvider::GetSceneChangeKeyframeCacheToken() const {
	std::string token;
	if (IsReentrantWorkerCall()) return {};
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		token = source_provider->GetSceneChangeKeyframeCacheToken();
	});
	return token;
}

void AsyncVideoProvider::GenerateSceneChangeKeyframes(agi::fs::path const& output_path, agi::BackgroundRunner *br) {
	auto run = [&](agi::ProgressSink *ps) {
		worker->Sync([&] {
			WorkerSyncTracker sync_tracker(*this);
			while (ProcessPending()) { }
			source_provider->GenerateSceneChangeKeyframes(output_path, ps);
			ResetCachedSourceFrame();
		});
	};

	if (br)
		br->Run(run);
	else
		run(nullptr);
}

void AsyncVideoProvider::LoadSubtitles(
	const AssFile *new_subs,
	VideoSubtitleUpdateOptions options) throw() {
	aegisub::async_video_trace::PipelineEvent submitted;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		submitted.version.content = ++content_version;
		pending_subs = agi::make_unique<AssFile>(*new_subs);
		pending_changed_lines.clear();
		subtitle_source_file = new_subs;
		subtitle_source_lines = CaptureSubtitleSourceLines(*new_subs);
		pending_overlay_upload_continuity_invalidation = true;
		pending_check_updated = false;
		MergeSubtitleUpdateOptions(pending_subtitle_update_options, options);
		submitted.stage = "submit_load";
		submitted.version.provider = provider_version;
		submitted.version.request = request_version.load(std::memory_order_relaxed);
		submitted.delivery_class = options.delivery_class;
		submitted.visual_interaction_id = options.visual_interaction_id;
		submitted.frame = pending_frame_number;
		submitted.timestamp_ns = aegisub::async_video_trace::CaptureTimestamp();
	}
	aegisub::async_video_trace::ObservePipelineEvent(submitted);
	ScheduleProcessing();
}

void AsyncVideoProvider::UpdateSubtitles(
	const AssFile *new_subs,
	const AssDialogue *changed,
	VideoSubtitleUpdateOptions options) throw() {
	if (!changed) {
		UpdateSubtitles(new_subs, std::span<const AssDialogue *const>{}, options);
		return;
	}
	const AssDialogue *changed_lines[] = { changed };
	UpdateSubtitles(new_subs, changed_lines, options);
}

void AsyncVideoProvider::UpdateSubtitles(
	const AssFile *new_subs,
	std::span<const AssDialogue *const> changed,
	VideoSubtitleUpdateOptions options) throw() {
	aegisub::async_video_trace::PipelineEvent submitted;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		submitted.version.content = ++content_version;
		bool valid = !changed.empty() && subtitle_source_file == new_subs;
		for (auto line : changed) {
			if (!valid)
				break;
			if (!line || line->Row < 0 || static_cast<size_t>(line->Row) >= subtitle_source_lines.size()) {
				valid = false;
				break;
			}
			auto const& identity = subtitle_source_lines[static_cast<size_t>(line->Row)];
			if (identity.first != line || identity.second != line->Id)
				valid = false;
		}

		if (valid) {
			std::vector<AssDialogueBase> updates;
			updates.reserve(changed.size());
			for (auto line : changed)
				updates.emplace_back(static_cast<AssDialogueBase const&>(*line));
			std::sort(updates.begin(), updates.end(), [](auto const& left, auto const& right) {
				return left.Row < right.Row;
			});
			updates.erase(std::unique(updates.begin(), updates.end(), [](auto const& left, auto const& right) {
				return left.Row == right.Row;
			}), updates.end());
			MergePendingChangedLines(pending_changed_lines, std::move(updates));
		}
		else {
			pending_subs = agi::make_unique<AssFile>(*new_subs);
			pending_changed_lines.clear();
			subtitle_source_file = new_subs;
			subtitle_source_lines = CaptureSubtitleSourceLines(*new_subs);
		}
		pending_overlay_upload_continuity_invalidation = true;
		if (pending_frame_kind != PendingFrameKind::Request)
			pending_check_updated = true;
		MergeSubtitleUpdateOptions(pending_subtitle_update_options, options);
		submitted.stage = "submit_update";
		submitted.version.provider = provider_version;
		submitted.version.request = request_version.load(std::memory_order_relaxed);
		submitted.delivery_class = options.delivery_class;
		submitted.visual_interaction_id = options.visual_interaction_id;
		submitted.frame = pending_frame_number;
		submitted.timestamp_ns = aegisub::async_video_trace::CaptureTimestamp();
	}
	aegisub::async_video_trace::ObservePipelineEvent(submitted);
	ScheduleProcessing();
}

void AsyncVideoProvider::RequestFrame(int new_frame, double new_time, bool supersede_in_flight) throw() {
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_time = new_time;
		pending_frame_number = new_frame;
		pending_frame_kind = PendingFrameKind::Request;
		pending_check_updated = false;
		if (supersede_in_flight)
			++request_version;
	}
	ScheduleProcessing();
}

void AsyncVideoProvider::CancelPendingFrameRequests() noexcept {
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_frame_kind = PendingFrameKind::None;
		pending_frame_number = -1;
		pending_time = -1.;
		pending_check_updated = false;
		request_version.fetch_add(1, std::memory_order_relaxed);
	}
}

void AsyncVideoProvider::PrefetchFrames(int first_frame, int count) noexcept {
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (prefetch_shutdown || first_frame < 0 || count <= 0)
			return;
		prefetch_next_frame = first_frame;
		prefetch_end_frame = first_frame + count;
		prefetch_request_version = request_version.load(std::memory_order_relaxed);
		prefetch_content_version = content_version.load(std::memory_order_relaxed);
		if (prefetch_scheduled)
			return;
		prefetch_scheduled = true;
	}
	worker->Async([this] { ProcessPrefetch(); });
}

void AsyncVideoProvider::CancelFramePrefetch() noexcept {
	std::scoped_lock lock(pending_mutex);
	prefetch_next_frame = -1;
	prefetch_end_frame = -1;
}

void AsyncVideoProvider::ProcessPrefetch() {
	int frame = -1;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (!CanContinuePrefetchLocked()) {
			prefetch_scheduled = false;
			return;
		}
		frame = prefetch_next_frame++;
	}

	if (!source_provider->HasFrameCache()) {
		std::lock_guard<std::mutex> lock(pending_mutex);
		prefetch_next_frame = -1;
		prefetch_end_frame = -1;
		prefetch_scheduled = false;
		return;
	}

	try {
		// Decode through the same path interactive requests use so the cache
		// key matches what playback will ask for.
		if (selected_source_mode == SourceFrameOutputMode::Native) {
			SourceFrame native_frame;
			std::shared_ptr<void> owner;
			source_provider->GetNativeFrame(frame, native_frame, owner);
		}
		else {
			auto scratch = acquire_buffer(source_buffers);
			source_provider->GetFrame(frame, *scratch);
		}
	}
	catch (VideoProviderError const& err) {
		LOG_E("video_provider/prefetch") << "Frame " << frame << " prefetch failed: " << err.GetMessage();
		std::lock_guard<std::mutex> lock(pending_mutex);
		prefetch_next_frame = -1;
		prefetch_end_frame = -1;
		prefetch_scheduled = false;
		return;
	}

	{
		// Re-post under the mutex together with the shutdown check: the
		// destructor sets the flag under this mutex before enqueueing its
		// drain, so a link that passes here is always queued ahead of the
		// drain and runs while the object is still alive.
		std::lock_guard<std::mutex> lock(pending_mutex);
		prefetch_scheduled = false;
		if (CanContinuePrefetchLocked()) {
			prefetch_scheduled = true;
			worker->Async([this] { ProcessPrefetch(); });
		}
	}
}

bool AsyncVideoProvider::CanContinuePrefetchLocked() const noexcept {
	return !prefetch_shutdown
		&& prefetch_request_version == request_version.load(std::memory_order_relaxed)
		&& prefetch_content_version == content_version.load(std::memory_order_relaxed)
		&& pending_frame_kind == PendingFrameKind::None
		&& !has_pending_color_space
		&& !pending_subs
		&& pending_changed_lines.empty()
		&& prefetch_next_frame >= 0
		&& prefetch_next_frame < prefetch_end_frame;
}

bool AsyncVideoProvider::IsCurrent(VideoRenderDeliveryVersion version) const noexcept {
	return version.provider == provider_version
		&& version.content == content_version.load(std::memory_order_relaxed)
		&& version.request == request_version.load(std::memory_order_relaxed);
}

bool AsyncVideoProvider::IsCurrent(VideoRenderPacket const& packet, int expected_frame) const noexcept {
	return packet.frame_number == expected_frame && IsCurrent(packet.delivery_version);
}

void AsyncVideoProvider::SetCurrentFrameContext(int current_frame, double current_time) throw() {
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (pending_frame_kind != PendingFrameKind::Request) {
			pending_frame_number = current_frame;
			pending_time = current_time;
			pending_frame_kind = PendingFrameKind::CurrentContext;
		}
	}
	ScheduleProcessing();
}

bool AsyncVideoProvider::NeedUpdate(std::vector<AssDialogueBase const*> const& visible_lines) {
	// Always need to render after a seek
	if (single_frame != NEW_SUBS_FILE || frame_number != last_rendered)
		return true;

	// Obviously need to render if the number of visible lines has changed
	if (visible_lines.size() != last_lines.size())
		return true;

	for (size_t i = 0; i < last_lines.size(); ++i) {
		auto const& last = last_lines[i];
		auto const& cur = *visible_lines[i];
		if (last.Layer  != cur.Layer)  return true;
		if (last.Margin != cur.Margin) return true;
		if (last.Style  != cur.Style)  return true;
		if (last.Effect != cur.Effect) return true;
		if (last.Text   != cur.Text)   return true;

		// Changing the start/end time effects the appearance only if the
		// line is animated. This is obviously not a very accurate check for
		// animated lines, but false positives aren't the end of the world
		if ((last.Start != cur.Start || last.End != cur.End) &&
			(!cur.Effect.get().empty() || cur.Text.get().find('\\') != std::string::npos))
			return true;
	}

	return false;
}

void AsyncVideoProvider::DeliverFrameReady(VideoRenderPacket packet, double time) {
	if (event_sink.on_frame_ready)
		event_sink.on_frame_ready(std::move(packet), time);
}

void AsyncVideoProvider::DeliverVideoError(std::string const& message) {
	if (event_sink.on_video_error)
		event_sink.on_video_error(message);
}

void AsyncVideoProvider::DeliverSubtitlesError(std::string const& message) {
	if (event_sink.on_subtitles_error)
		event_sink.on_subtitles_error(message);
}

void AsyncVideoProvider::ScheduleProcessing() {
	bool should_schedule = false;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (!processing_scheduled) {
			processing_scheduled = true;
			should_schedule = true;
		}
	}

	if (!should_schedule)
		return;

	worker->Async([this] {
		while (ProcessPending()) { }
	});
}

bool AsyncVideoProvider::ProcessPending() {
	struct PendingWork {
		std::unique_ptr<AssFile> subs;
		std::vector<AssDialogueBase> changed_lines;
		bool check_updated = false;
		bool has_frame = false;
		int frame_number = -1;
		double time = -1.;
		bool has_current_frame_context = false;
		int current_frame_number = -1;
		double current_time = -1.;
		bool has_color_space = false;
		std::string color_space;
		bool invalidate_overlay_upload_continuity = false;
		bool force_current_frame_render = false;
		VideoSubtitleUpdateOptions subtitle_update_options;
		VideoRenderDeliveryClass delivery_class = VideoRenderDeliveryClass::EveryFrame;
		std::uint64_t visual_interaction_id = 0;
		VideoRenderDeliveryVersion delivery_version;
	};

	PendingWork work;
	aegisub::async_video_trace::PipelineEvent work_trace;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		if (!pending_subs
			&& pending_changed_lines.empty()
			&& pending_frame_kind == PendingFrameKind::None
			&& !has_pending_color_space) {
			processing_scheduled = false;
			return false;
		}

		work.subs = std::move(pending_subs);
		work.changed_lines.swap(pending_changed_lines);
		work.invalidate_overlay_upload_continuity = pending_overlay_upload_continuity_invalidation;
		pending_overlay_upload_continuity_invalidation = false;
		work.check_updated = pending_check_updated;
		pending_check_updated = false;
		work.subtitle_update_options = pending_subtitle_update_options;
		pending_subtitle_update_options = {};
		PendingFrameKind const pending_kind = pending_frame_kind;
		if (pending_kind == PendingFrameKind::Request) {
			work.has_frame = true;
			work.frame_number = pending_frame_number;
			work.time = pending_time;
		}
		else if (pending_kind == PendingFrameKind::CurrentContext) {
			work.has_current_frame_context = true;
			work.current_frame_number = pending_frame_number;
			work.current_time = pending_time;
			if (work.subs || !work.changed_lines.empty()) {
				work.has_frame = true;
				work.frame_number = pending_frame_number;
				work.time = pending_time;
			}
		}
		else if ((work.subs || !work.changed_lines.empty()) && frame_number >= 0) {
			work.has_frame = true;
			work.frame_number = frame_number;
			work.time = time;
		}
		pending_frame_kind = PendingFrameKind::None;
		pending_frame_number = -1;
		pending_time = -1.;
		if (has_pending_color_space) {
			work.has_color_space = true;
			work.color_space = pending_color_space;
			has_pending_color_space = false;
			pending_color_space.clear();
		}
		work.delivery_version.provider = provider_version;
		work.delivery_version.request = request_version.load(std::memory_order_relaxed);
		work.delivery_version.content = content_version.load(std::memory_order_relaxed);
		work.delivery_class = pending_kind == PendingFrameKind::Request
			? VideoRenderDeliveryClass::EveryFrame
			: work.subtitle_update_options.delivery_class;
		work.visual_interaction_id = pending_kind == PendingFrameKind::Request
			? 0
			: work.subtitle_update_options.visual_interaction_id;
		work.force_current_frame_render = work.subtitle_update_options.force_current_frame_render;
		work_trace = {
			.stage = "worker_take",
			.version = work.delivery_version,
			.delivery_class = work.delivery_class,
			.visual_interaction_id = work.visual_interaction_id,
			.frame = work.frame_number,
			.timestamp_ns = aegisub::async_video_trace::CaptureTimestamp()};
	}
	aegisub::async_video_trace::ObservePipelineEvent(work_trace);
	auto observe_work = [&](char const *stage, double duration_ms = -1.0) {
		auto event = work_trace;
		event.stage = stage;
		event.timestamp_ns = aegisub::async_video_trace::CaptureTimestamp();
		event.duration_ms = duration_ms;
		aegisub::async_video_trace::ObservePipelineEvent(event);
	};

	if (work.has_color_space)
		source_provider->SetColorSpace(work.color_space);
	if (work.has_color_space)
		ResetCachedSourceFrame();

	if (work.has_current_frame_context) {
		frame_number = work.current_frame_number;
		time = work.current_time;
	}

	if (work.invalidate_overlay_upload_continuity)
		AdvanceOverlayUploadContinuity();

	if (work.subs) {
		subs = std::move(work.subs);
		subs_lines_by_row.clear();
		for (auto& line : subs->Events)
			subs_lines_by_row.push_back(&line);
		single_frame = NEW_SUBS_FILE;
	}
	if (!work.changed_lines.empty() && subs) {
		bool applied = false;
		for (auto const& changed_line : work.changed_lines) {
			int const target_row = changed_line.Row;
			if (target_row < 0 || static_cast<size_t>(target_row) >= subs_lines_by_row.size())
				continue;
			static_cast<AssDialogueBase&>(*subs_lines_by_row[static_cast<size_t>(target_row)]) = changed_line;
			applied = true;
		}
		if (applied)
			single_frame = NEW_SUBS_FILE;
	}

	if (!work.has_frame) {
		observe_work("worker_skip_no_frame");
		return true;
	}

	frame_number = work.frame_number;
	time = work.time;

	std::vector<AssDialogueBase const*> visible_lines;
	if (subs) {
		auto const& fps = subtitles_timecodes;
		for (auto const& line : subs->Events) {
			if (!line.Comment && IsAssDialogueVisibleAtTimeForOutput(line.Start, line.End, static_cast<int>(time), AssTimeOutputMode::LegacyRounding, &fps))
				visible_lines.push_back(&line);
		}
	}

	if (work.check_updated && !work.force_current_frame_render && !NeedUpdate(visible_lines)) {
		observe_work("worker_skip_no_change");
		return true;
	}

	auto remember_rendered_lines = [&] {
		last_lines.clear();
		last_lines.reserve(visible_lines.size());
		for (auto line : visible_lines)
			last_lines.push_back(*line);
		last_rendered = frame_number;
	};

	// Mouse-drag subtitle edits can outpace expensive compatibility renderers.
	// If newer work arrived before entering the renderer, skip this stale pass
	// instead of spending a long CSRI render only to drop the packet afterwards.
	if (!IsCurrent(work.delivery_version)) {
		observe_work("worker_skip_stale");
		AdvanceOverlayUploadContinuity();
		return true;
	}

	try {
		observe_work("worker_render_begin");
		auto const render_begin = std::chrono::steady_clock::now();
		auto packet = ProcRenderPacket(frame_number, time, false, false);
		auto const render_duration_ms =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_begin).count();
		bool should_deliver = IsCurrent(work.delivery_version);
		observe_work(should_deliver ? "worker_render_deliver" : "worker_render_drop", render_duration_ms);
		aegisub::async_video_trace::ObserveVideoFrameRenderDuration(frame_number, time, should_deliver, false, render_duration_ms);
		aegisub::async_video_trace::ObserveFrameResult(frame_number, time, should_deliver, false);
		if (should_deliver) {
			remember_rendered_lines();
			packet.delivery_version = work.delivery_version;
			packet.delivery_class = work.delivery_class;
			packet.visual_interaction_id = work.visual_interaction_id;
			DeliverFrameReady(std::move(packet), time);
		}
		else {
			AdvanceOverlayUploadContinuity();
		}
	}
	catch (AsyncVideoProviderVideoError const& err) {
		bool should_deliver = IsCurrent(work.delivery_version);
		observe_work(should_deliver ? "worker_error_video" : "worker_error_video_stale");
		if (should_deliver)
			DeliverVideoError(err.GetMessage());
		else {
			AdvanceOverlayUploadContinuity();
		}
	}
	catch (AsyncVideoProviderSubtitlesError const& err) {
		bool should_deliver = IsCurrent(work.delivery_version);
		observe_work(should_deliver ? "worker_error_subtitles" : "worker_error_subtitles_stale");
		if (should_deliver)
			DeliverSubtitlesError(err.GetMessage());
		else {
			AdvanceOverlayUploadContinuity();
		}
	}

	return true;
}

std::shared_ptr<VideoFrame> AsyncVideoProvider::GetFrame(int frame, double time, bool raw) {
	return GetFrameBgra(frame, time, raw);
}

std::shared_ptr<VideoFrame> AsyncVideoProvider::GetFrameBgra(int frame, double time, bool raw) {
	std::shared_ptr<VideoFrame> ret;
	if (IsReentrantWorkerCall()) return nullptr;
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		while (ProcessPending()) { }
		ret = BakePacketForCpuReadback(ProcRenderPacket(frame, time, raw, true));
	});
	return ret;
}

KeyPointRangeScanResult AsyncVideoProvider::FindKeyPointRange(KeyPointRangeScanRequest const& request) {
	KeyPointRangeScanResult result;
	if (IsReentrantWorkerCall()) return result;
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		while (ProcessPending()) { }

		int const frame_count = source_provider->GetFrameCount();
		int const width = source_provider->GetWidth();
		int const height = source_provider->GetHeight();
		if (request.frame < 0
			|| request.frame >= frame_count
			|| request.x < 0
			|| request.x >= width
			|| request.y < 0
			|| request.y >= height
			|| request.scan_step <= 0
			|| request.bounds_tolerance < 0
			|| (request.detect_fade && request.max_fade_frames < 0)) {
			result.status = KeyPointRangeScanStatus::InvalidRequest;
			return;
		}

		double const tolerance_squared =
			static_cast<double>(request.tolerance) * static_cast<double>(request.tolerance);
		aegisub::keypoint::ColorMatcher matcher(request.b, request.g, request.r, tolerance_squared);

		VideoFrame decoded;
		KeyPointPixels const *frame = nullptr;
		std::unordered_map<int, KeyPointPixels> pixel_cache;
		std::deque<int> cache_order;
		size_t cached_pixel_bytes = 0;
		constexpr size_t pixel_cache_budget = 16 * 1024 * 1024;
		auto load_frame = [&](int frame_number) -> KeyPointRangeScanStatus {
			if (auto const cached = pixel_cache.find(frame_number); cached != pixel_cache.end()) {
				frame = &cached->second;
				return KeyPointRangeScanStatus::Success;
			}
			try {
				source_provider->GetFrame(frame_number, decoded);
			}
			catch (VideoProviderError const&) {
				return KeyPointRangeScanStatus::FrameUnavailable;
			}
			if (std::cmp_not_equal(decoded.width, width) || std::cmp_not_equal(decoded.height, height) || decoded.pitch < decoded.width * 4 || decoded.height == 0 || decoded.pitch > decoded.data.size() / decoded.height)
				return KeyPointRangeScanStatus::FrameUnavailable;

			KeyPointPixels pixels(decoded, request.x, request.y);
			size_t const pixel_bytes = pixels.data.size();
			while (!cache_order.empty() && cached_pixel_bytes + pixel_bytes > pixel_cache_budget) {
				auto const oldest = pixel_cache.find(cache_order.front());
				cached_pixel_bytes -= oldest->second.data.size();
				pixel_cache.erase(oldest);
				cache_order.pop_front();
			}
			auto const entry = pixel_cache.emplace(frame_number, std::move(pixels)).first;
			cache_order.push_back(frame_number);
			cached_pixel_bytes += pixel_bytes;
			frame = &entry->second;
			return KeyPointRangeScanStatus::Success;
		};

		auto probe_anchor_frame = [&](
			int frame_number,
			aegisub::keypoint::ColorMatcher& active_matcher,
			KeyPointBounds& bounds) -> KeyPointRangeScanStatus {
			auto const status = load_frame(frame_number);
			if (status != KeyPointRangeScanStatus::Success)
				return status;

			return CalculateKeyPointBounds(*frame, request.x, request.y, active_matcher, bounds)
					   ? KeyPointRangeScanStatus::Success
					   : KeyPointRangeScanStatus::AnchorMismatch;
		};

		enum class ProbeResult : std::uint8_t { Match,
												Mismatch,
												Unknown,
												FrameUnavailable };
		constexpr int maximum_unknown_frames = 4;
		std::vector<FadeTemplateSample> fade_template;
		auto probe_frame = [&](
							   int frame_number,
							   aegisub::keypoint::ColorMatcher& active_matcher,
							   KeyPointBounds const& active_anchor_bounds) -> ProbeResult {
			auto const status = load_frame(frame_number);
			if (status != KeyPointRangeScanStatus::Success)
				return ProbeResult::FrameUnavailable;

			bool const bounds_match = MatchesKeyPointBoundsWithinTolerance(
				*frame,
				request.x,
				request.y,
				active_matcher,
				active_anchor_bounds,
				request.bounds_tolerance);
			if (fade_template.size() < 4)
				return bounds_match ? ProbeResult::Match : ProbeResult::Mismatch;
			auto const visibility = FadeVisibilityScore(*frame, fade_template);
			if (!visibility.valid)
				return ProbeResult::Unknown;
			double const shape_support = visibility.shape_support;
			if (bounds_match && shape_support > 0.1)
				return ProbeResult::Match;

			// A pale scene can join the foreground's horizontal/vertical color
			// runs without removing the text. Require the original stroke's
			// color and local contrast before continuing across that boundary.
			auto const matching_samples = std::count_if(
				fade_template.begin(), fade_template.end(), [&](auto const& sample) {
					return KeyPointPixelMatches(*frame, sample.foreground_x,
												sample.foreground_y, active_matcher);
				});
			if (matching_samples * 4 >= static_cast<int>(fade_template.size()) * 3 && shape_support >= 0.8)
				return ProbeResult::Match;
			return ProbeResult::Mismatch;
		};

		KeyPointBounds anchor_bounds;
		result.status = probe_anchor_frame(request.frame, matcher, anchor_bounds);
		if (result.status != KeyPointRangeScanStatus::Success)
			return;
		// A partially transparent click includes some background color inside
		// the stroke. Allow that variation during calibration, then rebuild a
		// precise core template on the confirmed fully-visible platform.
		fade_template = request.detect_fade
							? BuildFadeTemplate(*frame, request.x, request.y, matcher, 5.0)
							: std::vector<FadeTemplateSample>{};
		auto const anchor_visibility = request.detect_fade
										   ? FadeVisibilityScore(*frame, fade_template)
										   : FadeVisibility{};
		std::unordered_map<int, FadeVisibility> visibility_cache;
		if (request.detect_fade) {
			visibility_cache.reserve(static_cast<size_t>(request.max_fade_frames) * 2 + 1);
			visibility_cache.emplace(request.frame, anchor_visibility);
		}
		auto visibility_at = [&](int frame_number, FadeVisibility& visibility) {
			auto const cached = visibility_cache.find(frame_number);
			if (cached != visibility_cache.end()) {
				visibility = cached->second;
				return KeyPointRangeScanStatus::Success;
			}
			auto const status = load_frame(frame_number);
			if (status != KeyPointRangeScanStatus::Success)
				return status;
			visibility = FadeVisibilityScore(*frame, fade_template);
			visibility_cache.emplace(frame_number, visibility);
			return KeyPointRangeScanStatus::Success;
		};

		int left = request.frame;
		int right = request.frame;
		auto scan_strict_range = [&](
									 int active_anchor_frame,
									 aegisub::keypoint::ColorMatcher& active_matcher,
									 KeyPointBounds const& active_anchor_bounds) -> KeyPointRangeScanStatus {
			// A skipped frame could hide an entire unobservable interval. Fade
			// tracking needs consecutive evidence; legacy color-only scans may
			// still use the caller's coarse step.
			int const scan_step = fade_template.size() >= 4 ? 1 : request.scan_step;
			auto scan_direction = [&](int direction, int& boundary) {
				boundary = active_anchor_frame;
				int missing = direction < 0 ? -1 : frame_count;
				int unknown_frames = 0;
				for (int pos = active_anchor_frame + direction * scan_step;
					 pos >= 0 && pos < frame_count; pos += direction * scan_step) {
					auto const probe = probe_frame(pos, active_matcher, active_anchor_bounds);
					if (probe == ProbeResult::FrameUnavailable)
						return KeyPointRangeScanStatus::FrameUnavailable;
					if (probe == ProbeResult::Unknown)
						unknown_frames += scan_step;
					else
						unknown_frames = 0;
					if (probe == ProbeResult::Mismatch || unknown_frames > maximum_unknown_frames) {
						missing = pos;
						break;
					}
					if (probe == ProbeResult::Match)
						boundary = pos;
				}
				unknown_frames = 0;
				for (int pos = boundary + direction;
					 direction < 0 ? pos > missing : pos < missing; pos += direction) {
					auto const probe = probe_frame(pos, active_matcher, active_anchor_bounds);
					if (probe == ProbeResult::FrameUnavailable)
						return KeyPointRangeScanStatus::FrameUnavailable;
					if (probe == ProbeResult::Unknown)
						++unknown_frames;
					else
						unknown_frames = 0;
					if (probe == ProbeResult::Mismatch || unknown_frames > maximum_unknown_frames)
						break;
					if (probe == ProbeResult::Match)
						boundary = pos;
				}
				return KeyPointRangeScanStatus::Success;
			};
			auto const status = scan_direction(-1, left);
			return status == KeyPointRangeScanStatus::Success ? scan_direction(1, right) : status;
		};

		result.status = scan_strict_range(request.frame, matcher, anchor_bounds);
		if (result.status != KeyPointRangeScanStatus::Success)
			return;

		constexpr int plateau_samples = 4;
		std::array<double, plateau_samples> full_platform_scores{};
		aegisub::align_video_fade::PlateauReference full_platform_reference;
		if (request.detect_fade && request.max_fade_frames > 0 && fade_template.size() >= 4) {
			struct PlateauCandidate {
				bool found = false;
				int frame = -1;
				int start_index = -1;
				double level = 0.0;
			};
			std::vector<int> calibration_frames;
			std::vector<FadeVisibility> calibration_scores;
			int const calibration_start = std::max(0, request.frame - request.max_fade_frames);
			int const calibration_end = std::min(
				frame_count - 1,
				request.frame + request.max_fade_frames);
			calibration_frames.reserve(calibration_end - calibration_start + 1);
			calibration_scores.reserve(calibration_end - calibration_start + 1);
			for (int pos = calibration_start; pos <= calibration_end; ++pos) {
				FadeVisibility visibility;
				result.status = visibility_at(pos, visibility);
				if (result.status != KeyPointRangeScanStatus::Success)
					return;
				calibration_frames.push_back(pos);
				calibration_scores.push_back(visibility);
			}

			int const anchor_index = request.frame - calibration_start;
			auto connected_limit = [&](int direction) {
				int limit = anchor_index;
				int unknown_frames = 0;
				for (int i = anchor_index + direction;
					 i >= 0 && std::cmp_less(i, calibration_scores.size()); i += direction) {
					auto const& sample = calibration_scores[i];
					if (!sample.valid) {
						if (++unknown_frames > maximum_unknown_frames)
							break;
						continue;
					}
					if (sample.level <= anchor_visibility.level * 0.08)
						break;
					unknown_frames = 0;
					limit = i;
				}
				return limit;
			};
			int const connected_start = connected_limit(-1);
			int const connected_end = connected_limit(1);

			PlateauCandidate plateau;
			for (int start = connected_start;
				 start + plateau_samples <= connected_end + 1;
				 ++start) {
				std::array<double, plateau_samples> window{};
				auto const samples = std::span<FadeVisibility const>(calibration_scores).subspan(start, plateau_samples);
				if (std::ranges::any_of(samples, [](auto const& sample) { return !sample.valid; }))
					continue;
				std::ranges::transform(samples, window.begin(), [](auto const& sample) { return sample.level; });
				std::ranges::sort(window);
				double const level = (window[1] + window[2]) * 0.5;
				double const maximum_spread = std::max(0.03, std::abs(level) * 0.05);
				if (level <= 0.10 || window.back() - window.front() > maximum_spread)
					continue;
				if (!plateau.found || level > plateau.level) {
					auto const maximum = std::ranges::max_element(samples, {}, &FadeVisibility::level);
					plateau.found = true;
					plateau.frame = calibration_frames[start + (maximum - samples.begin())];
					plateau.start_index = start;
					plateau.level = level;
				}
			}

			// Refine the initial template on the brightest stable platform. Its
			// core color no longer contains a partially visible background.
			if (plateau.found) {
				// Small score changes are edge/compression noise, not evidence
				// that a fully visible click should move to another scene.
				int const refined_anchor_frame = plateau.level > std::max(
																	 anchor_visibility.level * 1.05, anchor_visibility.level + 0.05)
													 ? plateau.frame
													 : request.frame;
				result.status = load_frame(refined_anchor_frame);
				if (result.status != KeyPointRangeScanStatus::Success)
					return;
				int normalized_y = 0;
				if (NormalizeFrameY(*frame, request.y, normalized_y)) {
					auto const *pixel = GetFramePixel(*frame, request.x, normalized_y);
					aegisub::keypoint::ColorMatcher plateau_matcher(
						pixel[0], pixel[1], pixel[2], tolerance_squared);
					KeyPointBounds plateau_bounds;
					if (CalculateKeyPointBounds(
							*frame,
							request.x,
							request.y,
							plateau_matcher,
							plateau_bounds)) {
						auto plateau_template = BuildFadeTemplate(
							*frame,
							request.x,
							request.y,
							plateau_matcher);
						if (plateau_template.size() >= 4) {
							fade_template = std::move(plateau_template);
							visibility_cache.clear();
							result.status = scan_strict_range(
								refined_anchor_frame,
								plateau_matcher,
								plateau_bounds);
							if (result.status != KeyPointRangeScanStatus::Success)
								return;
						}
					}
				}
			}

			if (plateau.found) {
				bool platform_visible = true;
				for (int i = 0; i < plateau_samples; ++i) {
					FadeVisibility sample;
					result.status = visibility_at(calibration_frames[plateau.start_index + i], sample);
					if (result.status != KeyPointRangeScanStatus::Success)
						return;
					if (!sample.valid) {
						platform_visible = false;
						break;
					}
					full_platform_scores[i] = sample.level;
				}
				if (platform_visible)
					full_platform_reference = aegisub::align_video_fade::BuildPlateauReference(full_platform_scores);
				// A cut changes the antialiased edge's background, so its alpha
				// estimate can shift slightly even on a fully opaque stroke.
				// Still require a temporally flat run when confirming each end.
				full_platform_reference.level_band = std::max(
					full_platform_reference.level_band, full_platform_reference.level * 0.04);
			}
		}

		result.status = KeyPointRangeScanStatus::Success;
		result.left = left;
		result.right = right;
		result.strict_left = left;
		result.strict_right = right;
		result.fade_in_end = left;
		result.fade_out_start = right;

		if (!request.detect_fade
			|| request.max_fade_frames == 0
			|| fade_template.size() < 4
			|| !full_platform_reference.valid)
			return;

		struct DirectionResult {
			bool detected = false;
			int outer_frame = -1;
			int inner_frame = -1;
			double confidence = 0.0;
		};
		auto scan_fade = [&](int boundary, int outward_direction) {
			DirectionResult direction_result;
			constexpr int plateau_confirmation = plateau_samples;
			constexpr int low_signal_confirmation = 4;
			std::vector<double> inside_scores;
			std::vector<int> inside_frames;
			bool full_signal_run = false;
			// Color tolerance commonly moves the strict boundary several frames
			// into the fade. Continue inward until a real full-visibility plateau
			// is confirmed instead of treating the strict boundary as fade end.
			for (int offset = 0; offset <= request.max_fade_frames; ++offset) {
				int const pos = boundary - outward_direction * offset;
				if (pos < left || pos > right)
					break;
				FadeVisibility visibility;
				if (visibility_at(pos, visibility) != KeyPointRangeScanStatus::Success || !visibility.valid) {
					return direction_result;
				}
				double const score = visibility.level;
				inside_frames.push_back(pos);
				inside_scores.push_back(score);
				if (inside_scores.size() >= plateau_confirmation) {
					auto const recent = std::span<double const>(inside_scores).last(plateau_confirmation);
					full_signal_run = aegisub::align_video_fade::FindConfirmedPlateauStart(
						recent,
						full_platform_reference,
						plateau_confirmation) == 0;
					if (full_signal_run)
						break;
				}
			}
			if (!full_signal_run)
				return direction_result;

			std::vector<double> sorted_plateau(
				inside_scores.end() - plateau_confirmation,
				inside_scores.end());
			auto const plateau_middle = sorted_plateau.begin() + sorted_plateau.size() / 2;
			std::nth_element(sorted_plateau.begin(), plateau_middle, sorted_plateau.end());
			double const plateau_level = *plateau_middle;

			std::vector<double> outside_scores;
			std::vector<int> outside_frames;
			int low_signal_run = 0;
			for (int offset = 1; offset <= request.max_fade_frames; ++offset) {
				int const pos = boundary + outward_direction * offset;
				if (pos < 0 || pos >= frame_count)
					break;
				FadeVisibility visibility;
				if (visibility_at(pos, visibility) != KeyPointRangeScanStatus::Success || !visibility.valid) {
					return direction_result;
				}
				double const score = visibility.level;
				outside_frames.push_back(pos);
				outside_scores.push_back(score);
				if (score <= plateau_level * 0.08)
					++low_signal_run;
				else
					low_signal_run = 0;
				if (low_signal_run >= low_signal_confirmation) {
					// Low opacity is still part of the fade. Stop only on a flat
					// background run, not four declining samples near the tail.
					auto const recent = std::span<double const>(outside_scores).last(low_signal_confirmation);
					auto const [minimum, maximum] = std::ranges::minmax_element(recent);
					if (*maximum - *minimum <= plateau_level * 0.01)
						break;
				}
			}
			if (outside_scores.size() < 3)
				return direction_result;

			std::reverse(outside_scores.begin(), outside_scores.end());
			std::reverse(outside_frames.begin(), outside_frames.end());
			outside_scores.insert(outside_scores.end(), inside_scores.begin(), inside_scores.end());
			outside_frames.insert(outside_frames.end(), inside_frames.begin(), inside_frames.end());
			auto const fit = aegisub::align_video_fade::FitVisibilityCurve(
				outside_scores,
				plateau_confirmation);
			if (!fit.detected)
				return direction_result;

			direction_result.detected = true;
			direction_result.outer_frame = outside_frames[fit.outer_index];
			direction_result.inner_frame = outside_frames[fit.inner_index];
			direction_result.confidence = fit.confidence;
			return direction_result;
		};

		auto const fade_in = scan_fade(left, -1);
		if (fade_in.detected) {
			result.left = fade_in.outer_frame;
			result.fade_in_end = fade_in.inner_frame;
			result.fade_in_detected = true;
			result.fade_in_confidence = fade_in.confidence;
		}

		auto const fade_out = scan_fade(right, 1);
		if (fade_out.detected) {
			result.right = fade_out.outer_frame;
			result.fade_out_start = fade_out.inner_frame;
			result.fade_out_detected = true;
			result.fade_out_confidence = fade_out.confidence;
		}
	});
	return result;
}

bool AsyncVideoProvider::ReconfigureSourceOutputMode() {
	auto const compatibility_requires_bgra8 =
		subs_provider && subs_provider->GetRenderMode() == SubtitleRenderMode::CompatibilityFrameOnly;
	auto const available_modes = source_provider->GetAvailableSourceModes();
	auto const selected = SelectPreferredSourceFrameOutputMode(
		preferred_source_modes,
		available_modes,
		compatibility_requires_bgra8);

	auto applied = selected;
	if (!source_provider->SetOutputMode(applied)) {
		applied = SourceFrameOutputMode::Bgra8;
		if (!source_provider->SetOutputMode(applied))
			return false;
	}

	bool const mode_changed = selected_source_mode != applied;
	if (!has_logged_source_mode || mode_changed) {
		auto source_format = source_provider->GetNativeFormatDescription();
		auto render_color = source_provider->GetColorMetadata();
		auto source_color = source_provider->GetRealColorMetadata();
		LOG_I(kSourceModeLogTag)
			<< source_provider->GetDecoderName()
			<< ": preferred=" << FormatSourceModeList(preferred_source_modes)
			<< ", available=" << FormatSourceModeList(available_modes)
			<< ", selected=" << SourceFrameOutputModeName(selected)
			<< ", applied=" << SourceFrameOutputModeName(applied)
			<< ", source_format=" << (source_format.empty() ? "none" : source_format)
			<< ", render_color={" << FormatColorMetadata(render_color) << "}"
			<< ", source_color={" << FormatColorMetadata(source_color) << "}"
			<< (compatibility_requires_bgra8 ? ", compatibility_requires_bgra8=true" : "");
		has_logged_source_mode = true;
	}

	if (!mode_changed)
		return false;

	selected_source_mode = applied;
	++content_version;
	last_rendered = -1;
	last_lines.clear();
	ResetCachedSourceFrame();
	AdvanceOverlayUploadContinuity();
	TrimReusablePools();
	return true;
}

VideoRenderPacket AsyncVideoProvider::GetRenderPacket(int frame, double time, bool raw) {
	VideoRenderPacket ret;
	if (IsReentrantWorkerCall()) return {};
	worker->Sync([&]{
		WorkerSyncTracker sync_tracker(*this);
		auto const render_begin = std::chrono::steady_clock::now();
		while (ProcessPending()) { }
		ret = ProcRenderPacket(frame, time, raw);
		auto const render_duration_ms =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_begin).count();
		aegisub::async_video_trace::ObserveVideoFrameRenderDuration(frame, time, true, true, render_duration_ms);
		// Keep the provider's current-frame context aligned with synchronous callers.
		frame_number = frame;
		this->time = time;
	});
	return ret;
}

void AsyncVideoProvider::SetColorSpace(std::string const& matrix) {
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_color_space = matrix;
		has_pending_color_space = true;
		++content_version;
	}
	ScheduleProcessing();
}

void AsyncVideoProvider::SetSubtitlesTimecodes(agi::vfr::Framerate timecodes) {
	if (IsReentrantWorkerCall()) return;
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		while (ProcessPending()) { }
		subtitles_timecodes = std::move(timecodes);
		++content_version;
		single_frame = NEW_SUBS_FILE;
		last_rendered = -1;
		last_lines.clear();
		ResetCachedSourceFrame();
		AdvanceOverlayUploadContinuity();
	});
}

bool AsyncVideoProvider::SetPreferredSourceModes(std::vector<SourceFrameOutputMode> modes) {
	if (modes.empty())
		modes.push_back(SourceFrameOutputMode::Bgra8);

	bool changed = false;
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		while (ProcessPending()) { }
		preferred_source_modes = std::move(modes);
		changed = ReconfigureSourceOutputMode();
	});
	return changed;
}

void AsyncVideoProvider::ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider> provider) {
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		while (ProcessPending()) { }
		auto old_provider = std::move(subs_provider);
		subs_provider = std::move(provider);
		if (subs_provider) {
			LOG_I(kSubtitleProviderUseLogTag) << "Activated subtitles provider: "
				<< subs_provider->GetDebugName()
				<< " (mode=" << SubtitleRenderModeName(subs_provider->GetRenderMode()) << ")";
			subs_provider->OnActivated();
		}
		old_provider.reset();
		bool const mode_changed = ReconfigureSourceOutputMode();
		single_frame = NEW_SUBS_FILE;
		last_rendered = -1;
		last_lines.clear();
		ResetCachedSourceFrame();
		if (!mode_changed) {
			AdvanceOverlayUploadContinuity();
			TrimReusablePools();
		}
	});
}

RawVideoIdentity AsyncVideoProvider::GetRawVideoIdentity() const noexcept {
	RawVideoIdentity identity;
	identity.generation = provider_version;
	try {
		if (source_provider) {
			identity.width = source_provider->GetWidth();
			identity.height = source_provider->GetHeight();
			identity.frame_count = source_provider->GetFrameCount();
		}
	} catch (...) {
		// Identity is a best-effort snapshot; getters are simple pass-throughs
		// but must never throw across this boundary.
	}
	return identity;
}

RawVideoBatchResult AsyncVideoProvider::RunRawVideoBatch(
	RawVideoIdentity expected_identity,
	std::function<RawVideoBatchStatus(RawFrameAccess&)> const& callback) {
	RawVideoBatchResult result;
	if (IsReentrantWorkerCall()) {
		result.status = RawVideoBatchStatus::Error;
		result.message = "nested raw video batch";
		return result;
	}
	worker->Sync([&] {
		WorkerSyncTracker sync_tracker(*this);
		try {
			RawVideoIdentity const live = GetRawVideoIdentity();
			if (!live.Matches(expected_identity)) {
				result.status = RawVideoBatchStatus::ProviderChanged;
				result.message = "raw video identity changed";
				return;
			}
			while (ProcessPending()) { }
			if (!source_provider) {
				result.status = RawVideoBatchStatus::Error;
				result.message = "no source provider";
				return;
			}
			RawFrameAccess access(*source_provider, live);
			result.status = callback(access);
		} catch (agi::Exception const& e) {
			result.status = RawVideoBatchStatus::DecodeError;
			result.message = e.GetMessage();
		} catch (...) {
			result.status = RawVideoBatchStatus::Error;
			result.message = "unknown batch failure";
		}
	});
	return result;
}

aegisub::motion_track::FrameReadResult RawFrameAccess::FetchBgra(
	int frame, aegisub::motion_track::RawBgraView& out) noexcept {
	try {
		if (frame < 0 || frame >= identity.frame_count)
			return {.status = aegisub::motion_track::FrameReadStatus::FrameUnavailable, .message = "frame out of range"};
		source.GetFrame(frame, scratch);
		// The checks are ordered so no size arithmetic runs before width and
		// height are known sane. The geometry comparison against the identity
		// comes first: the identity carries the provider's int dimensions, so
		// past it both fields are bounded by int range and the width * 4
		// product below cannot wrap size_t (before the reorder, a wrapped
		// product could pass a zero pitch into the division further down).
		// Each pitch condition then guards that division: pitch == 0 is
		// rejected outright, pitch >= width * 4 and data.size() >= pitch *
		// height hold by non-wrapping arithmetic, and pitches beyond int range
		// are rejected because RawBgraView carries the stride as int.
		if (scratch.data.empty() || scratch.data.data() == nullptr || scratch.width == 0 || scratch.height == 0)
			return {.status = aegisub::motion_track::FrameReadStatus::FrameUnavailable,
					.message = "invalid frame buffer"};
		auto const identity_width = static_cast<size_t>(identity.width);
		auto const identity_height = static_cast<size_t>(identity.height);
		if (scratch.width != identity_width || scratch.height != identity_height)
			return {.status = aegisub::motion_track::FrameReadStatus::FrameUnavailable,
					.message = "frame geometry does not match provider identity"};
		if (scratch.pitch == 0 || scratch.pitch < static_cast<size_t>(scratch.width) * 4 || scratch.pitch > static_cast<size_t>(std::numeric_limits<int>::max()) || scratch.data.size() / scratch.pitch < static_cast<size_t>(scratch.height))
			return {.status = aegisub::motion_track::FrameReadStatus::FrameUnavailable,
					.message = "invalid frame buffer"};
		out.data = scratch.data.data();
		out.width = static_cast<int>(scratch.width);
		out.height = static_cast<int>(scratch.height);
		out.pitch = static_cast<int>(scratch.pitch);
		out.flipped = scratch.flipped;
		return {.status = aegisub::motion_track::FrameReadStatus::Ok};
	} catch (agi::Exception const& e) {
		return {.status = aegisub::motion_track::FrameReadStatus::DecodeError, .message = e.GetMessage()};
	} catch (...) {
		return {.status = aegisub::motion_track::FrameReadStatus::Error, .message = "unknown decode failure"};
	}
}

bool AsyncVideoProvider::IsReentrantWorkerCall() const {
	std::lock_guard<std::mutex> lock(sync_state.mutex);
	return sync_state.depth > 0
	    && sync_state.owner == std::this_thread::get_id();
}
