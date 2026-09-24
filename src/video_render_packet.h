// Copyright (c) 2026
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

#pragma once

#include "source_frame.h"
#include "subtitle_overlay.h"
#include "video_frame.h"

#include <cstdint>
#include <memory>

class VisualToolRenderSnapshot;

struct VideoRenderDeliveryVersion {
	std::uint64_t provider = 0;
	std::uint64_t content = 0;
	std::uint64_t request = 0;

	bool operator==(VideoRenderDeliveryVersion const&) const = default;
};

enum class VideoRenderDeliveryClass {
	EveryFrame,
	VisualSubtitleIntermediate,
	VisualSubtitleFinal
};

struct VideoSubtitleUpdateOptions {
	VideoRenderDeliveryClass delivery_class = VideoRenderDeliveryClass::EveryFrame;
	std::uint64_t visual_interaction_id = 0;
	bool force_current_frame_render = false;
	std::shared_ptr<const VisualToolRenderSnapshot> visual_tool_snapshot;
};

struct VideoRenderPacket {
	// For compatibility-only subtitle providers, this may already hold the
	// final baked BGRA display frame rather than a subtitle-free source frame.
	std::shared_ptr<VideoFrame> source_frame_storage;
	std::shared_ptr<void> source_frame_owner;
	std::shared_ptr<VideoFrame> composited_frame_storage;
	std::shared_ptr<SubtitleOverlayStorage> subtitle_overlay_storage;

	int frame_number = -1;
	VideoRenderDeliveryVersion delivery_version;
	VideoRenderDeliveryClass delivery_class = VideoRenderDeliveryClass::EveryFrame;
	std::uint64_t visual_interaction_id = 0;
	std::shared_ptr<const VisualToolRenderSnapshot> visual_tool_snapshot;
	SourceFrame source_frame;
	SubtitleOverlay subtitle_overlay;
	bool has_subtitle_overlay = false;
	bool allow_source_frame_upload_reuse = true;
	double time = 0.0;

	bool HasDistinctCompositedFrame() const {
		return static_cast<bool>(composited_frame_storage)
			&& composited_frame_storage != source_frame_storage;
	}

	std::shared_ptr<VideoFrame> DisplayFrame() const {
		return composited_frame_storage ? composited_frame_storage : source_frame_storage;
	}
};
