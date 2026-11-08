#pragma once

#include <libaegisub/color.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

struct VideoOverlayPerspectiveVertex {
	float x, y, z;
	float opacity;
};

template <size_t GridCount, size_t AxisCount, size_t ArrowCount>
class VideoOverlayStaticPerspectiveBatches {
	static_assert(GridCount && AxisCount && ArrowCount);
	static_assert(GridCount % 2 == 0 && AxisCount % 2 == 0 && ArrowCount % 2 == 0);
	static_assert(GridCount <= std::numeric_limits<int>::max() && AxisCount <= std::numeric_limits<int>::max() &&
				  ArrowCount <= std::numeric_limits<int>::max());

	std::array<VideoOverlayPerspectiveVertex, GridCount> const grid;
	std::array<VideoOverlayPerspectiveVertex, AxisCount> const axes;
	std::array<VideoOverlayPerspectiveVertex, ArrowCount> const arrows;
	float const maximum;

	static consteval float Validate(std::span<VideoOverlayPerspectiveVertex const> grid,
									std::span<VideoOverlayPerspectiveVertex const> axes,
									std::span<VideoOverlayPerspectiveVertex const> arrows) {
		float maximum = 0;
		for (auto vertices : {grid, axes, arrows})
			for (auto const& vertex : vertices) {
				if (!(vertex.opacity >= 0 && vertex.opacity <= 1))
					throw "Invalid perspective opacity";
				for (float coordinate : {vertex.x, vertex.y, vertex.z}) {
					if (!(coordinate >= -std::numeric_limits<float>::max() && coordinate <= std::numeric_limits<float>::max()))
						throw "Non-finite perspective vertex";
					float const magnitude = coordinate < 0 ? -coordinate : coordinate;
					if (magnitude > maximum)
						maximum = magnitude;
				}
			}
		return maximum;
	}

	public:
	consteval VideoOverlayStaticPerspectiveBatches(std::array<VideoOverlayPerspectiveVertex, GridCount> grid,
												   std::array<VideoOverlayPerspectiveVertex, AxisCount> axes,
												   std::array<VideoOverlayPerspectiveVertex, ArrowCount> arrows)
		: grid(grid), axes(axes), arrows(arrows), maximum(Validate(grid, axes, arrows)) {}
	[[nodiscard]] constexpr std::span<VideoOverlayPerspectiveVertex const> Grid() const { return grid; }
	[[nodiscard]] constexpr std::span<VideoOverlayPerspectiveVertex const> Axes() const { return axes; }
	[[nodiscard]] constexpr std::span<VideoOverlayPerspectiveVertex const> Arrows() const { return arrows; }
	[[nodiscard]] constexpr float MaxCoordinate() const { return maximum; }
};

inline void FillVideoOverlayPerspectiveGridColours(std::span<VideoOverlayPerspectiveVertex const> grid,
												   agi::Color const& secondary, std::span<float> colours) {
	if (colours.size() / 4 != grid.size() || colours.size() % 4 != 0)
		throw std::logic_error("Invalid perspective colour storage");
	float const r = secondary.r / 255.f, g = secondary.g / 255.f, b = secondary.b / 255.f;
	for (size_t i = 0; i < grid.size(); ++i) {
		colours[i * 4] = r;
		colours[i * 4 + 1] = g;
		colours[i * 4 + 2] = b;
		colours[i * 4 + 3] = grid[i].opacity;
	}
}

class VideoOverlayPreparedPerspectiveBatches final {
	std::vector<VideoOverlayPerspectiveVertex> const grid, axes, arrows;
	agi::Color const secondary;
	float const maximum;
	std::vector<float> const colours;

	static std::vector<float> PrepareColours(std::span<VideoOverlayPerspectiveVertex const> grid, agi::Color const& secondary) {
		std::vector<float> colours(grid.size() * 4);
		FillVideoOverlayPerspectiveGridColours(grid, secondary, colours);
		return colours;
	}

	public:
	template <size_t GridCount, size_t AxisCount, size_t ArrowCount>
	explicit VideoOverlayPreparedPerspectiveBatches(
		VideoOverlayStaticPerspectiveBatches<GridCount, AxisCount, ArrowCount> const& geometry, agi::Color const& secondary)
		: grid(geometry.Grid().begin(), geometry.Grid().end()), axes(geometry.Axes().begin(), geometry.Axes().end()),
		  arrows(geometry.Arrows().begin(), geometry.Arrows().end()), secondary(secondary), maximum(geometry.MaxCoordinate()),
		  colours(PrepareColours(grid, secondary)) {}
	VideoOverlayPreparedPerspectiveBatches(VideoOverlayPreparedPerspectiveBatches const&) = delete;
	VideoOverlayPreparedPerspectiveBatches& operator=(VideoOverlayPreparedPerspectiveBatches const&) = delete;
	[[nodiscard]] std::span<VideoOverlayPerspectiveVertex const> Grid() const { return grid; }
	[[nodiscard]] std::span<VideoOverlayPerspectiveVertex const> Axes() const { return axes; }
	[[nodiscard]] std::span<VideoOverlayPerspectiveVertex const> Arrows() const { return arrows; }
	[[nodiscard]] std::span<float const> GridColours() const { return colours; }
	[[nodiscard]] agi::Color const& SecondaryColour() const { return secondary; }
	[[nodiscard]] float MaxCoordinate() const { return maximum; }
};
