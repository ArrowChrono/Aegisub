#pragma once

#include "vector2d.h"
#include "visual_text_edit.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

class AssDialogue;
class AssStyle;

namespace aegisub::visual_geometry {

enum class EditKind : std::uint8_t { Affine,
									 Translation,
									 Endpoint,
									 Perspective };

struct Edit {
	std::string tag, value;
	EditKind kind = EditKind::Affine;
	Vector2D translation{0, 0};
};

struct Request;
struct PlanResult;

class Snapshot {
	struct Impl;
	std::unique_ptr<Impl> impl;
	friend PlanResult Plan(std::span<Request const>, Vector2D, bool);

	public:
	Snapshot(AssDialogue const& source, AssStyle const *style, Vector2D script_size);
	~Snapshot();
	Snapshot(Snapshot&&) noexcept;
	Snapshot& operator=(Snapshot&&) noexcept;
	[[nodiscard]] AssDialogue const& Source() const;
	[[nodiscard]] bool HasClip() const;
	[[nodiscard]] bool HasOrigin() const;
};

struct Request {
	Snapshot const *source;
	std::span<Edit const> edits;
	bool restore_source = false;
};

struct PlanResult {
	std::vector<VisualTextEdit> edits;
	std::string error;
	explicit operator bool() const noexcept { return error.empty(); }
};

PlanResult Plan(std::span<Request const> requests, Vector2D layout_size, bool restore_source = false);

}
