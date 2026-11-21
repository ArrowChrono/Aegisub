#include "gui_test_wx_boundary.h"

#include <wx/defs.h>

#ifdef __WXMSW__

#include "app_launch_plan.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_karaoke.h"
#include "audio_box.h"
#include "audio_display.h"
#include "audio_controller.h"
#include "audio_timing.h"
#include "base_grid.h"
#include "command/command.h"
#include "dialog_motion_track.h"
#include "dialog_manager.h"
#include "dialog_styling_assistant.h"
#include "project.h"
#include "placeholder_ctrl.h"
#include "options.h"
#include "subs_edit_box.h"
#include "compat.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "subtitle_grid_folding.h"
#include "video_controller.h"
#include "video_display.h"
#include "visual_guide_overlay.h"
#include "visual_tool.h"
#include "visual_tool_drag.h"

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/io.h>
#include <wx/debug.h>
#include <wx/eventfilter.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
wxAssertHandler_t startup_assertion_handler = nullptr;

wxAssertHandler_t CurrentAssertionHandler() noexcept {
#if wxDEBUG_LEVEL
	return wxTheAssertHandler;
#else
	return nullptr;
#endif
}

json::Object AssertionChannelState() {
	auto const handler = CurrentAssertionHandler();
	json::Object state;
	state["wx_debug_level"] = wxDEBUG_LEVEL;
	state["mode"] = startup_assertion_handler ? "startup-opt-in" : "ordinary-startup";
	state["handler_enabled"] = handler != nullptr;
	state["handler_matches_startup"] = startup_assertion_handler && handler == startup_assertion_handler;
	return state;
}
}

void EnableWxBoundaryTestAssertions(AppLaunchPlan const& launch_plan) {
	if (!launch_plan.ParseSucceeded() || launch_plan.RequestedPlainProcess() || launch_plan.mode != AppLaunchMode::GuiTest || !launch_plan.gui_test_host || !launch_plan.gui_test_run)
		return;
	auto const *value = std::getenv("AEGISUB_GUI_TEST_WX_ASSERTIONS");
	if (!value || std::string_view(value) != "1")
		return;
	if (startup_assertion_handler)
		throw std::logic_error("wx boundary assertion channel was already initialized");
	wxSetDefaultAssertHandler();
	startup_assertion_handler = CurrentAssertionHandler();
	if (!startup_assertion_handler)
		throw std::runtime_error("wx boundary assertion channel requires compiled wx assertions");
}

class GuiTestWxBoundary final : public wxEventFilter {
	agi::Context& context;
	VideoDisplay& display;
	json::Object& receipt;
	json::Array events, native_events, assertions, steps;
	int64_t step_index = -1;
	char const *step_name = "outside-step";
	HWND hwnd;
	inline static thread_local GuiTestWxBoundary *active = nullptr;

	void TagEvent(json::Object& event) const {
		event["step_index"] = step_index;
		event["step_name"] = step_name;
	}

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
		auto& observer = *reinterpret_cast<GuiTestWxBoundary *>(data);
		if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_CAPTURECHANGED || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
			json::Object event;
			observer.TagEvent(event);
			event["canvas_identity"] = reinterpret_cast<int64_t>(window);
			event["message"] = static_cast<int64_t>(message);
			event["wparam"] = static_cast<int64_t>(wp);
			event["lparam"] = static_cast<int64_t>(lp);
			event["capture_owner"] = reinterpret_cast<int64_t>(::GetCapture());
			observer.native_events.emplace_back(std::move(event));
		}
		return ::DefSubclassProc(window, message, wp, lp);
	}

	int FilterEvent(wxEvent& event) override {
		if (event.GetEventObject() != &display)
			return Event_Skip;
		json::Object value;
		if (event.GetEventType() == wxEVT_ENTER_WINDOW || event.GetEventType() == wxEVT_LEAVE_WINDOW || event.GetEventType() == wxEVT_MOTION || event.GetEventType() == wxEVT_LEFT_DOWN || event.GetEventType() == wxEVT_LEFT_UP) {
			auto const& mouse = static_cast<wxMouseEvent const&>(event);
			value["kind"] = mouse.Entering() ? "enter" : mouse.Leaving() ? "leave"
													 : mouse.LeftDown()  ? "left-down"
													 : mouse.LeftUp()    ? "left-up"
																		 : "motion";
			value["x"] = mouse.GetX();
			value["y"] = mouse.GetY();
			value["left_down"] = mouse.LeftIsDown();
			value["shift_down"] = mouse.ShiftDown();
			value["control_down"] = mouse.ControlDown();
			value["alt_down"] = mouse.AltDown();
		}
		else if (event.GetEventType() == wxEVT_MOUSE_CAPTURE_LOST)
			value["kind"] = "capture-lost";
		else
			return Event_Skip;
		TagEvent(value);
		if (step_index >= 0)
			value["state_before"] = State(context, display);
		value["native_capture"] = display.HasCapture();
		value["visual_interacting"] = display.IsVisualToolInteracting();
		events.emplace_back(std::move(value));
		return Event_Skip;
	}

	class KeyboardState final {
		std::array<BYTE, 256> previous{};

		public:
		KeyboardState(bool left, bool shift, bool control, bool alt) {
			if (!::GetKeyboardState(previous.data()))
				throw std::runtime_error("could not read GUI thread keyboard state");
			auto next = previous;
			for (int key : {VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU})
				next[key] = 0;
			next[VK_LBUTTON] = left ? 0x80 : 0;
			next[VK_SHIFT] = next[VK_LSHIFT] = shift ? 0x80 : 0;
			next[VK_CONTROL] = next[VK_LCONTROL] = control ? 0x80 : 0;
			next[VK_MENU] = next[VK_LMENU] = alt ? 0x80 : 0;
			if (!::SetKeyboardState(next.data()))
				throw std::runtime_error("could not set GUI thread fixture keyboard state");
		}
		~KeyboardState() {
			if (!::SetKeyboardState(previous.data()))
				std::terminate();
		}
		KeyboardState(KeyboardState const&) = delete;
		KeyboardState& operator=(KeyboardState const&) = delete;
	};

	static POINT Cursor(json::Object& result, VideoDisplay& display) {
		POINT point{};
		::SetLastError(0);
		bool const actual = ::GetCursorPos(&point) != FALSE;
		result["get_cursor_pos_succeeded"] = actual;
		result["get_cursor_pos_error"] = static_cast<int64_t>(actual ? 0 : ::GetLastError());
		auto const message_position = ::GetMessagePos();
		result["message_position"] = static_cast<int64_t>(message_position);
		if (!actual) {
			point.x = GET_X_LPARAM(message_position);
			point.y = GET_Y_LPARAM(message_position);
		}
		result["cursor_source"] = actual ? "GetCursorPos" : "GetMessagePos";
		result["cursor_x"] = static_cast<int64_t>(point.x);
		result["cursor_y"] = static_cast<int64_t>(point.y);
		auto window = ::WindowFromPoint(point);
		result["cursor_window"] = reinterpret_cast<int64_t>(window);
		while (window && window != display.GetHandle())
			window = ::GetParent(window);
		result["cursor_window_matches"] = window != nullptr;
		auto client = point;
		if (!::ScreenToClient(static_cast<HWND>(display.GetHandle()), &client))
			throw std::runtime_error("could not convert observed cursor to canvas coordinates");
		result["client_x"] = static_cast<int64_t>(client.x);
		result["client_y"] = static_cast<int64_t>(client.y);
		return point;
	}

	GuiTestWxBoundary(agi::Context& context, VideoDisplay& display, json::Object& receipt)
		: context(context), display(display), receipt(receipt), hwnd(static_cast<HWND>(display.GetHandle())) {
		if (active || !agi::dispatch::IsMainThread())
			throw std::runtime_error("wx boundary observer requires a non-nested GUI thread dispatch");
		if (!::SetWindowSubclass(hwnd, WindowProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
			throw std::runtime_error("could not observe owned canvas native messages");
		wxEvtHandler::AddFilter(this);
		active = this;
	}

	~GuiTestWxBoundary() override {
		active = nullptr;
		wxEvtHandler::RemoveFilter(this);
		if (!::RemoveWindowSubclass(hwnd, WindowProc, reinterpret_cast<UINT_PTR>(this)))
			std::terminate();
		receipt["wx_events"] = std::move(events);
		receipt["native_events"] = std::move(native_events);
		receipt["assertions"] = std::move(assertions);
		receipt["steps"] = std::move(steps);
	}

	static json::Object State(agi::Context& context, VideoDisplay& display) {
		auto const core = context.GetCore();
		auto const& selected = core.selectionController->GetSelectedSet();
		auto const *active_line = core.selectionController->GetActiveLine();
		auto& folding = core.ass->Folding();
		json::Object state;
		state["document_revision"] = core.subsController->GetDocumentRevision();
		state["document_generation"] = static_cast<int64_t>(core.subsController->GetDocumentGeneration());
		state["modified"] = core.subsController->IsModified();
		state["undo_empty"] = core.subsController->IsUndoStackEmpty();
		state["redo_empty"] = core.subsController->IsRedoStackEmpty();
		state["undo_description"] = core.subsController->GetUndoDescription();
		state["redo_description"] = core.subsController->GetRedoDescription();
		state["active_id"] = static_cast<int64_t>(active_line ? active_line->Id : -1);
		state["video_frame"] = static_cast<int64_t>(core.videoController->GetFrameN());
		if (auto const *edit_box = context.GetUI().subsEditBox) {
			if (auto const caret = edit_box->GetEditControlCaret()) {
				state["editor_selection_start"] = caret->first;
				state["editor_selection_end"] = caret->second;
			}
		}
		json::Array selected_ids, rows, groups;
		for (auto const *line : selected)
			selected_ids.emplace_back(static_cast<int64_t>(line->Id));
		int source_row = 0;
		for (auto const& line : core.ass->Events) {
			json::Object row;
			row["source_id"] = static_cast<int64_t>(line.Id);
			row["source_row"] = source_row;
			row["display_row"] = folding.SourceToDisplay(source_row++);
			row["selected"] = std::ranges::any_of(selected, [&line](auto const *item) { return item == &line; });
			row["text"] = line.Text.get();
			row["event"] = line.GetEntryData();
			rows.emplace_back(std::move(row));
		}
		for (auto const& group : folding.Groups()) {
			json::Object row;
			row["id"] = static_cast<int64_t>(group.id);
			row["start"] = group.start;
			row["end"] = group.end;
			row["collapsed"] = folding.IsCollapsed(group);
			groups.emplace_back(std::move(row));
		}
		state["selected_ids"] = std::move(selected_ids);
		state["rows"] = std::move(rows);
		state["fold_groups"] = std::move(groups);
		state["display_rows"] = static_cast<int64_t>(folding.DisplayRows().size());
		state["native_capture"] = display.HasCapture();
		state["visual_interacting"] = display.IsVisualToolInteracting();
		state["presentation_snapshot_available"] = static_cast<bool>(display.GetToolPresentationSnapshot());
		if (!display.tool)
			throw std::runtime_error("wx boundary test requires an active visual tool");
		state["interaction_pacing_active"] = display.tool->interaction_render_pacer.IsActive();
		for (auto const& [prefix, point] : {std::pair{"display", display.mouse_pos}, std::pair{"tool", display.tool->mouse_pos}}) {
			state[std::string(prefix) + "_mouse_valid"] = static_cast<bool>(point);
			state[std::string(prefix) + "_mouse_x"] = point ? json::UnknownElement(point.X()) : json::UnknownElement(json::Null{});
			state[std::string(prefix) + "_mouse_y"] = point ? json::UnknownElement(point.Y()) : json::UnknownElement(json::Null{});
		}
		auto const viewport = display.GetVisualGuideViewport();
		json::Object allocation;
		allocation["x"] = viewport.canvas_x;
		allocation["y"] = viewport.canvas_y;
		allocation["width"] = viewport.canvas_width;
		allocation["height"] = viewport.canvas_height;
		allocation["script_width"] = viewport.script_width;
		allocation["script_height"] = viewport.script_height;
		state["viewport"] = std::move(allocation);
		return state;
	}

	static json::Object MotionBorrowState(agi::Context& context, VideoDisplay& display) {
		auto core = context.GetCore();
		auto ui = context.GetUI();
		auto *styling = ui.dialog->Get<DialogStyling>();
		if (!styling || !ui.subsEditBox->edit_ctrl_tc)
			throw std::runtime_error("Motion Apply native probe requires Styling Assistant and native text editor");
		json::Object result;
		auto live = [&](AssDialogue const *borrowed) {
			return !borrowed || std::ranges::any_of(core.ass->Events, [&](auto const& row) { return &row == borrowed; });
		};
		bool grid_live = true;
		for (int i = 0; i < ui.subsGrid->GetRows(); ++i)
			grid_live = grid_live && live(ui.subsGrid->GetDialogue(i));
		result["grid_borrows_live"] = grid_live;
		result["edit_borrow_live"] = live(ui.subsEditBox->line);
		result["edit_can_focus"] = ui.subsEditBox->CanFocusEditControl();
		result["pending_text_after"] = ui.subsEditBox->visual_tool_text_sync_pending;
		result["styling_borrow_live"] = live(styling->active_line);
		result["visual_borrow_live"] = live(display.tool->active_line);
		result["karaoke_present"] = ui.karaoke != nullptr;
		result["karaoke_borrow_live"] = !ui.karaoke || live(ui.karaoke->active_line);
		result["row_count"] = static_cast<int64_t>(core.ass->Events.size());
		result["undo_available"] = !core.subsController->IsUndoStackEmpty();
		result["all_borrows_live"] = grid_live && live(ui.subsEditBox->line) && live(styling->active_line) && live(display.tool->active_line) && (!ui.karaoke || live(ui.karaoke->active_line));
#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
		auto *audio = dynamic_cast<AudioDisplay *>(ui.audioBox->GetAudioDisplayWindow());
#else
		auto *audio = ui.audioBox->audioDisplay;
#endif
		if (!audio)
			throw std::runtime_error("Motion Apply native probe requires wx AudioDisplay");
		result["karaoke_timing_enabled"] = ui.karaoke->IsEnabled();
		result["audio_marker_present"] = static_cast<bool>(audio->audio_marker);
		result["audio_dragged_present"] = audio->dragged_object != nullptr;
		result["audio_scroll_timer"] = audio->scroll_timer.IsRunning();
		result["audio_capture"] = audio->HasCapture();
		result["audio_hwnd"] = static_cast<int64_t>(reinterpret_cast<uintptr_t>(audio->GetHandle()));
		auto *timing = core.audioController->GetTimingController();
		AudioMarkerVector markers;
		timing->GetMarkers(TimeRange(0, 2000), markers);
		result["audio_marker_count"] = static_cast<int64_t>(markers.size());
		result["audio_timing_ready"] = timing->IsSubtitleRowBindingReady();
		result["visual_ready"] = display.tool->SubtitleRowsReady();
		result["styling_ready"] = styling->row_borrows_ready;
		result["grid_ready"] = ui.subsGrid->row_borrows_ready;
		result["grid_font_size"] = ui.subsGrid->font.GetPointSize();
		result["grid_line_height"] = ui.subsGrid->lineHeight;
		result["grid_background"] = static_cast<int64_t>(ui.subsGrid->row_colors.Default.GetColour().GetRGB());
		result["requested_grid_font_size"] = static_cast<int64_t>(OPT_GET("Subtitle/Grid/Font Size")->GetInt());
		result["requested_grid_background"] = static_cast<int64_t>(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Background")->GetColor()).GetRGB());
		auto *drag = dynamic_cast<VisualToolDrag *>(display.tool.get());
		if (!drag)
			throw std::runtime_error("Motion Apply native probe requires Drag");
		auto feature_live = [&](VisualToolDragDraggableFeature const *borrowed) {
			return !borrowed || std::ranges::any_of(drag->features, [&](auto const& feature) { return &feature == borrowed && live(feature.line); });
		};
		bool drag_live = feature_live(drag->primary) && feature_live(drag->active_feature);
		for (auto const& feature : drag->features) drag_live = drag_live && live(feature.line);
		for (auto const *feature : drag->sel_features) drag_live = drag_live && feature_live(feature);
		for (auto const *line : drag->selection) drag_live = drag_live && live(line);
		for (auto const *line : drag->double_click_selection) drag_live = drag_live && live(line);
		if (drag->frame_visibility)
			for (auto const *line : drag->frame_visibility->Visible()) drag_live = drag_live && live(line);
		result["drag_borrows_live"] = drag_live;
		result["drag_features"] = static_cast<int64_t>(drag->features.size());
		result["drag_selected_features"] = static_cast<int64_t>(drag->sel_features.size());
		result["drag_cached_selection"] = static_cast<int64_t>(drag->selection.size());
		result["drag_visibility_present"] = static_cast<bool>(drag->frame_visibility);
		result["drag_primary_present"] = drag->primary != nullptr;
		result["all_borrows_live"] = static_cast<json::Boolean const&>(result.at("all_borrows_live")) && drag_live;
		result["grid_capture"] = ui.subsGrid->HasCapture();
		result["edit_ready"] = ui.subsEditBox->row_borrows_ready;
		result["edit_hwnd"] = static_cast<int64_t>(reinterpret_cast<uintptr_t>(ui.subsEditBox->edit_ctrl_tc->GetHandle()));
		result["styling_hwnd"] = static_cast<int64_t>(reinterpret_cast<uintptr_t>(styling->style_name->GetHandle()));
		result["edit_text"] = from_wx(ui.subsEditBox->edit_ctrl_tc->GetValue());
		result["styling_text"] = from_wx(styling->style_name->GetValue());
		auto combo_state = [](auto *combo) {
			json::Object state;
			json::Array candidates;
			for (auto const& value : combo->GetStrings()) candidates.push_back(from_wx(value));
			state["candidates"] = std::move(candidates);
			state["value"] = from_wx(combo->GetValue());
			return state;
		};
		result["style_combo"] = combo_state(ui.subsEditBox->style_box);
		result["actor_combo"] = combo_state(ui.subsEditBox->actor_box);
		result["effect_combo"] = combo_state(ui.subsEditBox->effect_box);
		return result;
	}

	static json::Object MotionApplyFault(agi::Context& context, VideoDisplay& display, bool selection_fault, bool karaoke_timing, bool grid_capture) {
		using namespace aegisub::motion_track;
		auto core = context.GetCore();
		auto ui = context.GetUI();
		if (!ui.subsGrid || !ui.subsEditBox || !display.tool)
			throw std::runtime_error("Motion Apply fault probe requires real grid, edit box and visual tool");
		auto *styling = ui.dialog->ShowOnce<DialogStyling>(&context);
		if (!styling)
			throw std::runtime_error("Motion Apply fault probe could not create Styling Assistant");
		DialogMotionTrack dialog(&context);
		cmd::call("video/tool/drag", &context);
		ApplyPlanInput input;
		input.timecodes = core.project->Timecodes();
		if (!input.timecodes.IsLoaded())
			throw std::runtime_error("Motion Apply fault probe requires loaded video timecodes");
		input.storage_width = input.script_width = 640;
		input.storage_height = input.script_height = 480;
		input.video_frame_count = 20;
		input.decode_interval = input.direction_domain = {.first = 0, .last = 19};
		input.options.compact_epsilon = 100;
		for (int frame = 0; frame < 20; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.status = TrackStatus::Ok;
			sample.center_x = frame * 2;
			sample.transform.matrix[2] = frame * 2;
			input.samples.push_back(sample);
		}
		auto targets = core.selectionController->GetSortedSelection();
		if (targets.empty())
			throw std::runtime_error("Motion Apply fault probe requires selected lines");
		dialog.apply_source_.Capture(*core.ass, targets, input.timecodes, input.video_frame_count);
		std::string message;
		auto apply = [&](ApplyMode mode) {
			input.options.mode = mode;
			auto plan = BuildApplyPlan(*core.ass, dialog.apply_source_.Targets(), input);
			if (!plan.has_mutations())
				throw std::runtime_error("Motion Apply fault probe plan failed: " + plan.message);
			if (!dialog.InstallApplyPlan(plan, message))
				throw std::runtime_error("Motion Apply fault probe setup failed: " + message);
		};
		json::Object result;
		if (karaoke_timing)
			ui.karaoke->SetEnabled(true);
		if (selection_fault) {
			apply(ApplyMode::Exact);
			auto state = MotionBorrowState(context, display);
			result["initial_exact_all_ready"] = static_cast<json::Boolean const&>(state.at("grid_ready")) && static_cast<json::Boolean const&>(state.at("edit_ready")) && static_cast<json::Boolean const&>(state.at("styling_ready")) && static_cast<json::Boolean const&>(state.at("visual_ready")) && static_cast<json::Boolean const&>(state.at("audio_timing_ready"));
		}
		auto drag_state = MotionBorrowState(context, display);
		result["drag_features_before"] = std::move(drag_state.at("drag_features"));
		result["drag_selected_features_before"] = std::move(drag_state.at("drag_selected_features"));
		result["drag_cached_selection_before"] = std::move(drag_state.at("drag_cached_selection"));
		result["drag_visibility_before"] = std::move(drag_state.at("drag_visibility_present"));
#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
		auto *audio = dynamic_cast<AudioDisplay *>(ui.audioBox->GetAudioDisplayWindow());
#else
		auto *audio = ui.audioBox->audioDisplay;
#endif
		if (!audio)
			throw std::runtime_error("Motion Apply fault probe requires wx AudioDisplay");
		auto *timing = core.audioController->GetTimingController();
		AudioMarkerVector markers;
		timing->GetMarkers(TimeRange(0, 2000), markers);
		auto marker = std::ranges::find_if(markers, [&](auto const *item) { return timing->IsNearbyMarker(item->GetPosition(), 1, false); });
		if (marker == markers.end())
			throw std::runtime_error("Motion Apply native probe requires a draggable timing marker");
		auto const marker_time = (*marker)->GetPosition();
		audio->ScrollTimeRangeInView(TimeRange(std::max(0, marker_time - 100), marker_time + 100));
		auto const marker_x = audio->RelativeXFromTime(marker_time);
		auto const marker_y = audio->audio_top + audio->audio_height / 2;
		if (!audio->GetClientRect().Contains(marker_x, marker_y))
			throw std::runtime_error("Motion Apply native audio marker is outside its client");
		if (grid_capture) {
			auto *grid = ui.subsGrid;
			auto const point = MAKELPARAM(grid->GetClientSize().x / 2, grid->lineHeight + grid->lineHeight / 2);
			::SendMessage(grid->GetHandle(), WM_LBUTTONDOWN, MK_LBUTTON, point);
			result["capture_owner_hwnd"] = static_cast<int64_t>(reinterpret_cast<uintptr_t>(grid->GetHandle()));
			result["capture_owner_point"] = static_cast<int64_t>(point);
		}
		else {
			::SendMessage(audio->GetHandle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(marker_x, marker_y));
			result["capture_owner_hwnd"] = static_cast<int64_t>(reinterpret_cast<uintptr_t>(audio->GetHandle()));
			result["capture_owner_point"] = static_cast<int64_t>(MAKELPARAM(marker_x, marker_y));
		}
		result["grid_capture_before"] = ui.subsGrid->HasCapture();
		result["audio_marker_point"] = static_cast<int64_t>(MAKELPARAM(marker_x, marker_y));
		result["audio_marker_before"] = static_cast<bool>(audio->audio_marker);
		result["audio_dragged_before"] = audio->dragged_object != nullptr;
		ui.subsEditBox->QueueVisualToolTextSync();
		result["pending_text_before"] = ui.subsEditBox->visual_tool_text_sync_pending;
		result["styling_borrow_before"] = styling->active_line != nullptr;
		result["visual_borrow_before"] = display.tool->active_line != nullptr;
		int fault_count = 0;
		agi::signal::Connection fault = selection_fault
											? core.selectionController->AddSelectionListener([&] { ++fault_count; throw std::runtime_error("injected Motion Apply selection fault"); })
											: core.ass->AddCommitListener([&] { ++fault_count; throw std::runtime_error("injected Motion Apply early commit fault"); });
		bool publication_failed = false;
		try {
			apply(selection_fault ? ApplyMode::Compact : ApplyMode::Exact);
		}
		catch (MotionTrackApplyPublicationError const&) {
			publication_failed = true;
		}
		fault.Disconnect();
		result["publication_failed"] = publication_failed;
		result["fault_count"] = fault_count;
		for (auto& [key, value] : MotionBorrowState(context, display))
			result[key] = std::move(value);
		return result;
	}

	static json::Array EventSlice(json::Array const& source, size_t first) {
		json::Array result;
		for (size_t i = first; i < source.size(); ++i) {
			std::stringstream serialized;
			agi::JsonWriter::Write(source[i], serialized);
			json::UnknownElement copy;
			json::Reader::Read(copy, serialized);
			result.emplace_back(std::move(copy));
		}
		return result;
	}

	static bool StableCursor(json::Object const& before, json::Object const& after) {
		for (char const *field : {"cursor_x", "cursor_y", "client_x", "client_y", "cursor_window", "get_cursor_pos_error"}) {
			if (static_cast<json::Integer const&>(before.at(field)) != static_cast<json::Integer const&>(after.at(field)))
				return false;
		}
		return static_cast<json::String const&>(before.at("cursor_source")) == static_cast<json::String const&>(after.at("cursor_source")) &&
			   static_cast<json::Boolean const&>(before.at("get_cursor_pos_succeeded")) == static_cast<json::Boolean const&>(after.at("get_cursor_pos_succeeded")) &&
			   static_cast<json::Boolean const&>(before.at("cursor_window_matches")) && static_cast<json::Boolean const&>(after.at("cursor_window_matches"));
	}

	template <typename Action>
	void Step(char const *name, bool require_stable_cursor, Action&& action) {
		step_index = static_cast<int64_t>(steps.size());
		step_name = name;
		json::Object step, cursor_before, cursor_after;
		step["index"] = step_index;
		step["name"] = name;
		step["before"] = State(context, display);
		auto const first_wx = events.size(), first_native = native_events.size();
		Cursor(cursor_before, display);
		std::exception_ptr error;
		try {
			action();
		}
		catch (...) {
			error = std::current_exception();
		}
		Cursor(cursor_after, display);
		auto const stable = StableCursor(cursor_before, cursor_after);
		step["cursor_before"] = std::move(cursor_before);
		step["cursor_after"] = std::move(cursor_after);
		step["after"] = State(context, display);
		step["wx_events"] = EventSlice(events, first_wx);
		step["native_events"] = EventSlice(native_events, first_native);
		steps.emplace_back(std::move(step));
		if (error)
			std::rethrow_exception(error);
		if (require_stable_cursor && !stable)
			throw std::runtime_error("native transaction cursor observation changed or left owned canvas");
	}

	void NativeTransaction(FrameMain& frame, std::optional<wxPoint>& saved_position, int x, int y, bool idle) {
		if (display.HasCapture() || display.IsVisualToolInteracting())
			throw std::runtime_error("native transaction requires idle state without capture");
		if (!display.GetClientRect().Contains(x, y) || ::GetCapture() || ::GetWindowThreadProcessId(hwnd, nullptr) != ::GetCurrentThreadId())
			throw std::runtime_error("native transaction requires an owned client point and GUI thread without capture");
		Step("align", false, [&] {
			if (saved_position && !idle)
				throw std::runtime_error("native fixture alignment is already active");
			json::Object observation;
			auto const cursor = Cursor(observation, display);
			auto const screen = display.ClientToScreen(wxPoint(x, y));
			if (!saved_position)
				saved_position = frame.GetPosition();
			frame.Move(frame.GetPosition() + wxPoint(cursor.x - screen.x, cursor.y - screen.y));
			Cursor(observation, display);
			if (!static_cast<json::Boolean const&>(observation.at("cursor_window_matches")) ||
				static_cast<json::Integer const&>(observation.at("client_x")) != x || static_cast<json::Integer const&>(observation.at("client_y")) != y)
				throw std::runtime_error("observed cursor does not match owned canvas point after alignment");
		});
		auto dispatch = [&](char const *name, UINT message, bool left, bool shift = false, bool control = false) {
			Step(name, true, [&] {
				KeyboardState keyboard(left, shift, control, false);
				WPARAM const flags = (left ? MK_LBUTTON : 0) | (shift ? MK_SHIFT : 0) | (control ? MK_CONTROL : 0);
				::SendMessageW(hwnd, message, flags, MAKELPARAM(static_cast<short>(x), static_cast<short>(y)));
			});
		};
		dispatch("prime-leave", WM_MOUSELEAVE, false);
		dispatch("enter", WM_MOUSEMOVE, false);
		if (!idle)
			dispatch("down", WM_LBUTTONDOWN, true);
		dispatch("leave", WM_MOUSELEAVE, false, !idle, !idle);
		if (!idle)
			dispatch("reenter", WM_MOUSEMOVE, true);
	}

	static void ProbeAssertionChannel() {
		wxFAIL_MSG(wxS("AEGISUB_WX_BOUNDARY_ASSERTION_PROBE"));
	}

	public:
	GuiTestWxBoundary(GuiTestWxBoundary const&) = delete;
	GuiTestWxBoundary& operator=(GuiTestWxBoundary const&) = delete;

	static bool RecordAssertion(wxString const& file, int line, wxString const& function, wxString const& condition, wxString const& message) {
		if (!active)
			return false;
		json::Object assertion;
		active->TagEvent(assertion);
		assertion["file"] = from_wx(wxFileName(file).GetFullName());
		assertion["line"] = line;
		assertion["function"] = from_wx(function);
		assertion["condition"] = from_wx(condition);
		assertion["message"] = from_wx(message);
		active->assertions.emplace_back(std::move(assertion));
		return true;
	}

	static void Install(FrameMain& frame, agi::Context& context, agi::fs::path const& artifacts, WxBoundaryPublisher const& publish) {
		wxWeakRef<VideoDisplay> observed(context.GetUI().videoDisplay);
		auto target = [&context, observed] {
			auto *display = context.GetUI().videoDisplay;
			if (!agi::dispatch::IsMainThread() || !display || display != observed.get())
				throw std::runtime_error("wx boundary target changed or is outside the GUI thread");
			return display;
		};
		frame.Bind(wxEVT_MENU, [&context, target, publish, sequence = 0](wxCommandEvent&) mutable {
			auto state = State(context, *target());
			state["sequence"] = ++sequence;
			publish("visual-state-" + std::to_string(sequence) + ".json", std::move(state)); }, 0x7FFD);
		frame.Bind(wxEVT_MENU, [&frame, &context, artifacts, target, publish, sequence = 0, saved_position = std::optional<wxPoint>{}](wxCommandEvent&) mutable {
			json::Object reply;
			auto const current = ++sequence;
			reply["sequence"] = current;
			reply["status"] = "failed";
			reply["assertion_channel_before"] = AssertionChannelState();
			try {
				if (!frame.GetMenuBar() || frame.GetMenuBar()->FindItem(0x7FF6) || frame.FindWindow(0x7FF6))
					throw std::runtime_error("wx boundary command collides with a real control");
				auto stream = agi::io::Open(artifacts / "wx-boundary-request.json");
				json::UnknownElement parsed;
				json::Reader::Read(parsed, *stream);
				auto const& request = static_cast<json::Object const&>(parsed);
				auto integer = [&request](char const* name) { return static_cast<int64_t>(static_cast<json::Integer const&>(request.at(name))); };
				auto boolean = [&request](char const* name) { return static_cast<bool>(static_cast<json::Boolean const&>(request.at(name))); };
				auto const token = static_cast<json::String const&>(request.at("token"));
				auto const kind = static_cast<json::String const&>(request.at("kind"));
				auto* display = target();
				auto const x = integer("x"), y = integer("y");
				if (integer("sequence") != current || token.empty() || integer("canvas_identity") != reinterpret_cast<int64_t>(display->GetHandle()) || x < -32768 || x > 32767 || y < -32768 || y > 32767)
					throw std::runtime_error("wx boundary request identity or coordinates are invalid");
				reply["token"] = token;
				reply["kind"] = kind;
				reply["canvas_identity"] = integer("canvas_identity");
				reply["before"] = State(context, *display);
				if (!startup_assertion_handler || CurrentAssertionHandler() != startup_assertion_handler)
					throw std::runtime_error("wx boundary dispatch requires the enabled startup assertion channel");
				{
					GuiTestWxBoundary observer(context, *display, reply);
					if (kind == "assertion-channel-probe")
						ProbeAssertionChannel();
					else if (kind == "release-capture" || kind == "native-capture-loss") {
						if (!display->HasCapture() || !display->IsVisualToolInteracting())
							throw std::runtime_error("capture transition requires an active captured interaction");
						if (kind == "release-capture")
							display->ReleaseMouse();
						else if (!::ReleaseCapture())
							throw std::runtime_error("native capture release failed");
					}
					else if (kind == "native-boundary-transaction" || kind == "native-idle-boundary-transaction") {
						observer.NativeTransaction(frame, saved_position, static_cast<int>(x), static_cast<int>(y), kind == "native-idle-boundary-transaction");
					}
					else if (kind == "native-restore") {
						if (display->HasCapture() || display->IsVisualToolInteracting() || !saved_position)
							throw std::runtime_error("native fixture restore requires idle state and a saved frame position");
						frame.Move(*saved_position);
						saved_position.reset();
					}
					else if (kind == "motion-apply-load-rebind") {
						context.GetCore().project->LoadSubtitles(artifacts / "rebind.ass", "UTF-8", false);
						reply["motion_apply"] = MotionBorrowState(context, *display);
					}
					else if (kind == "motion-apply-retired-style") {
						if (context.GetUI().subsGrid->row_borrows_ready)
							throw std::runtime_error("Grid style recovery probe requires retired row borrows");
						OPT_SET("Subtitle/Grid/Font Size")->SetInt(OPT_GET("Subtitle/Grid/Font Size")->GetInt() + 5);
						auto const previous = OPT_GET("Colour/Subtitle Grid/Background/Background")->GetColor();
						OPT_SET("Colour/Subtitle Grid/Background/Background")->SetColor(agi::Color(255 - previous.r, 255 - previous.g, 255 - previous.b));
						reply["motion_apply"] = MotionBorrowState(context, *display);
					}
					else if (kind == "motion-apply-inspect") {
						reply["motion_apply"] = MotionBorrowState(context, *display);
					}
					else if (kind == "motion-apply-early" || kind == "motion-apply-selection" || kind == "motion-apply-early-karaoke" || kind == "motion-apply-selection-karaoke" || kind == "motion-apply-early-grid" || kind == "motion-apply-selection-grid") {
						reply["motion_apply"] = MotionApplyFault(context, *display, kind == "motion-apply-selection" || kind == "motion-apply-selection-karaoke" || kind == "motion-apply-selection-grid", kind == "motion-apply-early-karaoke" || kind == "motion-apply-selection-karaoke", kind == "motion-apply-early-grid" || kind == "motion-apply-selection-grid");
					}
					else if (kind == "enter" || kind == "leave" || kind == "motion") {
						wxMouseEvent event(kind == "enter" ? wxEVT_ENTER_WINDOW : kind == "leave" ? wxEVT_LEAVE_WINDOW
																								  : wxEVT_MOTION);
						event.SetPosition(wxPoint(static_cast<int>(x), static_cast<int>(y)));
						event.SetLeftDown(boolean("left_down"));
						event.SetShiftDown(boolean("shift_down"));
						event.SetControlDown(boolean("control_down"));
						event.SetAltDown(boolean("alt_down"));
						event.SetEventObject(display);
						event.SetId(display->GetId());
						reply["processed"] = display->ProcessWindowEvent(event);
					}
					else
						throw std::runtime_error("unknown wx boundary request kind");
				}
				reply["after"] = State(context, *display);
				reply["status"] = "dispatched";
			}
			catch (agi::Exception const& error) { reply["error"] = error.GetMessage(); }
			catch (std::exception const& error) { reply["error"] = error.what(); }
			reply["assertion_channel_after"] = AssertionChannelState();
			publish("wx-boundary-" + std::to_string(current) + ".json", std::move(reply)); }, 0x7FF6);
	}
};

void InstallWxBoundaryTestHost(FrameMain& frame, agi::Context& context, agi::fs::path const& artifacts, WxBoundaryPublisher const& publish) {
	GuiTestWxBoundary::Install(frame, context, artifacts, publish);
}
bool RecordWxBoundaryTestAssertion(wxString const& file, int line, wxString const& function, wxString const& condition, wxString const& message) {
	return GuiTestWxBoundary::RecordAssertion(file, line, function, condition, message);
}
#endif
