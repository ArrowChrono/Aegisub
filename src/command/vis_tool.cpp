// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "command.h"

#include "../include/aegisub/context.h"
#include "../include/aegisub/context_ui.h"
#include "../libresrc/libresrc.h"
#include "../project.h"
#include "../video_display.h"
#include "../visual_tool_clip.h"
#include "../visual_tool_cross.h"
#include "../visual_tool_drag.h"
#include "../visual_tool_measure.h"
#include "../visual_tool_rotatexy.h"
#include "../visual_tool_rotatez.h"
#include "../visual_tool_scale.h"
#include "../visual_tool_vector_clip.h"

#include <libaegisub/make_unique.h>

namespace {
	using cmd::Command;

	template<class T>
	struct visual_tool_command : public Command {
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

		bool Validate(const agi::Context *c) override {
			return !!c->GetCore().project->VideoProvider();
		}

		bool IsActive(const agi::Context *c) override {
			return c->GetUI().videoDisplay->ToolIsType(typeid(T));
		}

		void operator()(agi::Context *c) override {
			auto video_display = c->GetUI().videoDisplay;
			video_display->SetTool(agi::make_unique<T>(video_display, c));
		}
	};

	struct visual_geometry_linking final : public Command {
		CMD_NAME("video/tool/geometry_linking")
		CMD_ICON(toggle_audio_link)
		STR_MENU("Geometry Linking")
		STR_DISP("Geometry Linking")
		STR_HELP("Link origins and clips to supported subtitle geometry edits")
		CMD_TYPE(COMMAND_TOGGLE | COMMAND_VALIDATE)
		bool Validate(agi::Context const *c) override {
			return c->GetUI().videoDisplay != nullptr;
		}
		bool IsActive(agi::Context const *c) override {
			return c->GetUI().videoDisplay->GeometryLinkingEnabled();
		}
		void operator()(agi::Context *c) override {
			c->GetUI().videoDisplay->ToggleGeometryLinking();
		}
	};

	struct visual_mode_cross final : public visual_tool_command<VisualToolCross> {
		CMD_NAME("video/tool/cross")
		CMD_ICON(visual_standard)
		STR_MENU("Standard")
		STR_DISP("Standard")
		STR_HELP("Standard mode, double click sets position")
	};

	struct visual_mode_drag final : public visual_tool_command<VisualToolDrag> {
		CMD_NAME("video/tool/drag")
		CMD_ICON(visual_move)
		STR_MENU("Drag")
		STR_DISP("Drag")
		STR_HELP("Drag subtitles")
	};

	struct visual_mode_measure final : public visual_tool_command<VisualToolMeasure> {
		CMD_NAME("video/tool/measure")
		CMD_ICON(visual_measure)
		STR_MENU("Measure")
		STR_DISP("Measure")
		STR_HELP("Measure with temporary guides or draw a Perspective target for subtitles")
	};

	struct visual_mode_rotate_z final : public visual_tool_command<VisualToolRotateZ> {
		CMD_NAME("video/tool/rotate/z")
		CMD_ICON(visual_rotatez)
		STR_MENU("Rotate Z")
		STR_DISP("Rotate Z")
		STR_HELP("Rotate subtitles on their Z axis")
	};

	struct visual_mode_rotate_xy final : public visual_tool_command<VisualToolRotateXY> {
		CMD_NAME("video/tool/rotate/xy")
		CMD_ICON(visual_rotatexy)
		STR_MENU("Rotate XY")
		STR_DISP("Rotate XY")
		STR_HELP("Rotate subtitles on their X and Y axes")
	};

	struct visual_mode_scale final : public visual_tool_command<VisualToolScale> {
		CMD_NAME("video/tool/scale")
		CMD_ICON(visual_scale)
		STR_MENU("Scale")
		STR_DISP("Scale")
		STR_HELP("Scale subtitles on X and Y axes")
	};

	struct visual_mode_clip final : public visual_tool_command<VisualToolClip> {
		CMD_NAME("video/tool/clip")
		CMD_ICON(visual_clip)
		STR_MENU("Clip")
		STR_DISP("Clip")
		STR_HELP("Clip subtitles to a rectangle")
	};

	struct visual_mode_vector_clip final : public visual_tool_command<VisualToolVectorClip> {
		CMD_NAME("video/tool/vector_clip")
		CMD_ICON(visual_vector_clip)
		STR_MENU("Vector Clip")
		STR_DISP("Vector Clip")
		STR_HELP("Clip subtitles to a vectorial area")
	};

	template<VisualScaleAxis Axis>
	struct visual_scale_normalize : public Command {
		CMD_TYPE(COMMAND_VALIDATE)

		bool Validate(const agi::Context *c) override {
			auto *display = c->GetUI().videoDisplay;
			return display && display->CanNormalizeScaleTool(Axis);
		}

		void operator()(agi::Context *c) override {
			if (auto *display = c->GetUI().videoDisplay)
				display->NormalizeScaleTool(Axis);
		}
	};

	struct visual_scale_normalize_x final : public visual_scale_normalize<VisualScaleAxis::X> {
		CMD_NAME("video/tool/scale/normalize/x")
		CMD_ICON(visual_scale_x_100)
		STR_MENU("Scale: Set X to 100%")
		STR_DISP("Scale: Set X to 100%")
		STR_HELP("Set X scale to 100% and adjust Y to preserve the ratio")
	};

	struct visual_scale_normalize_y final : public visual_scale_normalize<VisualScaleAxis::Y> {
		CMD_NAME("video/tool/scale/normalize/y")
		CMD_ICON(visual_scale_y_100)
		STR_MENU("Scale: Set Y to 100%")
		STR_DISP("Scale: Set Y to 100%")
		STR_HELP("Set Y scale to 100% and adjust X to preserve the ratio")
	};

	template<int dx, int dy, VisualNudgeMagnitude Mag>
	struct visual_tool_nudge : public Command {
		CMD_TYPE(COMMAND_VALIDATE)

		bool Validate(const agi::Context *c) override {
			return c->GetUI().videoDisplay->CanNudgeTool();
		}

		void operator()(agi::Context *c) override {
			c->GetUI().videoDisplay->NudgeTool(Vector2D(dx, dy), Mag);
		}
	};

	struct visual_tool_nudge_left final : public visual_tool_nudge<-1, 0, VisualNudgeMagnitude::Normal> {
		CMD_NAME("video/tool/nudge/left")
		STR_MENU("Nudge left")
		STR_DISP("Nudge left")
		STR_HELP("Nudge the current visual tool value left by one step")
	};
	struct visual_tool_nudge_right final : public visual_tool_nudge<1, 0, VisualNudgeMagnitude::Normal> {
		CMD_NAME("video/tool/nudge/right")
		STR_MENU("Nudge right")
		STR_DISP("Nudge right")
		STR_HELP("Nudge the current visual tool value right by one step")
	};
	struct visual_tool_nudge_up final : public visual_tool_nudge<0, -1, VisualNudgeMagnitude::Normal> {
		CMD_NAME("video/tool/nudge/up")
		STR_MENU("Nudge up")
		STR_DISP("Nudge up")
		STR_HELP("Nudge the current visual tool value up by one step")
	};
	struct visual_tool_nudge_down final : public visual_tool_nudge<0, 1, VisualNudgeMagnitude::Normal> {
		CMD_NAME("video/tool/nudge/down")
		STR_MENU("Nudge down")
		STR_DISP("Nudge down")
		STR_HELP("Nudge the current visual tool value down by one step")
	};
	struct visual_tool_nudge_left_large final : public visual_tool_nudge<-1, 0, VisualNudgeMagnitude::Large> {
		CMD_NAME("video/tool/nudge/left/large")
		STR_MENU("Nudge left (large)")
		STR_DISP("Nudge left (large)")
		STR_HELP("Nudge the current visual tool value left by a large step")
	};
	struct visual_tool_nudge_right_large final : public visual_tool_nudge<1, 0, VisualNudgeMagnitude::Large> {
		CMD_NAME("video/tool/nudge/right/large")
		STR_MENU("Nudge right (large)")
		STR_DISP("Nudge right (large)")
		STR_HELP("Nudge the current visual tool value right by a large step")
	};
	struct visual_tool_nudge_up_large final : public visual_tool_nudge<0, -1, VisualNudgeMagnitude::Large> {
		CMD_NAME("video/tool/nudge/up/large")
		STR_MENU("Nudge up (large)")
		STR_DISP("Nudge up (large)")
		STR_HELP("Nudge the current visual tool value up by a large step")
	};
	struct visual_tool_nudge_down_large final : public visual_tool_nudge<0, 1, VisualNudgeMagnitude::Large> {
		CMD_NAME("video/tool/nudge/down/large")
		STR_MENU("Nudge down (large)")
		STR_DISP("Nudge down (large)")
		STR_HELP("Nudge the current visual tool value down by a large step")
	};

	// mode values match VisualToolMeasure::SubMode: segment=0, perspective=1
	template<int Mode>
	struct measure_submode : public Command {
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

		bool Validate(const agi::Context *c) override {
			return c->GetUI().videoDisplay->ToolIsType(typeid(VisualToolMeasure));
		}

		bool IsActive(const agi::Context *c) override {
			return Validate(c) && c->GetUI().videoDisplay->GetToolSubMode() == Mode;
		}

		void operator()(agi::Context *c) override {
			c->GetUI().videoDisplay->SetToolSubMode(Mode);
		}
	};

	struct measure_mode_segment final : public measure_submode<0> {
		CMD_NAME("video/tool/measure/segment")
		CMD_ICON(visual_measure)
		STR_MENU("Measure: Segment")
		STR_DISP("Measure: Segment")
		STR_HELP("Measure distances with temporary guides")
	};

	struct measure_mode_perspective final : public measure_submode<1> {
		CMD_NAME("video/tool/measure/perspective")
		CMD_ICON(visual_vector_clip_drag)
		STR_MENU("Measure: Perspective Quad")
		STR_DISP("Measure: Perspective Quad")
		STR_HELP("Draw a target quadrilateral for the active subtitle")
	};

	struct measure_perspective_apply final : public Command {
		CMD_NAME("video/tool/measure/perspective/apply")
		CMD_ICON(button_audio_commit)
		STR_MENU("Measure: Apply Perspective Quad")
		STR_DISP("Measure: Apply Perspective Quad")
		STR_HELP("Apply the drawn target quadrilateral to the active subtitle line")
		CMD_TYPE(COMMAND_VALIDATE)

		bool Validate(const agi::Context *c) override {
			return c->GetUI().videoDisplay->CanApplyMeasurePerspective();
		}

		void operator()(agi::Context *c) override {
			c->GetUI().videoDisplay->ApplyMeasurePerspective();
		}
	};

	struct measure_guide_delete final : public Command {
		CMD_NAME("video/tool/measure/guide/delete")
		CMD_ICON(delete_button)
		STR_MENU("Measure: Delete Guide")
		STR_DISP("Measure: Delete Guide")
		STR_HELP("Delete the selected measurement guide")
		CMD_TYPE(COMMAND_VALIDATE)

		bool Validate(const agi::Context *c) override {
			return c->GetUI().videoDisplay->CanRemoveMeasureGuide();
		}

		void operator()(agi::Context *c) override {
			c->GetUI().videoDisplay->RemoveMeasureGuide();
		}
	};

	// mode values match VisualToolVectorClip button IDs minus BUTTON_DRAG:
	// drag=0, line=1, bicubic=2, convert=3, insert=4, remove=5,
	// freehand=6, freehand_smooth=7, move=8
	template<int Mode>
	struct vector_clip_submode : public Command {
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

		bool Validate(const agi::Context *c) override {
			return c->GetUI().videoDisplay->ToolIsType(typeid(VisualToolVectorClip));
		}

		bool IsActive(const agi::Context *c) override {
			return Validate(c) && c->GetUI().videoDisplay->GetToolSubMode() == Mode;
		}

		void operator()(agi::Context *c) override {
			c->GetUI().videoDisplay->SetToolSubMode(Mode);
		}
	};

	struct vector_clip_mode_drag final : public vector_clip_submode<0> {
		CMD_NAME("video/tool/vector_clip/drag")
		CMD_ICON(visual_vector_clip_drag)
		STR_MENU("Vector clip: Drag")
		STR_DISP("Vector clip: Drag")
		STR_HELP("Drag vector clip control points")
	};
	struct vector_clip_mode_line final : public vector_clip_submode<1> {
		CMD_NAME("video/tool/vector_clip/line")
		CMD_ICON(visual_vector_clip_line)
		STR_MENU("Vector clip: Line")
		STR_DISP("Vector clip: Line")
		STR_HELP("Append a line to the vector clip")
	};
	struct vector_clip_mode_bicubic final : public vector_clip_submode<2> {
		CMD_NAME("video/tool/vector_clip/bicubic")
		CMD_ICON(visual_vector_clip_bicubic)
		STR_MENU("Vector clip: Bicubic")
		STR_DISP("Vector clip: Bicubic")
		STR_HELP("Append a bicubic curve to the vector clip")
	};
	struct vector_clip_mode_convert final : public vector_clip_submode<3> {
		CMD_NAME("video/tool/vector_clip/convert")
		CMD_ICON(visual_vector_clip_convert)
		STR_MENU("Vector clip: Convert")
		STR_DISP("Vector clip: Convert")
		STR_HELP("Convert a vector clip segment between line and bicubic")
	};
	struct vector_clip_mode_insert final : public vector_clip_submode<4> {
		CMD_NAME("video/tool/vector_clip/insert")
		CMD_ICON(visual_vector_clip_insert)
		STR_MENU("Vector clip: Insert")
		STR_DISP("Vector clip: Insert")
		STR_HELP("Insert a control point into the vector clip")
	};
	struct vector_clip_mode_remove final : public vector_clip_submode<5> {
		CMD_NAME("video/tool/vector_clip/remove")
		CMD_ICON(visual_vector_clip_remove)
		STR_MENU("Vector clip: Remove")
		STR_DISP("Vector clip: Remove")
		STR_HELP("Remove a control point from the vector clip")
	};
	struct vector_clip_mode_freehand final : public vector_clip_submode<6> {
		CMD_NAME("video/tool/vector_clip/freehand")
		CMD_ICON(visual_vector_clip_freehand)
		STR_MENU("Vector clip: Freehand")
		STR_DISP("Vector clip: Freehand")
		STR_HELP("Draw a freehand vector clip shape")
	};
	struct vector_clip_mode_freehand_smooth final : public vector_clip_submode<7> {
		CMD_NAME("video/tool/vector_clip/freehand_smooth")
		CMD_ICON(visual_vector_clip_freehand_smooth)
		STR_MENU("Vector clip: Freehand smooth")
		STR_DISP("Vector clip: Freehand smooth")
		STR_HELP("Draw a smoothed freehand vector clip shape")
	};
	struct vector_clip_mode_move final : public vector_clip_submode<8> {
		CMD_NAME("video/tool/vector_clip/move")
		CMD_ICON(visual_vector_clip_move)
		STR_MENU("Vector clip: Move")
		STR_DISP("Vector clip: Move")
		STR_HELP("Append a move point to the vector clip")
	};
}

namespace cmd {
	void init_visual_tools() {
		reg(agi::make_unique<visual_geometry_linking>());
		reg(agi::make_unique<visual_mode_cross>());
		reg(agi::make_unique<visual_mode_drag>());
		reg(agi::make_unique<visual_mode_rotate_z>());
		reg(agi::make_unique<visual_mode_rotate_xy>());
		reg(agi::make_unique<visual_mode_scale>());
		reg(agi::make_unique<visual_scale_normalize_x>());
		reg(agi::make_unique<visual_scale_normalize_y>());
		reg(agi::make_unique<visual_mode_clip>());
		reg(agi::make_unique<visual_mode_vector_clip>());
		reg(agi::make_unique<visual_mode_measure>());
		reg(agi::make_unique<measure_mode_segment>());
		reg(agi::make_unique<measure_mode_perspective>());
		reg(agi::make_unique<measure_perspective_apply>());
		reg(agi::make_unique<measure_guide_delete>());

		reg(agi::make_unique<visual_tool_nudge_left>());
		reg(agi::make_unique<visual_tool_nudge_right>());
		reg(agi::make_unique<visual_tool_nudge_up>());
		reg(agi::make_unique<visual_tool_nudge_down>());
		reg(agi::make_unique<visual_tool_nudge_left_large>());
		reg(agi::make_unique<visual_tool_nudge_right_large>());
		reg(agi::make_unique<visual_tool_nudge_up_large>());
		reg(agi::make_unique<visual_tool_nudge_down_large>());

		reg(agi::make_unique<vector_clip_mode_drag>());
		reg(agi::make_unique<vector_clip_mode_line>());
		reg(agi::make_unique<vector_clip_mode_move>());
		reg(agi::make_unique<vector_clip_mode_bicubic>());
		reg(agi::make_unique<vector_clip_mode_convert>());
		reg(agi::make_unique<vector_clip_mode_insert>());
		reg(agi::make_unique<vector_clip_mode_remove>());
		reg(agi::make_unique<vector_clip_mode_freehand>());
		reg(agi::make_unique<vector_clip_mode_freehand_smooth>());
	}
}
