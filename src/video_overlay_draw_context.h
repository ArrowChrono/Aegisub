// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "vector2d.h"
#include "video_overlay_perspective_batches.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <libaegisub/color.h>

inline agi::Color VideoOverlayColourFromRgb(unsigned long rgb) {
	return {static_cast<unsigned char>(rgb), static_cast<unsigned char>(rgb >> 8), static_cast<unsigned char>(rgb >> 16)};
}

struct VideoOverlayTextExtent {
	int width = 0;
	int height = 0;

	VideoOverlayTextExtent() = default;
	VideoOverlayTextExtent(int width, int height) : width(width), height(height) {}
	[[nodiscard]] int GetWidth() const { return width; }
	[[nodiscard]] int GetHeight() const { return height; }
	bool operator==(VideoOverlayTextExtent const&) const = default;
};

struct VideoOverlayTextStyle {
	std::string face;
	int size = 12;
	bool bold = false;
	bool italic = false;
	agi::Color colour{255, 255, 255};
	bool outline = true;
};

class VideoOverlayTextRenderer {
	public:
	virtual ~VideoOverlayTextRenderer() = default;
	virtual VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) = 0;
	virtual void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) = 0;
};

struct VideoOverlayPerspectiveTransform {
	Vector2D origin;
	float angle_x, angle_y, angle_z;
	float fax, fay;
	float perspective_z_scale;
};

class VideoOverlayDrawContext {
public:
	virtual ~VideoOverlayDrawContext() = default;

	virtual void SetLineColour(agi::Color const& colour, float alpha = 1.0f, int width = 1) = 0;
	virtual void SetFillColour(agi::Color const& colour, float alpha = 1.0f) = 0;
	virtual void SetInvert() = 0;
	virtual void ClearInvert() = 0;

	virtual void DrawPerspectiveBatches(std::span<VideoOverlayPerspectiveVertex const>,
										std::span<VideoOverlayPerspectiveVertex const>, std::span<VideoOverlayPerspectiveVertex const>,
										VideoOverlayPerspectiveTransform const&, agi::Color const&, agi::Color const&, int) {
		throw std::logic_error("This overlay context does not support perspective lines");
	}
	virtual void DrawPreparedPerspectiveBatches(VideoOverlayPreparedPerspectiveBatches const& prepared,
												VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, int width) {
		DrawPerspectiveBatches(prepared.Grid(), prepared.Axes(), prepared.Arrows(), transform,
							   primary, prepared.SecondaryColour(), width);
	}

	virtual void DrawLine(Vector2D p1, Vector2D p2) = 0;
	virtual void DrawLines(size_t dim, float const *lines, size_t n) = 0;
	virtual void DrawLineStrip(Vector2D const *points, size_t n) = 0;
	virtual void DrawRectangle(Vector2D p1, Vector2D p2) = 0;
	virtual void DrawPolygon(Vector2D const *points, size_t n) = 0;
	virtual void DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& start, std::vector<int> const& count, Vector2D video_pos, Vector2D video_size, bool invert) = 0;
	virtual void DrawCircle(Vector2D center, float radius) = 0;
	virtual void DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) = 0;

	virtual VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) = 0;
	virtual void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) = 0;
};
