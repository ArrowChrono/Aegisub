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

#include <libaegisub/signal.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <wx/brush.h>
#include <wx/window.h>

#include "presentation/presentation_contract.h"
#include "time_display_mode.h"

namespace agi {
	struct Context;
	class OptionValue;
}
class AssDialogue;
class GridColumn;
class WidthHelper;
class wxScrollBar;
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
namespace aegisub::grid { class SubtitleGridRendererSlot; }
#endif

class BaseGrid final : public wxWindow {
	using InputTimingClock = std::chrono::steady_clock;

	struct PendingClickPaintTiming {
		bool pending = false;
		bool slow_input = false;
		std::uint64_t event_id = 0;
		int row = -1;
		double message_age_ms = -1.0;
		double handler_ms = 0.0;
		double focus_ms = 0.0;
		InputTimingClock::time_point click_started;
		InputTimingClock::time_point handler_finished;
	};

	std::vector<agi::signal::Connection> connections;
	int lineHeight = 1;     ///< Height of a line in pixels in the current font
	bool holding = false;   ///< Is a drag selection in process?
	/// Last handled drag-selection input, used to coalesce duplicate mouse motion.
	int last_drag_row = -1;
	int last_drag_modifiers = -1;
	aegisub::presentation::Revision last_drag_revision = 0;
	bool drag_selection_preview_active = false;
	int drag_selection_preview_active_row = -1;
	int drag_selection_preview_first_row = -1;
	int drag_selection_preview_last_row = -1;
	bool drag_selection_preview_add_base = false;
	bool committing_drag_selection_preview = false;
	bool handling_mouse_selection = false;
	std::vector<int> drag_selection_base_rows;
	wxFont font;            ///< Current grid font
	wxScrollBar *scrollBar; ///< The grid's scrollbar
	wxSize last_client_size = wxDefaultSize;
	SubtitleTimeDisplayMode display_mode = SubtitleTimeDisplayMode::Ass;

	/// Row from which the selection shrinks/grows from when selecting via the
	/// keyboard, shift-clicking or dragging
	int extendRow = -1;

	/// First row that is visible at the current scroll position
	int yPos = 0;

	int active_row = -1;
	bool reveal_active_after_commit = false;
	int current_frame = -1;

	std::unique_ptr<WidthHelper> width_helper;
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	std::unique_ptr<aegisub::grid::SubtitleGridRendererSlot> renderer_slot;
	std::uint64_t width_renderer_generation = 0;
	bool CanUseSkiaRenderer() const noexcept;
#endif

	/// Rows which are visible on the current video frame
	std::vector<int> visible_rows;
	std::vector<int> selected_rows;
	aegisub::presentation::Revision grid_revision = 0;
	std::uint64_t next_click_timing_id = 0;
	PendingClickPaintTiming pending_click_paint_timing;

	agi::Context *context; ///< Associated project context

	std::vector<std::unique_ptr<GridColumn>> columns;
	std::vector<bool> columns_visible;

	std::vector<wxRect> text_refresh_rects;
	int measured_fold_column_width = 0;

	bool refresh_on_idle = false;
	bool full_refresh_on_idle = false;
	std::vector<int> subtitle_rows_refresh_on_idle;

	/// Cached brushes used for row backgrounds
	struct {
		wxBrush Default;
		wxBrush Header;
		wxBrush Selection;
		wxBrush Comment;
		wxBrush Visible;
		wxBrush SelectedComment;
		wxBrush LeftCol;
	} row_colors;

	std::vector<AssDialogue*> index_line_map;  ///< Row number -> dialogue line
	std::vector<AssDialogue const*> projection_line_map; ///< Row number -> dialogue line for presentation projection
	std::vector<int> display_line_ids;                   ///< Stable IDs in the previous display map, for scroll anchoring

	/// Cached grid body context menu
	std::unique_ptr<wxMenu> context_menu;

	void OnContextMenu(wxContextMenuEvent &evt);
	void OnDPIChanged(wxDPIChangedEvent &evt);
	void OnHighlightVisibleChange(agi::OptionValue const& opt);
	void OnIdle(wxIdleEvent&);
	void OnKeyDown(wxKeyEvent &event);
	void OnCharHook(wxKeyEvent &event);
	void OnMouseEvent(wxMouseEvent &event);
	void OnMouseCaptureLost(wxMouseCaptureLostEvent &event);
	void OnPaint(wxPaintEvent &event);
	void OnScroll(wxScrollEvent &event);
	void OnShowColMenu(wxCommandEvent &event);
	void OnSize(wxSizeEvent &event);
	void OnSubtitlesCommit(int type, const AssDialogue *single_line);
	void OnActiveLineChanged(AssDialogue *);
	void OnCurrentFrameChanged(int frame_number);
	void OnVideoProviderChanged();

	void AdjustScrollbar();
	void ApplyDragSelectionPreview(int active_row, int anchor_row, bool add_base);
	void CommitDragSelectionPreview();
	void ClearDragSelectionPreview(bool cancel_drag = false);
	std::vector<int> GetRowsDisplayedAtCurrentFrame() const;
	std::vector<int> GetSelectedRowsInWindow() const;
	wxRect GetScrollableRect() const;
	void RefreshChangedVisibleRows(std::vector<int> const& old_visible_rows, std::vector<int> const& new_visible_rows);
	void QueueChangedVisibleRowsRefresh(std::vector<int> const& old_visible_rows, std::vector<int> const& new_visible_rows);
	void QueueSubtitleGridRowRefresh(int row_index);
	void QueueVisibleWindowRefresh();
	void FlushQueuedSubtitleGridRowRefreshes();
	void RefreshAfterScroll(int old_y_pos);
	void RefreshSubtitleGridRow(int row_index);
	void SetColumnWidths();

	aegisub::presentation::SubtitleGridWindow QueryGridWindow(int first_row, int row_count, std::vector<std::string> column_ids = {}) const;
	std::vector<std::string> ProjectionColumnIdsForPaint(std::vector<char> const& paint_columns) const;
	std::vector<std::string> ProjectionColumnIdsForWidths() const;
	aegisub::presentation::SubtitleGridRowState ResolveGridRowState(AssDialogue const& line) const;
	bool IsDisplayed(const AssDialogue *line) const;

	/// Rebuild the row maps after a structural change.
	/// @param remeasure_columns Recompute column widths. Every column derives its
	/// width from an order-independent aggregate (a max over all rows, the total
	/// row count, or a fixed string), so a commit that only reorders existing
	/// lines can skip the full-file remeasure.
	void UpdateMaps(bool remeasure_columns = true, bool preserve_anchor = true);
	void UpdateDisplayMap(bool preserve_anchor = true);
	void UpdateStyle();

	int GetRows() const { return index_line_map.size(); }
	int GetDisplayRows() const;
	int SourceRow(int display_row) const;
	int DisplayRow(int source_row) const;
	int FoldColumnWidth() const;
	void MakeRowVisible(int row);

	/// @brief Get dialogue by index
	/// @param n Index to look up
	/// @return Subtitle dialogue line for index, or 0 if invalid index
	AssDialogue *GetDialogue(int n) const;

public:
	BaseGrid(wxWindow* parent, agi::Context *context);
	~BaseGrid();

	void SetDisplayMode(SubtitleTimeDisplayMode mode);
	void SetByFrame(bool state);
	void ScrollTo(int y);
	void RestoreScrollPosition(int source_row);
	void NextVisibleLine(int direction);
	void NotifySystemFontsChanged();
	void NotifyTextRasterPolicyChanged();

	DECLARE_EVENT_TABLE()
};
