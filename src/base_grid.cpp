// Copyright (c) 2006, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "base_grid.h"

#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/menu.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_box.h"
#include "compat.h"
#include "grid_core/grid_layout.h"
#include "grid_column.h"
#include "grid_column_painter.h"
#include "options.h"
#include "perf_trace.h"
#include "presentation/subtitle_grid_diff.h"
#include "presentation/subtitle_grid_projection.h"
#include "project.h"
#include "utils.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "subtitle_grid_selection_policy.h"
#include "subtitle_grid_folding.h"
#include "video_controller.h"
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
#include "subtitle_grid_renderer_slot.h"
#endif

#include <libaegisub/make_unique.h>
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <chrono>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>

// Check menu.h for id range allocation before editing this enum
enum {
	GRID_SCROLLBAR = 1730,
	MENU_SHOW_COL = (wxID_HIGHEST + 1) + 2000 // Needs 15 IDs after this
};

namespace {
	constexpr double GridTimingLogThresholdMs = 16.0;

	double DurationMs(
		std::chrono::steady_clock::time_point started,
		std::chrono::steady_clock::time_point finished = std::chrono::steady_clock::now()) noexcept
	{
		return std::chrono::duration<double, std::milli>(finished - started).count();
	}

	double CurrentWindowsMessageAgeMs() noexcept {
#ifdef _WIN32
		auto const now = static_cast<DWORD>(::GetTickCount());
		auto const raw_message_time = ::GetMessageTime();
		if (raw_message_time == 0 || raw_message_time == -1)
			return -1.0;
		auto const message_time = static_cast<DWORD>(raw_message_time);
		return static_cast<double>(static_cast<DWORD>(now - message_time));
#else
		return -1.0;
#endif
	}
}

BaseGrid::BaseGrid(wxWindow* parent, agi::Context *context)
: wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxSUNKEN_BORDER)
, scrollBar(new wxScrollBar(this, GRID_SCROLLBAR, wxDefaultPosition, wxDefaultSize, wxSB_VERTICAL))
, context(context)
, current_frame(context->GetCore().videoController->GetFrameN())
, columns(GetGridColumns())
, columns_visible(OPT_GET("Subtitle/Grid/Column")->GetListBool())
{
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	renderer_slot = std::make_unique<aegisub::grid::SubtitleGridRendererSlot>();
	width_renderer_generation = renderer_slot->Generation();
#endif
	scrollBar->SetScrollbar(0,10,100,10);

	auto scrollbarpositioner = new wxBoxSizer(wxHORIZONTAL);
	scrollbarpositioner->AddStretchSpacer();
	scrollbarpositioner->Add(scrollBar, 0, wxEXPAND, 0);

	SetSizerAndFit(scrollbarpositioner);

	SetBackgroundStyle(wxBG_STYLE_PAINT);

	for (size_t i : agi::util::range(std::min(columns_visible.size(), columns.size()))) {
		if (!columns_visible[i])
			columns[i]->SetVisible(false);
	}

	UpdateStyle();
	OnHighlightVisibleChange(*OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame"));

	auto core = context->GetCore();
	connections = agi::signal::make_vector({
		core.ass->AddCommitListener(&BaseGrid::OnSubtitlesCommit, this),

		core.selectionController->AddActiveLineListener(&BaseGrid::OnActiveLineChanged, this),
		core.selectionController->AddSelectionListener([&]{
			perf_trace::VideoUiDurationScope trace("grid_select.grid.selection", GetRows());
			bool const cancelled_preview = drag_selection_preview_active
				&& !committing_drag_selection_preview
				&& !handling_mouse_selection;
			if (cancelled_preview)
				ClearDragSelectionPreview(true);
			++grid_revision;
			auto new_selected_rows = GetSelectedRowsInWindow();
			if (cancelled_preview)
				Refresh(false);
			else
				RefreshChangedVisibleRows(selected_rows, new_selected_rows);
			selected_rows = std::move(new_selected_rows);
		}),
		core.project->AddVideoProviderListener(&BaseGrid::OnVideoProviderChanged, this),
		core.videoController->AddFramePresentedListener(&BaseGrid::OnCurrentFrameChanged, this),

		OPT_SUB("Subtitle/Grid/Font Face", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Subtitle/Grid/Font Size", &BaseGrid::UpdateStyle, this),
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
		OPT_SUB("Subtitle/Grid/Skia/ClearType", [this](agi::OptionValue const&) {
			NotifyTextRasterPolicyChanged();
		}),
#endif
		OPT_SUB("Colour/Subtitle Grid/Active Border", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Background", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Comment", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Inframe", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Selected Comment", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Selection", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Collision", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Header", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Left Column", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Lines", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Selection", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Standard", &BaseGrid::UpdateStyle, this),

		OPT_SUB("Subtitle/Grid/Highlight Subtitles in Frame", &BaseGrid::OnHighlightVisibleChange, this),
		OPT_SUB("Subtitle/Grid/Hide Overrides", [&](agi::OptionValue const&) { Refresh(false); }),
		OPT_SUB("Subtitle/Grid/Show Folding Column", [this] {
			SetColumnWidths();
			Refresh(false);
		}),
		OPT_SUB("Subtitle/Character Counter/Show Decimal CPS", &BaseGrid::UpdateStyle, this),
	});

	Bind(wxEVT_CONTEXT_MENU, &BaseGrid::OnContextMenu, this);
	Bind(wxEVT_DPI_CHANGED, &BaseGrid::OnDPIChanged, this);
}

BaseGrid::~BaseGrid() { }

#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
bool BaseGrid::CanUseSkiaRenderer() const noexcept {
	return renderer_slot
		&& renderer_slot->UsesSkia()
		&& renderer_slot->SupportsContentScale(GetContentScaleFactor());
}
#endif

BEGIN_EVENT_TABLE(BaseGrid,wxWindow)
	EVT_PAINT(BaseGrid::OnPaint)
	EVT_SIZE(BaseGrid::OnSize)
	EVT_COMMAND_SCROLL(GRID_SCROLLBAR,BaseGrid::OnScroll)
	EVT_MOUSE_EVENTS(BaseGrid::OnMouseEvent)
	EVT_MOUSE_CAPTURE_LOST(BaseGrid::OnMouseCaptureLost)
	EVT_KEY_DOWN(BaseGrid::OnKeyDown)
	EVT_CHAR_HOOK(BaseGrid::OnCharHook)
	EVT_MENU_RANGE(MENU_SHOW_COL,MENU_SHOW_COL+15,BaseGrid::OnShowColMenu)
	EVT_IDLE(BaseGrid::OnIdle)
END_EVENT_TABLE()

void BaseGrid::OnSubtitlesCommit(int type, const AssDialogue *single_line) {
	if (type == AssFile::COMMIT_NEW || (type & AssFile::COMMIT_ORDER) || (type & AssFile::COMMIT_DIAG_ADDREM) || (type & AssFile::COMMIT_EXTRADATA))
		ClearDragSelectionPreview(true);

	auto const before_revision = grid_revision;
	++grid_revision;
	std::vector<std::string> diff_column_ids;
	if (type & AssFile::COMMIT_DIAG_TIME)
		diff_column_ids = {
			aegisub::presentation::SubtitleGridColumnIdStart,
			aegisub::presentation::SubtitleGridColumnIdEnd,
			aegisub::presentation::SubtitleGridColumnIdCps,
		};
	else if (type & AssFile::COMMIT_DIAG_TEXT)
		diff_column_ids = {aegisub::presentation::SubtitleGridColumnIdText};

	auto const diff = aegisub::presentation::BuildSubtitleGridDiffFromCommit(
		type,
		before_revision,
		grid_revision,
		single_line,
		single_line ? ResolveGridRowState(*single_line) : aegisub::presentation::SubtitleGridRowState{},
		diff_column_ids);

	if (diff.kind == aegisub::presentation::SubtitleGridDiffKind::Reset) {
		// A commit that is exactly COMMIT_ORDER (sort / move / swap) permutes
		// existing lines without touching any field or the row count, so the
		// column widths cannot have changed. Skipping the remeasure avoids
		// projecting and re-measuring every row in the file on each sort.
		// COMMIT_NEW is 0 and any combination with ADDREM/META still remeasures.
		bool const view_or_order_only = (type & ~(AssFile::COMMIT_ORDER | AssFile::COMMIT_EXTRADATA)) == 0 && type != AssFile::COMMIT_NEW;
		UpdateMaps(!view_or_order_only, type != AssFile::COMMIT_NEW);
		auto const *active = context->GetCore().selectionController->GetActiveLine();
		for (auto const& line : context->GetCore().ass->Events) {
			if (&line == active) {
				if (reveal_active_after_commit || DisplayRow(line.Row) < 0) {
					extendRow = active_row = line.Row;
					MakeRowVisible(line.Row);
				}
				break;
			}
		}
		reveal_active_after_commit = false;
		return;
	}

	if (diff.requires_full_refresh && (type & AssFile::COMMIT_DIAG_META)) {
		SetColumnWidths();
		Refresh(false);
		return;
	}
	if (diff.kind == aegisub::presentation::SubtitleGridDiffKind::RowsChanged && (type & AssFile::COMMIT_DIAG_TIME)) {
		// Dragging start / end time in audio display can generate lots of commit in a short period of time.
		// On the other hand, GDI painting time depends on area, and BaseGrid typically is very large. Therefore repainting BaseGrid can be expensive.
		// To prevent GUI lag / FPS drop caused by frequent repaint of BaseGrid, collect row invalidations and repaint them when idle.
		if (single_line) {
			QueueSubtitleGridRowRefresh(single_line->Row);
			auto new_visible_rows = GetRowsDisplayedAtCurrentFrame();
			QueueChangedVisibleRowsRefresh(visible_rows, new_visible_rows);
			visible_rows = std::move(new_visible_rows);

			if (context->GetCore().selectionController->GetActiveLine() == single_line)
				QueueVisibleWindowRefresh();
		}
		else {
			refresh_on_idle = true;
			full_refresh_on_idle = true;
			subtitle_rows_refresh_on_idle.clear();
		}
	}
	else if (diff.kind == aegisub::presentation::SubtitleGridDiffKind::RowsChanged && (type & AssFile::COMMIT_DIAG_TEXT)) {
		if (!diff.upserted_rows.empty()) {
			RefreshSubtitleGridRow(diff.upserted_rows.front().row_index);
			return;
		}

		for (auto const& rect : text_refresh_rects)
			RefreshRect(rect, false);
	}
}

void BaseGrid::OnShowColMenu(wxCommandEvent &event) {
	int item = event.GetId() - MENU_SHOW_COL;
	bool new_value = !columns_visible[item];

	columns_visible.resize(columns.size(), true);
	columns_visible[item] = new_value;
	OPT_SET("Subtitle/Grid/Column")->SetListBool(columns_visible);
	columns[item]->SetVisible(new_value);

	SetColumnWidths();

	Refresh(false);
}

void BaseGrid::OnHighlightVisibleChange(agi::OptionValue const& opt) {
	(void)opt;
	++grid_revision;
	Refresh(false);
}

void BaseGrid::UpdateStyle() {
	wxString fontname = FontFace("Subtitle/Grid");
	if (fontname.empty()) fontname = wxS("Tahoma");
	font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Subtitle/Grid/Font Size")->GetInt());
	font.SetWeight(wxFONTWEIGHT_NORMAL);
	if (width_helper)
		width_helper->Reset();

	wxClientDC dc(this);
	dc.SetFont(font);

	// Set line height
	lineHeight = dc.GetCharHeight() + 4;
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (CanUseSkiaRenderer()) {
		try {
			if (auto painter = renderer_slot->CreateMeasurementPainter(font)) {
				int measured_width = 0;
				int measured_height = 0;
				painter->MeasureText(L"Ag", measured_width, measured_height);
				if (measured_height > 0)
					lineHeight = measured_height + 4;
			}
		}
		catch (std::exception const& error) {
			LOG_W("subtitle/grid/renderer")
				<< "Skia line-height measurement failed; retaining wx metric: "
				<< error.what();
		}
	}
#endif

	// Set row brushes
	row_colors.Default.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Background")->GetColor()));
	row_colors.Header.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Header")->GetColor()));
	row_colors.Selection.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
	row_colors.Comment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Comment")->GetColor()));
	row_colors.Visible.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Inframe")->GetColor()));
	row_colors.SelectedComment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selected Comment")->GetColor()));
	row_colors.LeftCol.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Left Column")->GetColor()));

	SetColumnWidths();

	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::UpdateMaps(bool remeasure_columns, bool preserve_anchor) {
	auto const previous_rows = index_line_map.size();
	index_line_map.clear();
	projection_line_map.clear();
	index_line_map.reserve(previous_rows);
	projection_line_map.reserve(previous_rows);

	auto core = context->GetCore();
	for (auto& curdiag : core.ass->Events) {
		index_line_map.push_back(&curdiag);
		projection_line_map.push_back(&curdiag);
	}

	UpdateDisplayMap(preserve_anchor);
	if (remeasure_columns || measured_fold_column_width != FoldColumnWidth())
		SetColumnWidths();
}

int BaseGrid::GetDisplayRows() const {
	return static_cast<int>(context->GetCore().ass->Folding().DisplayRows().size());
}

int BaseGrid::SourceRow(int display_row) const {
	return context->GetCore().ass->Folding().DisplayToSource(display_row);
}

int BaseGrid::DisplayRow(int source_row) const {
	return context->GetCore().ass->Folding().SourceToDisplay(source_row);
}

int BaseGrid::FoldColumnWidth() const {
	if (!OPT_GET("Subtitle/Grid/Show Folding Column")->GetBool() || context->GetCore().ass->Folding().Groups().empty())
		return 0;
	return FromDIP(20);
}

void BaseGrid::UpdateDisplayMap(bool preserve_anchor) {
	auto& folding = context->GetCore().ass->Folding();
	int anchor_id = preserve_anchor && yPos >= 0 && std::cmp_less(yPos, display_line_ids.size())
						? display_line_ids[yPos]
						: -1;
	if (anchor_id >= 0) {
		for (auto const& line : context->GetCore().ass->Events) {
			if (line.Id != anchor_id)
				continue;
			int row = folding.SourceToDisplay(line.Row);
			if (row < 0) {
				if (auto const *group = folding.GroupAt(line.Row))
					row = folding.SourceToDisplay(group->start);
			}
			if (row >= 0)
				yPos = row;
			break;
		}
	}
	display_line_ids.clear();
	display_line_ids.reserve(folding.DisplayRows().size());
	for (auto const& row : folding.DisplayRows())
		display_line_ids.push_back(row.dialogue->Id);
	++grid_revision;
	subtitle_rows_refresh_on_idle.clear();
	AdjustScrollbar();
	visible_rows = GetRowsDisplayedAtCurrentFrame();
	selected_rows = GetSelectedRowsInWindow();
	Refresh(false);
}

void BaseGrid::OnActiveLineChanged(AssDialogue *new_active) {
	perf_trace::VideoUiDurationScope trace("grid_select.grid.active", GetRows());
	bool const cancelled_preview = drag_selection_preview_active
		&& !committing_drag_selection_preview
		&& !handling_mouse_selection;
	if (cancelled_preview) {
		ClearDragSelectionPreview(true);
		selected_rows = GetSelectedRowsInWindow();
	}
	++grid_revision;

	if (new_active) {
		if (GetDialogue(new_active->Row) == new_active)
			MakeRowVisible(new_active->Row);
		else
			reveal_active_after_commit = true;
		extendRow = active_row = new_active->Row;
		Refresh(false);
	}
	else {
		active_row = -1;
		if (cancelled_preview)
			Refresh(false);
	}
}

void BaseGrid::MakeRowVisible(int row) {
	if (DisplayRow(row) < 0) {
		// Some edit commands announce selection after mutating Events but before
		// Commit. Keep the old folding baseline for structural repair, and defer
		// projection until its pointers have been rebound to the committed file.
		auto const& events = context->GetCore().ass->Events;
		if (!std::ranges::equal(events, index_line_map,
								[](AssDialogue const& line, AssDialogue const *cached) { return &line == cached; })) {
			reveal_active_after_commit = true;
			return;
		}
	}
	if (context->GetCore().ass->Folding().EnsureVisible(row))
		UpdateDisplayMap();
	row = DisplayRow(row);
	if (row < 0)
		return;
	int h = GetClientSize().GetHeight();

	if (row < yPos + 1)
		ScrollTo(row - 1);
	else if (row > yPos + h/lineHeight - 3)
		ScrollTo(row - h/lineHeight + 3);
}

void BaseGrid::OnCurrentFrameChanged(int frame_number) {
	current_frame = frame_number;
	++grid_revision;
	auto new_visible_rows = GetRowsDisplayedAtCurrentFrame();
	if (new_visible_rows == visible_rows)
		return;

	RefreshChangedVisibleRows(visible_rows, new_visible_rows);
	visible_rows = std::move(new_visible_rows);
}

void BaseGrid::OnVideoProviderChanged() {
	auto core = context->GetCore();
	current_frame = core.project->VideoProvider() ? core.videoController->GetFrameN() : -1;
	++grid_revision;
	Refresh(false);
}

void BaseGrid::OnIdle(wxIdleEvent&) {
	if (refresh_on_idle) {
		refresh_on_idle = false;
		if (full_refresh_on_idle) {
			full_refresh_on_idle = false;
			subtitle_rows_refresh_on_idle.clear();
			Refresh(false);
		}
		else {
			FlushQueuedSubtitleGridRowRefreshes();
		}
	}
}

void BaseGrid::OnPaint(wxPaintEvent &) {
	auto const grid_paint_started = InputTimingClock::now();
	auto click_timing = pending_click_paint_timing;
	pending_click_paint_timing.pending = false;
	auto const paint_started = click_timing.pending
		? InputTimingClock::now()
		: InputTimingClock::time_point{};
	auto log_click_paint_timing = agi::make_scope_exit([click_timing, paint_started] {
		if (!click_timing.pending)
			return;
		auto const paint_finished = InputTimingClock::now();
		auto const paint_queue_wait_ms =
			DurationMs(click_timing.handler_finished, paint_started);
		auto const paint_ms = DurationMs(paint_started, paint_finished);
		auto const click_to_paint_end_ms =
			DurationMs(click_timing.click_started, paint_finished);
		if (!click_timing.slow_input &&
			paint_queue_wait_ms < GridTimingLogThresholdMs &&
			paint_ms < GridTimingLogThresholdMs &&
			click_to_paint_end_ms < GridTimingLogThresholdMs)
			return;
		LOG_I("subtitle/grid/input_timing")
			<< "event_id=" << click_timing.event_id
			<< " phase=first_paint_complete"
			<< " row=" << click_timing.row
			<< " message_age_ms=" << click_timing.message_age_ms
			<< " handler_ms=" << click_timing.handler_ms
			<< " focus_ms=" << click_timing.focus_ms
			<< " paint_queue_wait_ms=" << paint_queue_wait_ms
			<< " paint_ms=" << paint_ms
			<< " click_to_paint_end_ms=" << click_to_paint_end_ms;
	});

	int w = 0;
	int h = 0;
	GetClientSize(&w,&h);
	auto const layout = aegisub::grid::CalculateGridLayout({
		.client_width = w,
		.client_height = h,
		.vertical_scrollbar_width = scrollBar->GetSize().GetWidth(),
		.line_height = lineHeight,
		.row_count = GetDisplayRows(),
		.first_visible_row = yPos,
	});
	w = layout.grid_width;
	h = layout.client_height;
	const int nDraw = layout.rows_to_draw;
	int const fold_column_width = FoldColumnWidth();

	// Find which columns and visible rows need to be repainted
	std::vector<char> paint_columns;
	paint_columns.resize(columns.size(), false);
	bool any = false;
	bool paint_header = false;
	int first_dirty_row = nDraw;
	int last_dirty_row = -1;
	for (wxRegionIterator region(GetUpdateRegion()); region; ++region) {
		wxRect updrect = region.GetRect();
		if (updrect.x < w && updrect.x + updrect.width > 0)
			any = true;
		if (updrect.y <= lineHeight && updrect.y + updrect.height > 0)
			paint_header = true;
		if (nDraw && updrect.y + updrect.height > lineHeight) {
			int first = updrect.y <= lineHeight ? 0 : (updrect.y - lineHeight) / lineHeight;
			int last = (updrect.y + updrect.height - 1 - lineHeight) / lineHeight;
			first_dirty_row = std::min(first_dirty_row, mid(0, first, nDraw - 1));
			last_dirty_row = std::max(last_dirty_row, mid(0, last, nDraw - 1));
		}

		int x = fold_column_width;
		for (size_t i : agi::util::range(columns.size())) {
			int width = columns[i]->Width();
			if (width && updrect.x < x + width && updrect.x + updrect.width > x) {
				paint_columns[i] = true;
			}
			x += width;
		}
	}

	if (!any) {
		if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
			auto const update_box = GetUpdateRegion().GetBox();
			LOG_I("subtitle/grid/paint_skip")
				<< "reason=empty-content-update"
				<< " client_width=" << w
				<< " client_height=" << h
				<< " update_x=" << update_box.x
				<< " update_y=" << update_box.y
				<< " update_width=" << update_box.width
				<< " update_height=" << update_box.height;
		}
		wxBufferedPaintDC dc(this);
		dc.SetBackground(row_colors.Default);
		dc.Clear();
		return;
	}

	auto paint_grid = [&](GridColumnPainter& backend_painter) {
	auto *painter = &backend_painter;
	painter->SetFont(font);
	painter->Clear(from_wx(row_colors.Default.GetColour()));

	// Draw labels
	bool const has_dirty_rows = first_dirty_row <= last_dirty_row;
	if (has_dirty_rows) {
		int const top = (first_dirty_row + 1) * lineHeight;
		int const height = (last_dirty_row - first_dirty_row + 1) * lineHeight + 1;
		painter->FillRectangle(0, top, fold_column_width + columns[0]->Width(), height,
							   from_wx(row_colors.LeftCol.GetColour()));
	}

	// Row colors
	auto const text_standard = OPT_GET("Colour/Subtitle Grid/Standard")->GetColor();
	auto const text_selection = OPT_GET("Colour/Subtitle Grid/Selection")->GetColor();
	auto const text_collision = OPT_GET("Colour/Subtitle Grid/Collision")->GetColor();
	auto const grid_line_color = OPT_GET("Colour/Subtitle Grid/Lines")->GetColor();

	// First grid row
	if (paint_header)
		painter->DrawLine(0, 0, w, 0, grid_line_color);

	auto paint_text = [&](wxString const& str, int x, int y, int col) {
		int left = x + 4;
		if (columns[col]->Centered()) {
			int tw = 0, th = 0;
			painter->MeasureText(std::wstring(str.wx_str()), tw, th);
			left += (columns[col]->Width() - 6 - tw) / 2;
		}

		painter->DrawText(std::wstring(str.wx_str()), left, y + 2);
	};

	// Paint header
	if (paint_header) {
		painter->SetTextColor(text_standard);
		painter->FillRectangle(0, 0, w, lineHeight,
			from_wx(row_colors.Header.GetColour()));

		int x = fold_column_width;
		for (size_t i : agi::util::range(columns.size())) {
			if (paint_columns[i])
				paint_text(columns[i]->Header(), x, 0, i);
			x += columns[i]->Width();
		}

		painter->DrawLine(0, lineHeight, w, lineHeight, grid_line_color);
	}

	// Paint the rows
	const int grid_x = fold_column_width + columns[0]->Width();

	int const projected_lines = nDraw;
	auto projected_window = QueryGridWindow(
		yPos,
		projected_lines,
		has_dirty_rows
			? ProjectionColumnIdsForPaint(paint_columns)
			: std::vector<std::string>{aegisub::presentation::SubtitleGridColumnIdLineNumber});
	visible_rows.clear();
	selected_rows.clear();
	visible_rows.reserve(projected_window.rows.size());
	selected_rows.reserve(projected_window.rows.size());
	std::vector<aegisub::presentation::SubtitleGridRow const*> projected_rows_by_screen_row(
		static_cast<size_t>(projected_lines),
		nullptr);
	for (auto const& row : projected_window.rows) {
		if (row.state.visible_at_current_frame)
			visible_rows.push_back(row.row_index);
		if (row.state.selected)
			selected_rows.push_back(row.row_index);
		int const screen_row = DisplayRow(row.row_index) - yPos;
		if (screen_row >= 0 && screen_row < projected_lines)
			projected_rows_by_screen_row[static_cast<size_t>(screen_row)] = &row;
	}
	int active_screen_row = -1;

	for (int i = first_dirty_row; i <= last_dirty_row; ++i) {
		wxBrush color = row_colors.Default;
		auto const* projected_row = projected_rows_by_screen_row[static_cast<size_t>(i)];
		auto const row_state = projected_row
			? projected_row->state
			: aegisub::presentation::SubtitleGridRowState{};
		bool const is_comment = projected_row && projected_row->comment;
		if (row_state.active)
			active_screen_row = i;

		bool inSel = row_state.selected;
		if (inSel && is_comment)
			color = row_colors.SelectedComment;
		else if (inSel)
			color = row_colors.Selection;
		else if (is_comment)
			color = row_colors.Comment;

		if (row_state.visible_at_current_frame) {
			if (color == row_colors.Default)
				color = row_colors.Visible;
		}
		auto const row_bg = from_wx(color.GetColour());
		painter->SetRowBackground(row_bg);

		// Draw row background color
		if (color != row_colors.Default)
			painter->FillRectangle(grid_x, (i + 1) * lineHeight + 1, w, lineHeight, row_bg);

		if (row_state.collides_with_active)
			painter->SetTextColor(text_collision);
		else if (inSel)
			painter->SetTextColor(text_selection);
		else
			painter->SetTextColor(text_standard);

		// Draw text
		int x = fold_column_width;
		int y = (i + 1) * lineHeight;
		if (projected_row && fold_column_width) {
			auto const& folding = context->GetCore().ass->Folding();
			auto const *group = folding.GroupAt(projected_row->row_index);
			if (group && group->start == projected_row->row_index) {
				painter->SetTextColor(text_standard);
				painter->DrawText(folding.IsCollapsed(*group) ? std::wstring(L">") : std::wstring(L"v"), FromDIP(5), y + 2);
				painter->SetTextColor(row_state.collides_with_active ? text_collision : inSel ? text_selection
																							  : text_standard);
			}
		}
		for (size_t j : agi::util::range(columns.size())) {
			if (paint_columns[j] && projected_row)
				columns[j]->Paint(*painter, x, y, *projected_row, context);
			x += columns[j]->Width();
		}

		// Draw grid
		painter->DrawLine(0, y, w, y, grid_line_color);
		painter->DrawLine(0, y + lineHeight, w , y + lineHeight, grid_line_color);
	}

	// Draw grid columns
	if (paint_header || has_dirty_rows) {
		int minH = paint_header ? 0 : (first_dirty_row + 1) * lineHeight;
		int maxH = has_dirty_rows ? (last_dirty_row + 2) * lineHeight : lineHeight;
		int x = fold_column_width;
		painter->DrawLine(x, minH, x, maxH, grid_line_color);
		for (auto const& column : columns) {
			x += column->Width();
			if (x < w)
				painter->DrawLine(x, minH, x, maxH, grid_line_color);
		}
		painter->DrawLine(0, minH, 0, maxH, grid_line_color);
		painter->DrawLine(w, minH, w, maxH, grid_line_color);
	}

	if (active_screen_row >= first_dirty_row && active_screen_row <= last_dirty_row) {
		painter->StrokeRectangle(0, (active_screen_row + 1) * lineHeight, w, lineHeight + 1,
			OPT_GET("Colour/Subtitle Grid/Active Border")->GetColor());
	}
	};
	auto log_host_frame = [&](char const *backend) {
		if (!perf_trace::IsCategoryEnabled(perf_trace::Category::Log))
			return;
		LOG_I("subtitle/grid/host_frame")
			<< "backend=" << backend
			<< " total_ms=" << DurationMs(grid_paint_started);
	};

#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (CanUseSkiaRenderer()) {
		bool handled = false;
		{
			// The retained raster target and presentation bridge already provide
			// the frame buffer. Present directly so wx does not allocate and BitBlt
			// a second full-window buffer after the dirty-region blits.
			wxPaintDC dc(this);
			handled = renderer_slot->Paint(
				*this, dc, wxSize(w, h), GetUpdateRegion(), font, paint_grid,
				DurationMs(grid_paint_started));
		}
		if (handled) {
			log_host_frame("skia");
			return;
		}
	}
#endif
	{
		wxBufferedPaintDC dc(this);
		auto painter = MakeWxDcGridColumnPainter(dc);
		paint_grid(*painter);
	}
	log_host_frame("wx");
}

void BaseGrid::OnDPIChanged(wxDPIChangedEvent &event) {
	event.Skip();
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (renderer_slot)
		renderer_slot->InvalidateRasterPolicy();
#endif
	UpdateStyle();
	Refresh(false);
}

void BaseGrid::NotifySystemFontsChanged() {
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (!renderer_slot)
		return;
	renderer_slot->InvalidateSystemFonts();
	if (width_helper)
		width_helper->Reset();
	UpdateStyle();
	Refresh(false);
#endif
}

void BaseGrid::NotifyTextRasterPolicyChanged() {
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (!renderer_slot)
		return;
	renderer_slot->InvalidateRasterPolicy();
	Refresh(false);
#endif
}

void BaseGrid::OnSize(wxSizeEvent &) {
	auto const size = GetClientSize();
	auto const previous = last_client_size;
	last_client_size = size;
	AdjustScrollbar();
	if (previous == size)
		return;
#ifdef _WIN32
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (CanUseSkiaRenderer()) {
		Refresh(false);
		return;
	}
#endif
	if (previous.x > 0 && previous.y == size.y) {
		// Columns keep their positions on a width-only resize. Include the old
		// scrollbar and right border, whose pixels become grid content on growth.
		int const left = std::max(0, std::min(previous.x, size.x) - scrollBar->GetSize().x - 2);
		RefreshRect(wxRect(left, 0, size.x - left, size.y), false);
		return;
	}
#endif
	Refresh(false);
}

void BaseGrid::OnScroll(wxScrollEvent &event) {
	int newPos = event.GetPosition();
	if (yPos != newPos) {
		int old_y_pos = yPos;
		yPos = newPos;
		context->GetCore().ass->Properties.scroll_position = std::max(0, SourceRow(yPos));
		visible_rows = GetRowsDisplayedAtCurrentFrame();
		selected_rows = GetSelectedRowsInWindow();
		RefreshAfterScroll(old_y_pos);
	}
}

void BaseGrid::OnMouseEvent(wxMouseEvent &event) {
	auto const trace_left_click =
		perf_trace::IsCategoryEnabled(perf_trace::Category::Log)
		&& (event.LeftDown() || event.LeftDClick());
	auto const event_id = trace_left_click ? ++next_click_timing_id : 0;
	auto const click_started = trace_left_click
		? InputTimingClock::now()
		: InputTimingClock::time_point{};
	auto const message_age_ms = trace_left_click ? CurrentWindowsMessageAgeMs() : -1.0;
	auto const revision_at_entry = grid_revision;
	double focus_ms = 0.0;
	bool focus_requested = false;
	bool focus_changed = false;
	bool selection_handled = false;
	int trace_row = -1;
	auto log_click_timing = agi::make_scope_exit([&] {
		if (!trace_left_click)
			return;
		auto const handler_finished = InputTimingClock::now();
		auto const handler_ms = DurationMs(click_started, handler_finished);
		auto const slow_input =
			message_age_ms >= GridTimingLogThresholdMs ||
			handler_ms >= GridTimingLogThresholdMs ||
			focus_ms >= GridTimingLogThresholdMs;
		auto const revision_changed = grid_revision != revision_at_entry;
		// Keep the first click while its invalidation is waiting for paint; later
		// clicks can be coalesced into the same first repaint by wxWidgets.
		if (selection_handled && revision_changed && !pending_click_paint_timing.pending) {
			pending_click_paint_timing = {
				true,
				slow_input,
				event_id,
				trace_row,
				message_age_ms,
				handler_ms,
				focus_ms,
				click_started,
				handler_finished,
			};
		}
		if (!slow_input)
			return;
		LOG_I("subtitle/grid/input_timing")
			<< "event_id=" << event_id
			<< " phase=mouse_handler_complete"
			<< " row=" << trace_row
			<< " message_age_ms=" << message_age_ms
			<< " handler_ms=" << handler_ms
			<< " focus_requested=" << focus_requested
			<< " focus_changed=" << focus_changed
			<< " focus_ms=" << focus_ms
			<< " selection_handled=" << selection_handled
			<< " revision_changed=" << revision_changed;
	});

	if (hotkey::check("Subtitle Grid", context, event))
		return;

	int h = GetClientSize().GetHeight();
	bool shift = event.ShiftDown();
	bool alt = event.AltDown();
	bool ctrl = event.CmdDown();
	int const drag_modifiers = (shift ? 1 : 0) | (ctrl ? 2 : 0) | (alt ? 4 : 0);
	auto core = context->GetCore();
	auto ui = context->GetUI();

	// Row that mouse is over
	bool click = event.LeftDown();
	bool dclick = event.LeftDClick();
	int display_row = event.GetY() / lineHeight + yPos - 1;
	if (holding && !click)
		display_row = mid(0, display_row, GetDisplayRows() - 1);
	int row = event.GetY() < lineHeight && !holding ? -1 : SourceRow(display_row);
	trace_row = row;
	AssDialogue *dlg = GetDialogue(row);
	if (!dlg) row = 0;

	focus_requested = event.ButtonDown() && OPT_GET("Subtitle/Grid/Focus Allow")->GetBool();
	if (focus_requested) {
		focus_changed = wxWindow::FindFocus() != this;
		auto const focus_started = InputTimingClock::now();
		SetFocus();
		focus_ms = DurationMs(focus_started);
	}

	if ((click || dclick) && dlg && event.GetX() >= 0 && event.GetX() < FoldColumnWidth()) {
		auto& folding = core.ass->Folding();
		if (auto const *group = folding.GroupAt(row); group && group->start == row) {
			// A double click has already toggled on its first press.
			if (click) {
				auto const *active = core.selectionController->GetActiveLine();
				if (!folding.IsCollapsed(*group) && active && active->Row > group->start && active->Row <= group->end)
					core.selectionController->SetActiveLine(dlg);
				if (folding.Toggle(*core.ass, row))
					core.ass->Commit(from_wx(_("toggle subtitle group")), AssFile::COMMIT_FOLD);
			}
			return;
		}
	}

	if (holding) {
		if (!event.LeftIsDown()) {
			if (dlg)
				MakeRowVisible(row);
			holding = false;
			last_drag_row = -1;
			last_drag_modifiers = -1;
			ReleaseMouse();
			CommitDragSelectionPreview();
			return;
		}
		else {
			// Only scroll if the mouse has moved to a different row to avoid
			// scrolling on sloppy clicks
			if (row != extendRow) {
				if (display_row <= yPos)
					ScrollTo(yPos - 3);
				// When dragging down we give a 3 row margin to make it easier
				// to see what's going on, but we don't want to scroll down if
				// the user clicks on the bottom row and drags up
				else if (display_row > yPos + h / lineHeight - (row > extendRow ? 3 : 1))
					ScrollTo(yPos + 3);
			}
		}
	}
	else if (click && dlg) {
		holding = true;
		CaptureMouse();
	}

	if (holding && !click && !dclick
		&& row == last_drag_row
		&& drag_modifiers == last_drag_modifiers
		&& grid_revision == last_drag_revision) {
		// Mouse motion within the current row cannot change the drag selection.
		// Avoid rebuilding the selected-row vector and invalidating the same row
		// for every motion event delivered by wx.
		return;
	}

	if ((click || holding || dclick) && dlg) {
		int old_extend = extendRow;
		bool const drag_motion = holding && !click && !dclick;
		if (drag_motion) {
			// A modifier transition changes the selection base in the legacy
			// per-motion implementation. Materialize the current preview once so
			// re-pressing Ctrl continues from what is actually selected on screen.
			bool const ctrl_changed = last_drag_modifiers >= 0
				&& ((last_drag_modifiers ^ drag_modifiers) & 2) != 0;
			if (ctrl_changed && drag_selection_preview_active) {
				CommitDragSelectionPreview();
			}
			if (ctrl && !drag_selection_preview_active && drag_selection_base_rows.empty()) {
				auto const& current_selection = core.selectionController->GetSelectedSet();
				drag_selection_base_rows.reserve(current_selection.size());
				for (auto *line : current_selection)
					if (line)
						drag_selection_base_rows.push_back(line->Row);
				std::sort(drag_selection_base_rows.begin(), drag_selection_base_rows.end());
			}

			selection_handled = true;
			perf_trace::VideoUiDurationScope select_trace(
				"grid_select.mouse.total",
				std::abs(row - old_extend) + 1);
			ApplyDragSelectionPreview(row, old_extend, ctrl);
			last_drag_row = row;
			last_drag_modifiers = drag_modifiers;
			last_drag_revision = grid_revision;
			return;
		}

		std::vector<int> selected_rows;
		if (ctrl) {
			auto const& selection = core.selectionController->GetSelectedSet();
			selected_rows.reserve(selection.size());
			for (auto *line : selection)
				if (line)
					selected_rows.push_back(line->Row);
		}

		auto plan = aegisub::subtitle_grid_selection_policy::PlanMouseSelection({
			GetRows(),
			row,
			old_extend,
			std::move(selected_rows),
			click,
			dclick,
			holding,
			{shift, ctrl, alt},
		});
		if (plan.handled) {
			selection_handled = true;
			handling_mouse_selection = true;
			auto end_mouse_selection = agi::make_scope_exit([&] {
				handling_mouse_selection = false;
			});
			perf_trace::VideoUiDurationScope select_trace(
				"grid_select.mouse.total",
				static_cast<int>(plan.selected_rows.size()),
				plan.activate_media ? 1 : 0);
			if (plan.set_active) {
				// SetActiveLine will scroll the grid if the row is only half-visible,
				// but we don't want to scroll until the mouse moves or the button is
				// released, to avoid selecting multiple lines on a click
				int old_y_pos = yPos;
				core.selectionController->SetActiveLine(GetDialogue(plan.active_row));
				ScrollTo(old_y_pos);
				extendRow = plan.anchor_row;
			}

			if (plan.set_selection) {
				Selection newsel;
				for (int selected_row : plan.selected_rows)
					if (auto *line = GetDialogue(selected_row))
						newsel.insert(line);
				core.selectionController->SetSelectedSet(std::move(newsel));
			}

			if (plan.activate_media) {
				if (ui.audioBox)
					ui.audioBox->ScrollToActiveLine();
				core.videoController->JumpToTime(dlg->Start);
			}
			if (holding && click) {
				drag_selection_base_rows.clear();
				auto const& current_selection = core.selectionController->GetSelectedSet();
				drag_selection_base_rows.reserve(current_selection.size());
				for (auto *line : current_selection)
					if (line)
						drag_selection_base_rows.push_back(line->Row);
				std::sort(drag_selection_base_rows.begin(), drag_selection_base_rows.end());
			}
			if (holding) {
				last_drag_row = row;
				last_drag_modifiers = drag_modifiers;
				last_drag_revision = grid_revision;
			}
			return;
		}
	}

	// Mouse wheel
	if (event.GetWheelRotation() != 0) {
		if (ForwardMouseWheelEvent(this, event)) {
			int step = shift ? h / lineHeight - 2 : 3;
			ScrollTo(yPos - step * event.GetWheelRotation() / event.GetWheelDelta());
		}
		return;
	}

	event.Skip();
}

void BaseGrid::OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
	holding = false;
	last_drag_row = -1;
	last_drag_modifiers = -1;
	CommitDragSelectionPreview();
}

void BaseGrid::ApplyDragSelectionPreview(int active_row, int anchor_row, bool add_base) {
	perf_trace::VideoUiDurationScope trace(
		"grid_select.mouse.preview",
		std::abs(active_row - anchor_row) + 1,
		add_base ? static_cast<int>(drag_selection_base_rows.size()) : 0);
	int preview_first_row = std::min(active_row, anchor_row);
	int preview_last_row = std::max(active_row, anchor_row);
	// Ctrl drag is additive in the legacy grid: folding back keeps every row
	// crossed while Ctrl was held. All ranges share the anchor, so their union
	// remains one interval and can be accumulated without per-row work.
	if (add_base && drag_selection_preview_active && drag_selection_preview_add_base) {
		preview_first_row = std::min(preview_first_row, drag_selection_preview_first_row);
		preview_last_row = std::max(preview_last_row, drag_selection_preview_last_row);
	}
	int const old_active_row = drag_selection_preview_active
		? drag_selection_preview_active_row
		: this->active_row;

	drag_selection_preview_active = true;
	drag_selection_preview_active_row = active_row;
	drag_selection_preview_first_row = preview_first_row;
	drag_selection_preview_last_row = preview_last_row;
	drag_selection_preview_add_base = add_base;
	++grid_revision;

	auto new_selected_rows = GetSelectedRowsInWindow();
	RefreshChangedVisibleRows(selected_rows, new_selected_rows);
	selected_rows = std::move(new_selected_rows);
	if (old_active_row != active_row) {
		RefreshSubtitleGridRow(old_active_row);
		RefreshSubtitleGridRow(active_row);
	}
}

void BaseGrid::CommitDragSelectionPreview() {
	if (!drag_selection_preview_active)
		return;

	auto const active_row = drag_selection_preview_active_row;
	bool const add_base = drag_selection_preview_add_base;
	int const anchor_row = extendRow;
	int const first_row = drag_selection_preview_first_row;
	int const last_row = drag_selection_preview_last_row;
	auto base_rows = add_base ? std::move(drag_selection_base_rows) : std::vector<int>{};
	ClearDragSelectionPreview();
	committing_drag_selection_preview = true;
	auto end_commit = agi::make_scope_exit([&] { committing_drag_selection_preview = false; });

	perf_trace::VideoUiDurationScope trace(
		"grid_select.mouse.commit",
		std::abs(active_row - anchor_row) + 1,
		static_cast<int>(base_rows.size()));
	auto plan = aegisub::subtitle_grid_selection_policy::PlanMouseSelection({
		GetRows(),
		first_row,
		last_row,
		std::move(base_rows),
		false,
		false,
		true,
		{false, add_base, false},
	});

	auto core = context->GetCore();
	core.selectionController->SetActiveLine(GetDialogue(active_row));
	extendRow = anchor_row;

	Selection new_selection;
	for (int selected_row : plan.selected_rows)
		if (auto *line = GetDialogue(selected_row))
			new_selection.insert(line);
	core.selectionController->SetSelectedSet(std::move(new_selection));
}

void BaseGrid::ClearDragSelectionPreview(bool cancel_drag) {
	bool const release_capture = cancel_drag && holding && HasCapture();
	if (cancel_drag) {
		holding = false;
		last_drag_row = -1;
		last_drag_modifiers = -1;
	}
	drag_selection_preview_active = false;
	drag_selection_preview_active_row = -1;
	drag_selection_preview_first_row = -1;
	drag_selection_preview_last_row = -1;
	drag_selection_preview_add_base = false;
	drag_selection_base_rows.clear();
	if (release_capture)
		ReleaseMouse();
}

void BaseGrid::OnContextMenu(wxContextMenuEvent &evt) {
	wxPoint pos = evt.GetPosition();
	if (pos == wxDefaultPosition || ScreenToClient(pos).y > lineHeight) {
		if (!context_menu) context_menu = menu::GetMenu("grid_context", (wxID_HIGHEST + 1) + 8000, context);
		menu::OpenPopupMenu(context_menu.get(), this);
	}
	else {
		wxMenu menu;
		auto *fold_column = menu.AppendCheckItem(wxID_ANY, _("Folding column"),
												 _("Show folding controls when the subtitle file contains fold groups"));
		fold_column->Check(OPT_GET("Subtitle/Grid/Show Folding Column")->GetBool());
		menu.Bind(wxEVT_MENU, [](wxCommandEvent& event) { OPT_SET("Subtitle/Grid/Show Folding Column")->SetBool(event.IsChecked()); }, fold_column->GetId());
		for (size_t i : agi::util::range(columns.size())) {
			if (columns[i]->CanHide())
				menu.Append(MENU_SHOW_COL + i, columns[i]->Description(), wxEmptyString, wxITEM_CHECK)->Check(columns[i]->Visible());
		}
		PopupMenu(&menu);
	}
}

void BaseGrid::RestoreScrollPosition(int source_row) {
	auto const& folding = context->GetCore().ass->Folding();
	int display = folding.SourceToDisplay(source_row);
	if (display < 0) {
		if (auto const *group = folding.GroupAt(source_row))
			display = folding.SourceToDisplay(group->start);
		else
			display = source_row > 0 ? GetDisplayRows() - 1 : 0;
	}
	ScrollTo(display);
}

void BaseGrid::ScrollTo(int y) {
	int nextY = mid(0, y, std::max(0, GetDisplayRows() - 1));
	if (yPos != nextY) {
		int old_y_pos = yPos;
		yPos = nextY;
		context->GetCore().ass->Properties.scroll_position = std::max(0, SourceRow(yPos));
		scrollBar->SetThumbPosition(yPos);
		visible_rows = GetRowsDisplayedAtCurrentFrame();
		selected_rows = GetSelectedRowsInWindow();
		RefreshAfterScroll(old_y_pos);
	}
}

std::vector<int> BaseGrid::GetRowsDisplayedAtCurrentFrame() const {
	std::vector<int> rows;
	if (!OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame")->GetBool())
		return rows;

	auto core = context->GetCore();
	if (!core.project->VideoProvider() || current_frame < 0)
		return rows;

	int lines = GetClientSize().GetHeight() / lineHeight + 1;
	lines = mid(0, lines, GetDisplayRows() - yPos);
	rows.reserve(lines);

	auto window = QueryGridWindow(
		yPos,
		lines,
		std::vector<std::string>{aegisub::presentation::SubtitleGridColumnIdLineNumber});
	for (auto const& row : window.rows)
		if (row.state.visible_at_current_frame)
			rows.push_back(row.row_index);

	return rows;
}

std::vector<int> BaseGrid::GetSelectedRowsInWindow() const {
	std::vector<int> rows;

	int lines = GetClientSize().GetHeight() / lineHeight + 1;
	lines = mid(0, lines, GetDisplayRows() - yPos);
	rows.reserve(lines);

	// Selection repaint only needs row indices. Avoid building projection rows and
	// column values on every mouse motion while a drag preview is active.
	auto const& selection = context->GetCore().selectionController->GetSelectedSet();
	for (int display_row = yPos; display_row < yPos + lines; ++display_row) {
		int const row = SourceRow(display_row);
		auto const *line = context->GetCore().ass->Folding().DisplayRows()[display_row].dialogue;
		if (!line)
			continue;

		bool selected = false;
		if (drag_selection_preview_active) {
			selected = row >= drag_selection_preview_first_row
				&& row <= drag_selection_preview_last_row;
			if (!selected && drag_selection_preview_add_base)
				selected = std::binary_search(
					drag_selection_base_rows.begin(), drag_selection_base_rows.end(), row);
		}
		else {
			selected = selection.count(const_cast<AssDialogue *>(line)) != 0;
		}

		if (selected)
			rows.push_back(row);
	}

	return rows;
}

wxRect BaseGrid::GetScrollableRect() const {
	int width = 0;
	int height = 0;
	GetClientSize(&width, &height);
	width -= scrollBar->GetSize().GetWidth();
	int top = lineHeight + 1;

	return wxRect(0, top, std::max(0, width), std::max(0, height - top));
}

void BaseGrid::RefreshChangedVisibleRows(std::vector<int> const& old_visible_rows, std::vector<int> const& new_visible_rows) {
	auto old_it = begin(old_visible_rows);
	auto new_it = begin(new_visible_rows);

	while (old_it != end(old_visible_rows) || new_it != end(new_visible_rows)) {
		int row = -1;
		if (old_it == end(old_visible_rows))
			row = *new_it++;
		else if (new_it == end(new_visible_rows))
			row = *old_it++;
		else if (*old_it < *new_it)
			row = *old_it++;
		else if (*new_it < *old_it)
			row = *new_it++;
		else {
			++old_it;
			++new_it;
			continue;
		}

		RefreshSubtitleGridRow(row);
	}
}

void BaseGrid::QueueChangedVisibleRowsRefresh(std::vector<int> const& old_visible_rows, std::vector<int> const& new_visible_rows) {
	auto old_it = begin(old_visible_rows);
	auto new_it = begin(new_visible_rows);

	while (old_it != end(old_visible_rows) || new_it != end(new_visible_rows)) {
		int row = -1;
		if (old_it == end(old_visible_rows))
			row = *new_it++;
		else if (new_it == end(new_visible_rows))
			row = *old_it++;
		else if (*old_it < *new_it)
			row = *old_it++;
		else if (*new_it < *old_it)
			row = *new_it++;
		else {
			++old_it;
			++new_it;
			continue;
		}

		QueueSubtitleGridRowRefresh(row);
	}
}

void BaseGrid::QueueSubtitleGridRowRefresh(int row_index) {
	if (full_refresh_on_idle)
		return;

	refresh_on_idle = true;
	subtitle_rows_refresh_on_idle.push_back(row_index);
}

void BaseGrid::QueueVisibleWindowRefresh() {
	int lines = GetClientSize().GetHeight() / lineHeight + 1;
	lines = mid(0, lines, GetDisplayRows() - yPos);
	for (int i = 0; i < lines; ++i)
		QueueSubtitleGridRowRefresh(SourceRow(yPos + i));
}

void BaseGrid::FlushQueuedSubtitleGridRowRefreshes() {
	std::sort(begin(subtitle_rows_refresh_on_idle), end(subtitle_rows_refresh_on_idle));
	subtitle_rows_refresh_on_idle.erase(
		std::unique(begin(subtitle_rows_refresh_on_idle), end(subtitle_rows_refresh_on_idle)),
		end(subtitle_rows_refresh_on_idle));

	for (int row : subtitle_rows_refresh_on_idle)
		RefreshSubtitleGridRow(row);

	subtitle_rows_refresh_on_idle.clear();
}

void BaseGrid::RefreshAfterScroll(int old_y_pos) {
	wxRect rect = GetScrollableRect();
	int delta_rows = yPos - old_y_pos;
	int delta_pixels = -delta_rows * lineHeight;

	if (rect.GetWidth() > 0 && rect.GetHeight() > 0 && delta_pixels > -rect.GetHeight() && delta_pixels < rect.GetHeight())
		ScrollWindow(0, delta_pixels, &rect);
	else
		Refresh(false);
}

void BaseGrid::AdjustScrollbar() {
	perf_trace::VideoUiDurationScope trace("subtitle_grid.adjust_scrollbar");
	wxSize clientSize = GetClientSize();
	wxSize scrollbarSize = scrollBar->GetSize();

	wxRect const bounds(clientSize.GetWidth() - scrollbarSize.GetWidth(), 0, scrollbarSize.GetWidth(), clientSize.GetHeight());
	if (scrollBar->GetRect() != bounds)
		scrollBar->SetSize(bounds);

	if (GetDisplayRows() <= 1) {
		yPos = 0;
		context->GetCore().ass->Properties.scroll_position = 0;
		scrollBar->SetScrollbar(0, 1, 1, 1, true);
		if (scrollBar->IsEnabled())
			scrollBar->Enable(false);
		return;
	}

	if (!scrollBar->IsEnabled())
		scrollBar->Enable(true);

	int drawPerScreen = clientSize.GetHeight() / lineHeight;
	int rows = GetDisplayRows();

	yPos = mid(0, yPos, rows - 1);
	context->GetCore().ass->Properties.scroll_position = std::max(0, SourceRow(yPos));

	int const range = rows + drawPerScreen - 1;
	int const page = drawPerScreen - 2;
	if (scrollBar->GetThumbPosition() != yPos || scrollBar->GetThumbSize() != drawPerScreen ||
		scrollBar->GetRange() != range || scrollBar->GetPageSize() != page)
		scrollBar->SetScrollbar(yPos, drawPerScreen, range, page, true);
}

void BaseGrid::RefreshSubtitleGridRow(int row_index) {
	int const visible_row = DisplayRow(row_index) - yPos;
	if (visible_row < 0)
		return;

	int width = 0;
	int height = 0;
	GetClientSize(&width, &height);

	int const top = (visible_row + 1) * lineHeight;
	if (top >= height)
		return;

	// Repaint the whole row so selection/background/border stay in sync with the edited text.
	RefreshRect(wxRect(0, top, width, lineHeight + 1), false);
}

void BaseGrid::SetColumnWidths() {
	int w, h;
	GetClientSize(&w, &h);

	std::unique_ptr<GridColumnPainter> painter;
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (CanUseSkiaRenderer())
		painter = renderer_slot->CreateMeasurementPainter(font);
#endif
	std::unique_ptr<wxClientDC> dc;
	if (!painter) {
		dc = std::make_unique<wxClientDC>(this);
		painter = MakeWxDcGridColumnPainter(*dc);
		painter->SetFont(font);
	}

	text_refresh_rects.clear();
	measured_fold_column_width = FoldColumnWidth();
	int x = measured_fold_column_width;

	if (!width_helper)
		width_helper = agi::make_unique<WidthHelper>();
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	if (renderer_slot && width_renderer_generation != renderer_slot->Generation()) {
		width_helper->Reset();
		width_renderer_generation = renderer_slot->Generation();
	}
#endif
	width_helper->SetPainter(painter.get());

	aegisub::presentation::VisibleSubtitleRowsRequest request;
	request.first_row = 0;
	request.row_count = GetRows();
	request.column_ids = ProjectionColumnIdsForWidths();
	auto width_window = aegisub::presentation::BuildSubtitleGridWindow(
		projection_line_map,
		request,
		grid_revision);

	for (auto const& column : columns) {
		column->UpdateWidth(context, *width_helper, width_window);
		if (column->Width() && column->RefreshOnTextChange())
			text_refresh_rects.emplace_back(x, 0, column->Width(), h);
		x += column->Width();
	}
	width_helper->Age();
}

AssDialogue *BaseGrid::GetDialogue(int n) const {
	if (static_cast<size_t>(n) >= index_line_map.size()) return nullptr;
	return index_line_map[n];
}

aegisub::presentation::SubtitleGridWindow BaseGrid::QueryGridWindow(int first_row, int row_count, std::vector<std::string> column_ids) const {
	aegisub::presentation::VisibleSubtitleRowsRequest request;
	request.first_row = first_row;
	request.row_count = row_count;
	request.column_ids = std::move(column_ids);
	return aegisub::presentation::BuildFoldedSubtitleGridWindow(
		context->GetCore().ass->Folding().DisplayRows(),
		request,
		grid_revision,
		[&](AssDialogue const& line) {
			return ResolveGridRowState(line);
		});
}

std::vector<std::string> BaseGrid::ProjectionColumnIdsForPaint(std::vector<char> const& paint_columns) const {
	std::vector<std::string> column_ids;
	column_ids.reserve(columns.size());

	for (size_t i : agi::util::range(columns.size())) {
		if (i >= paint_columns.size() || !paint_columns[i] || columns[i]->Width() <= 0)
			continue;

		auto const* column_id = columns[i]->ProjectionColumnId();
		if (std::find(column_ids.begin(), column_ids.end(), column_id) == column_ids.end())
			column_ids.emplace_back(column_id);
	}

	if (column_ids.empty())
		column_ids.emplace_back(aegisub::presentation::SubtitleGridColumnIdLineNumber);

	return column_ids;
}

std::vector<std::string> BaseGrid::ProjectionColumnIdsForWidths() const {
	std::vector<std::string> column_ids;
	column_ids.reserve(columns.size());

	for (auto const& column : columns) {
		if (!column->Visible())
			continue;

		auto const* column_id = column->ProjectionWidthColumnId();
		if (!column_id)
			continue;

		if (std::find(column_ids.begin(), column_ids.end(), column_id) == column_ids.end())
			column_ids.emplace_back(column_id);
	}

	if (column_ids.empty())
		column_ids.emplace_back(aegisub::presentation::SubtitleGridColumnIdLineNumber);

	return column_ids;
}

aegisub::presentation::SubtitleGridRowState BaseGrid::ResolveGridRowState(AssDialogue const& line) const {
	auto core = context->GetCore();
	auto const& selection = core.selectionController->GetSelectedSet();

	aegisub::presentation::SubtitleGridRowState state;
	auto const* committed_active_line = core.selectionController->GetActiveLine();
	auto const* displayed_active_line = drag_selection_preview_active
		? GetDialogue(drag_selection_preview_active_row)
		: committed_active_line;
	if (drag_selection_preview_active) {
		state.selected =
			(line.Row >= drag_selection_preview_first_row && line.Row <= drag_selection_preview_last_row)
			|| (drag_selection_preview_add_base
				&& std::binary_search(drag_selection_base_rows.begin(), drag_selection_base_rows.end(), line.Row));
	}
	else {
		state.selected = selection.count(const_cast<AssDialogue*>(&line)) != 0;
	}
	state.active = displayed_active_line == &line;
	state.visible_at_current_frame =
		OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame")->GetBool() &&
		IsDisplayed(&line);
	state.collides_with_active = committed_active_line
		&& committed_active_line != &line
		&& line.CollidesWith(committed_active_line);
	return state;
}

bool BaseGrid::IsDisplayed(const AssDialogue *line) const {
	auto core = context->GetCore();
	if (!core.project->VideoProvider() || current_frame < 0)
		return false;
	return core.project->Timecodes().FrameAtTime(line->Start, agi::vfr::START) <= current_frame
		&& core.project->Timecodes().FrameAtTime(line->End, agi::vfr::END) >= current_frame;
}

void BaseGrid::OnCharHook(wxKeyEvent &event) {
	int const key = event.GetKeyCode();
	bool const modifier_only = key == WXK_SHIFT || key == WXK_CONTROL || key == WXK_ALT;
	if (!holding || !modifier_only)
		CommitDragSelectionPreview();
	if (hotkey::check("Subtitle Grid", context, event))
		return;

	if (key == WXK_UP || key == WXK_DOWN ||
		key == WXK_PAGEUP || key == WXK_PAGEDOWN ||
		key == WXK_HOME || key == WXK_END)
	{
		event.Skip();
		return;
	}

	hotkey::check("Audio", context, event);
}

void BaseGrid::OnKeyDown(wxKeyEvent &event) {
	int const key = event.GetKeyCode();
	bool const modifier_only = key == WXK_SHIFT || key == WXK_CONTROL || key == WXK_ALT;
	if (!holding || !modifier_only)
		CommitDragSelectionPreview();
	int w,h;
	GetClientSize(&w, &h);

	bool ctrl = event.CmdDown();
	bool alt = event.AltDown();
	bool shift = event.ShiftDown();

	int dir = 0;
	int step = 1;
	if (key == WXK_UP) dir = -1;
	else if (key == WXK_DOWN) dir = 1;
	else if (key == WXK_PAGEUP) {
		dir = -1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_PAGEDOWN) {
		dir = 1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_HOME) {
		dir = -1;
		step = GetDisplayRows();
	}
	else if (key == WXK_END) {
		dir = 1;
		step = GetDisplayRows();
	}

	if (!dir) {
		event.Skip();
		return;
	}

	auto core = context->GetCore();
	auto active_line = core.selectionController->GetActiveLine();
	int old_extend = extendRow;
	auto const& selection = core.selectionController->GetSelectedSet();
	std::vector<int> selected_rows;
	selected_rows.reserve(selection.size());
	for (auto *line : selection)
		if (line)
			selected_rows.push_back(line->Row);

	int const active_display = active_line ? DisplayRow(active_line->Row) : 0;
	int const target_display = mid(0, std::max(0, active_display) + dir * std::max(1, step), std::max(0, GetDisplayRows() - 1));
	auto plan = aegisub::subtitle_grid_selection_policy::PlanKeyboardSelection({
		.row_count = GetRows(),
		.active_row = active_line ? active_line->Row : -1,
		.anchor_row = old_extend,
		.selected_rows = std::move(selected_rows),
		.direction = dir,
		.step = step,
		.modifiers = {.shift = shift, .ctrl = ctrl, .alt = alt},
		.target_row = SourceRow(target_display),
	});
	if (!plan.handled) {
		event.Skip();
		return;
	}

	perf_trace::VideoUiDurationScope select_trace(
		"grid_select.key.total",
		static_cast<int>(plan.selected_rows.size()));

	if (plan.set_active) {
		core.selectionController->SetActiveLine(GetDialogue(plan.active_row));
		extendRow = plan.anchor_row;
	}

	if (plan.set_selection) {
		Selection newsel;
		for (int selected_row : plan.selected_rows)
			if (auto *line = GetDialogue(selected_row))
				newsel.insert(line);
		core.selectionController->SetSelectedSet(std::move(newsel));
	}

	if (plan.make_active_visible)
		MakeRowVisible(plan.active_row);
}

void BaseGrid::NextVisibleLine(int direction) {
	auto core = context->GetCore();
	auto const *active = core.selectionController->GetActiveLine();
	if (!active || !GetDisplayRows())
		return;
	int const row = DisplayRow(active->Row);
	int const next = mid(0, std::max(0, row) + direction, GetDisplayRows() - 1);
	if (auto *line = GetDialogue(SourceRow(next)))
		core.selectionController->SetSelectionAndActive({line}, line);
}

void BaseGrid::SetByFrame(bool state) {
	SetDisplayMode(state ? SubtitleTimeDisplayMode::Frame : SubtitleTimeDisplayMode::Ass);
}

void BaseGrid::SetDisplayMode(SubtitleTimeDisplayMode mode) {
	if (display_mode == mode)
		return;

	if (mode == SubtitleTimeDisplayMode::Frame && !context->GetCore().project->Timecodes().IsLoaded())
		mode = SubtitleTimeDisplayMode::Ass;

	display_mode = mode;
	for (auto& column : columns)
		column->SetDisplayMode(display_mode);
	SetColumnWidths();
	Refresh(false);
}
