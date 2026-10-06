#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace aegisub::motion_track {

struct GeometryTagResult {
	bool success = true;
	bool has_geometry = false;
	bool has_animated_clip = false;
	std::string text;
	std::string error;
};

class PreparedAffineGeometry {
	struct Coordinate {
		size_t begin, length;
		double value;
	};
	struct Tag {
		size_t begin, length;
		bool origin = false;
		bool rectangle = false;
		std::string shape;
		std::vector<Coordinate> coordinates;
	};
	std::string source;
	std::vector<Tag> tags;
	std::string error;
	bool origin_present = false;
	bool clip_present = false;

	public:
	explicit PreparedAffineGeometry(std::string_view text);
	[[nodiscard]] bool HasOrigin() const;
	[[nodiscard]] bool HasClip() const;
	[[nodiscard]] GeometryTagResult Transform(TrackTransform const& transform, bool origins) const;
};

// Transform absolute \org and \clip/\iclip coordinates with a matrix already
// expressed in script (PlayRes) coordinates. Unrelated bytes are preserved.
// Affine curves stay cubic; projective curves are subdivided with a convex-hull
// error bound in script pixels, including output drawing quantization.
// Rectangular clips inside \t remain supported only when the transformed clip
// is also a representable rectangle: libass cannot animate vector clips.
// Converting a rectangle in an event with additional clip tags is rejected,
// because vector and rectangular clips have different renderer precedence.
// On failure text is the original event, so callers can reject atomically.
GeometryTagResult TransformGeometryTags(std::string_view text,
										TrackTransform const& script_transform, double curve_tolerance = 0.1);

} // namespace aegisub::motion_track
