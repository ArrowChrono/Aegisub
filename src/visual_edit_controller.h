#pragma once

#include "vector2d.h"
#include "visual_text_edit.h"
#include "visual_tool_render_snapshot.h"
#include "visual_tool_scale_policy.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace aegisub::visual_edit {

enum class Mode : std::uint8_t { Drag,
								 RectClip,
								 RotateZ,
								 RotateXY,
								 Scale };
enum class PointerPhase : std::uint8_t { Enter,
										 Move,
										 Leave,
										 Press,
										 Release,
										 DoubleClick };
enum class CaptureAction : std::uint8_t { None,
										  Capture,
										  Release };

struct Move {
	Vector2D start;
	Vector2D end;
	int start_ms = 0;
	int end_ms = 0;
};

struct RectClip {
	Vector2D first;
	Vector2D second;
	bool inverse = false;
};

struct Line {
	int id = 0;
	int row = -1;
	std::string text;
	int start_ms = 0;
	int first_frame = 0;
	int last_frame = -1;
	Vector2D position;
	std::optional<Move> move;
	std::optional<Vector2D> origin;
	RectClip clip;
	Vector2D scale{100, 100};
	float rotation_x = 0;
	float rotation_y = 0;
	float rotation_z = 0;
	float fax = 0;
	float fay = 0;
	int position_to_move_start_ms = 0;
	int position_to_move_end_ms = 0;
};

struct Viewport {
	Vector2D video_position;
	Vector2D video_size;
	Vector2D script_size;
	Vector2D layout_size{0, 0};
};

struct Frame {
	int number = 0;
	int time_ms = 0;
};

struct PointerInput {
	PointerPhase phase;
	Vector2D position;
	bool left_down = false;
	bool shift = false;
	bool control = false;
	bool alt = false;
};

struct Result {
	CaptureAction capture = CaptureAction::None;
	bool interaction_began = false;
	bool interaction_ended = false;
	bool selection_changed = false;
	bool geometry_changed = false;
	bool toolbar_changed = false;
};

enum class OverrideIntent : std::uint8_t { Translation,
										   Endpoint,
										   Origin };

struct OverrideEdit {
	int id;
	std::string tag;
	std::string value;
	OverrideIntent intent;
	Vector2D translation{0, 0};
};

using OverridePlanner = std::function<std::optional<std::vector<VisualTextEdit>>(std::span<OverrideEdit const>)>;

struct Style {
	unsigned long highlight_primary = 0;
	unsigned long highlight_secondary = 0;
	unsigned long line_primary = 0;
	unsigned long line_secondary = 0;
	float shaded_alpha = 0;
};

class Controller final {
	struct Impl;
	std::unique_ptr<Impl> impl;

	public:
	explicit Controller(Mode mode);
	~Controller();
	Controller(Controller const&) = delete;
	Controller& operator=(Controller const&) = delete;

	void Reset(std::vector<Line> lines, std::set<int> selected,
			   std::optional<int> active, Frame frame, Viewport viewport);
	void SetFrame(Frame frame);
	void UpdateLines(std::span<Line const> lines);
	void SetViewport(Viewport viewport);
	void SetSelection(std::set<int> selected, std::optional<int> active);

	Result Pointer(PointerInput const& input, VisualToolRenderSnapshot const *displayed_snapshot = nullptr, bool defer_begin = false, OverridePlanner const& planner = {});
	Result BeginPreparedPress(bool preserve_move_times = false, OverridePlanner const& planner = {});
	Result ToggleMovePosition();
	[[nodiscard]] bool CanNudge() const;
	Result Nudge(Vector2D direction, float step, float origin_step);
	[[nodiscard]] bool CanNormalizeScale(visual_tool_scale_policy::Axis axis) const;
	Result NormalizeScale(visual_tool_scale_policy::Axis axis);
	Result FinishInteraction();
	void AcceptChanges();

	[[nodiscard]] bool IsInteracting() const noexcept;
	[[nodiscard]] std::set<int> const& SelectedLines() const noexcept;
	[[nodiscard]] std::optional<int> ActiveLine() const noexcept;
	[[nodiscard]] bool ActiveLineHasMove() const noexcept;
	[[nodiscard]] std::vector<int> SelectedMoveLines() const;
	[[nodiscard]] std::span<const VisualTextEdit> IncrementalChanges() const noexcept;
	[[nodiscard]] std::span<const VisualTextEdit> DraftChanges() const noexcept;
	[[nodiscard]] std::shared_ptr<const VisualToolRenderSnapshot> CaptureSnapshot(
		std::shared_ptr<const VisualToolRenderContext> const& context, Style const& style) const;
};

}
