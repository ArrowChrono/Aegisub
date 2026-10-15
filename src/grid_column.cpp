// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "grid_column.h"

#include "compat.h"
#include "grid_column_painter.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "presentation/subtitle_grid_display.h"
#include "project.h"
#include "time_display_mode.h"
#include "video_controller.h"

#include <wx/strconv.h>
#include <wx/string.h>

void WidthHelper::Age() {
	for (auto it = begin(widths), e = end(widths); it != e; ) {
		if (it->second.age == age)
			++it;
		else
			it = widths.erase(it);
	}
	++age;
}

void WidthHelper::Reset() {
	widths.clear();
	age = 0;
}

int WidthHelper::operator()(boost::flyweight<std::string> const& str) {
	if (str.get().empty()) return 0;
	auto it = widths.find(str);
	if (it != end(widths)) {
		it->second.age = age;
		return it->second.width;
	}

	int width = 0;
	int height = 0;
#ifdef _WIN32
	// Reuse the UTF-16 scratch buffer so we don't allocate a fresh wxString
	// per measurement on the hot flyweight path. wxMBConvUTF8 matches the
	// historical behavior of the wxDC path exactly.
	wxMBConvUTF8 conv;
	size_t len = conv.ToWChar(nullptr, 0, str.get().c_str(), str.get().size());
	scratch.assign(static_cast<size_t>(std::max<size_t>(len, 1)), L'\0');
	conv.ToWChar(scratch.data(), len, str.get().c_str(), str.get().size());
	painter->MeasureText(scratch, width, height);
#else
	painter->MeasureText(str, width, height);
#endif

	widths[str] = {width, age};
	return width;
}

int WidthHelper::operator()(std::string const& str) {
	return (*this)(boost::flyweight<std::string>(str));
}

int WidthHelper::operator()(wxString const& str) {
	int width = 0, height = 0;
	painter->MeasureText(std::wstring(str.wx_str()), width, height);
	return width;
}

int WidthHelper::operator()(const char *str) {
	int width = 0, height = 0;
	painter->MeasureText(std::string(str), width, height);
	return width;
}

int WidthHelper::operator()(const wchar_t *str) {
	int width = 0, height = 0;
	painter->MeasureText(std::wstring(str), width, height);
	return width;
}

void GridColumn::UpdateWidth(const agi::Context *c, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) {
	if (!visible) {
		width = 0;
		return;
	}

	width = WidthFromRows(c, helper, rows);
	if (width)
		width = 10 + std::max(width, helper(Header()));
}

void GridColumn::Paint(GridColumnPainter &painter, int x, int y, aegisub::presentation::SubtitleGridRow const& row, const agi::Context *c) const {
	wxString str = Value(row, c);
	if (Centered()) {
		int w = 0, h = 0;
		painter.MeasureText(std::wstring(str.wx_str()), w, h);
		x += (width - 6 - w) / 2;
	}
	painter.DrawText(std::wstring(str.wx_str()), x + 4, y + 2);
}

namespace {
#define COLUMN_HEADER(value) \
	private: const wxString header = value; \
	public: wxString const& Header() const override { return header; }
#define COLUMN_DESCRIPTION(value) \
	private: const wxString description = value; \
	public: wxString const& Description() const override { return description; }
#define COLUMN_PROJECTION_ID(value) \
	public: char const *ProjectionColumnId() const override { return value; }

int max_value(int aegisub::presentation::SubtitleGridRow::*field, aegisub::presentation::SubtitleGridWindow const& rows) {
	int value = 0;
	for (auto const& row : rows.rows) {
		if (row.*field > value)
			value = row.*field;
	}
	return value;
}

int max_width(std::string aegisub::presentation::SubtitleGridRow::*field, aegisub::presentation::SubtitleGridWindow const& rows, WidthHelper &helper) {
	int w = 0;
	for (auto const& row : rows.rows) {
		auto const& v = row.*field;
		if (v.empty()) continue;
		int width = helper(v);
		if (width > w)
			w = width;
	}
	return w;
}

struct GridColumnLineNumber final : GridColumn {
	COLUMN_HEADER(_("#"))
	COLUMN_DESCRIPTION(_("Line Number"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdLineNumber)
	bool Centered() const override { return true; }

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context * = nullptr) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId()));
	}

	int WidthFromRows(const agi::Context *, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		return rows.total_rows <= 0 ? 0 : helper(std::to_wstring(rows.total_rows));
	}
};

struct GridColumnLayer final : GridColumn {
	COLUMN_HEADER(_("L"))
	COLUMN_DESCRIPTION(_("Layer"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdLayer)
	bool Centered() const override { return true; }

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId()));
	}

	int WidthFromRows(const agi::Context *, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		int max_layer = max_value(&aegisub::presentation::SubtitleGridRow::layer, rows);
		return max_layer == 0 ? 0 : helper(std::to_wstring(max_layer));
	}
};

struct GridColumnTime : GridColumn {
	SubtitleTimeDisplayMode display_mode = SubtitleTimeDisplayMode::Ass;

	bool Centered() const override { return true; }
	void SetDisplayMode(SubtitleTimeDisplayMode mode) override { display_mode = mode; }

	aegisub::presentation::SubtitleGridDisplayOptions DisplayOptions(const agi::Context *c) const {
		aegisub::presentation::SubtitleGridDisplayOptions options;
		options.time_display_mode = display_mode;
		options.timecodes = &c->GetCore().project->Timecodes();
		return options;
	}
};

struct GridColumnStartTime final : GridColumnTime {
	COLUMN_HEADER(_("Start"))
	COLUMN_DESCRIPTION(_("Start Time"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdStart)
	char const *ProjectionWidthColumnId() const override {
		return display_mode == SubtitleTimeDisplayMode::Frame ? ProjectionColumnId() : nullptr;
	}

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *c) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId(), DisplayOptions(c)));
	}

	int WidthFromRows(const agi::Context *c, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		if (display_mode == SubtitleTimeDisplayMode::Ass)
			return helper(wxS("0:00:00.00"));
		if (display_mode == SubtitleTimeDisplayMode::Exact)
			return helper(wxS("0:00:00.000"));
		int frame = c->GetCore().videoController->FrameAtTime(
			agi::Time(max_value(&aegisub::presentation::SubtitleGridRow::start_ms, rows)),
			agi::vfr::START);
		return helper(std::to_wstring(frame));
	}
};

struct GridColumnEndTime final : GridColumnTime {
	COLUMN_HEADER(_("End"))
	COLUMN_DESCRIPTION(_("End Time"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdEnd)
	char const *ProjectionWidthColumnId() const override {
		return display_mode == SubtitleTimeDisplayMode::Frame ? ProjectionColumnId() : nullptr;
	}

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *c) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId(), DisplayOptions(c)));
	}

	int WidthFromRows(const agi::Context *c, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		if (display_mode == SubtitleTimeDisplayMode::Ass)
			return helper(wxS("0:00:00.00"));
		if (display_mode == SubtitleTimeDisplayMode::Exact)
			return helper(wxS("0:00:00.000"));
		int frame = c->GetCore().videoController->FrameAtTime(
			agi::Time(max_value(&aegisub::presentation::SubtitleGridRow::end_ms, rows)),
			agi::vfr::END);
		return helper(std::to_wstring(frame));
	}
};

struct GridColumnStyle final : GridColumn {
	COLUMN_HEADER(_("Style"))
	COLUMN_DESCRIPTION(_("Style"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdStyle)
	bool Centered() const override { return false; }
	bool RefreshOnTextChange() const override { return true; }

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId()));
	}

	int WidthFromRows(const agi::Context *, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		return max_width(&aegisub::presentation::SubtitleGridRow::style, rows, helper);
	}
};

struct GridColumnEffect final : GridColumn {
	COLUMN_HEADER(_("Effect"))
	COLUMN_DESCRIPTION(_("Effect"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdEffect)
	bool Centered() const override { return false; }
	bool RefreshOnTextChange() const override { return true; }

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId()));
	}

	int WidthFromRows(const agi::Context *, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		return max_width(&aegisub::presentation::SubtitleGridRow::effect, rows, helper);
	}
};

struct GridColumnActor final : GridColumn {
	COLUMN_HEADER(_("Actor"))
	COLUMN_DESCRIPTION(_("Actor"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdActor)
	bool Centered() const override { return false; }
	bool RefreshOnTextChange() const override { return true; }

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId()));
	}

	int WidthFromRows(const agi::Context *, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		return max_width(&aegisub::presentation::SubtitleGridRow::actor, rows, helper);
	}
};

struct GridColumnMargin : GridColumn {
	int index;
	GridColumnMargin(int index) : index(index) { }

	bool Centered() const override { return true; }

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *) const override {
		return to_wx(aegisub::presentation::FormatSubtitleGridCell(row, ProjectionColumnId()));
	}

	int WidthFromRows(const agi::Context *, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const& rows) const override {
		// Format hides 0 (style default). Non-zero values — including negatives —
		// must reserve width from the actual display string, not only positive max.
		int w = 0;
		for (auto const& row : rows.rows) {
			if (row.margins[index] == 0)
				continue;
			int width = helper(std::to_wstring(row.margins[index]));
			if (width > w)
				w = width;
		}
		return w;
	}
};

struct GridColumnMarginLeft final : GridColumnMargin {
	GridColumnMarginLeft() : GridColumnMargin(0) { }
	COLUMN_HEADER(_("Left"))
	COLUMN_DESCRIPTION(_("Left Margin"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdMarginLeft)
};

struct GridColumnMarginRight final : GridColumnMargin {
	GridColumnMarginRight() : GridColumnMargin(1) { }
	COLUMN_HEADER(_("Right"))
	COLUMN_DESCRIPTION(_("Right Margin"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdMarginRight)
};

struct GridColumnMarginVert final : GridColumnMargin {
	GridColumnMarginVert() : GridColumnMargin(2) { }
	COLUMN_HEADER(_("Vert"))
	COLUMN_DESCRIPTION(_("Vertical Margin"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdMarginVertical)
};

class GridColumnCPS final : public GridColumn {
	const agi::OptionValue *ignore_whitespace = OPT_GET("Subtitle/Character Counter/Ignore Whitespace");
	const agi::OptionValue *ignore_punctuation = OPT_GET("Subtitle/Character Counter/Ignore Punctuation");
	const agi::OptionValue *cps_warn = OPT_GET("Subtitle/Character Counter/CPS Warning Threshold");
	const agi::OptionValue *cps_error = OPT_GET("Subtitle/Character Counter/CPS Error Threshold");
	const agi::OptionValue *show_decimal_cps = OPT_GET("Subtitle/Character Counter/Show Decimal CPS");
	const agi::OptionValue *bg_color = OPT_GET("Colour/Subtitle Grid/CPS Error");
	SubtitleTimeDisplayMode display_mode = SubtitleTimeDisplayMode::Ass;

public:
	COLUMN_HEADER(_("CPS"))
	COLUMN_DESCRIPTION(_("Characters Per Second"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdCps)
	bool Centered() const override { return true; }
	bool RefreshOnTextChange() const override { return true; }
	char const *ProjectionWidthColumnId() const override { return nullptr; }
	void SetDisplayMode(SubtitleTimeDisplayMode mode) override { display_mode = mode; }

	wxString Value(aegisub::presentation::SubtitleGridRow const&, const agi::Context *) const override {
		return wxS("");
	}

	aegisub::presentation::SubtitleGridDisplayOptions DisplayOptions(const agi::Context *c) const {
		aegisub::presentation::SubtitleGridDisplayOptions options;
		options.time_display_mode = display_mode;
		options.timecodes = &c->GetCore().project->Timecodes();
		options.show_decimal_cps = show_decimal_cps->GetBool();
		if (ignore_whitespace->GetBool())
			options.ignore_whitespace = true;
		if (ignore_punctuation->GetBool())
			options.ignore_punctuation = true;
		return options;
	}

	int WidthFromRows(const agi::Context *, WidthHelper &helper, aegisub::presentation::SubtitleGridWindow const&) const override {
		return helper(show_decimal_cps->GetBool() ? wxS("100.0") : wxS("999"));
	}

	void Paint(GridColumnPainter &painter, int x, int y, aegisub::presentation::SubtitleGridRow const& row, const agi::Context *c) const override {
		auto const options = DisplayOptions(c);
		double cps = aegisub::presentation::CalculateSubtitleGridCps(row, options);
		PaintCps(painter, x, y, cps);
	}

	void PaintCps(GridColumnPainter &painter, int x, int y, double cps) const {
		if (cps < 0 || cps > 100) return;

		wxString str = to_wx(aegisub::presentation::FormatSubtitleGridCps(cps, show_decimal_cps->GetBool()));
		int ext_w = 0, ext_h = 0;
		painter.MeasureText(std::wstring(str.wx_str()), ext_w, ext_h);
		auto tc = painter.CurrentTextColor();

		auto const colors = aegisub::presentation::ResolveSubtitleGridCpsColors(
			cps, cps_warn->GetInt(), cps_error->GetInt(), bg_color->GetColor(),
			painter.CurrentRowBackground(), tc);
		if (colors.highlighted) {
			painter.FillRectangle(x, y + 1, width, ext_h + 3, colors.background);
			painter.SetTextColor(colors.text);
		}

		x += (width + 2 - ext_w) / 2;
		painter.DrawText(std::wstring(str.wx_str()), x, y + 2);
		painter.SetTextColor(tc);
	}
};

class GridColumnText final : public GridColumn {
	const agi::OptionValue *override_mode;
	std::string replace_char;

	agi::signal::Connection replace_char_connection;

public:
	GridColumnText()
	: override_mode(OPT_GET("Subtitle/Grid/Hide Overrides"))
	, replace_char(OPT_GET("Subtitle/Grid/Hide Overrides Char")->GetString())
	, replace_char_connection(OPT_SUB("Subtitle/Grid/Hide Overrides Char",
		[&](agi::OptionValue const& v) { replace_char = v.GetString(); }))
	{
	}

	COLUMN_HEADER(_("Text"))
	COLUMN_DESCRIPTION(_("Text"))
	COLUMN_PROJECTION_ID(aegisub::presentation::SubtitleGridColumnIdText)
	bool Centered() const override { return false; }
	bool CanHide() const override { return false; }
	bool RefreshOnTextChange() const override { return true; }
	char const *ProjectionWidthColumnId() const override { return nullptr; }

	wxString Value(aegisub::presentation::SubtitleGridRow const& row, const agi::Context *) const override {
		auto str = to_wx(aegisub::presentation::FormatSubtitleGridText(row.text, OverrideMode(), replace_char));
		if (str.size() > 512)
			str = str.Left(512) + wxS("...");
		return str;
	}

	aegisub::presentation::SubtitleGridOverrideMode OverrideMode() const {
		int mode = override_mode->GetInt();
		if (mode == 0)
			return aegisub::presentation::SubtitleGridOverrideMode::Show;
		if (mode == 1)
			return aegisub::presentation::SubtitleGridOverrideMode::Replace;
		return aegisub::presentation::SubtitleGridOverrideMode::Hide;
	}

	int WidthFromRows(const agi::Context *, WidthHelper &, aegisub::presentation::SubtitleGridWindow const&) const override {
		return 5000;
	}
};

template<typename T>
std::unique_ptr<GridColumn> make() {
	return std::unique_ptr<GridColumn>(new T);
}

}

std::vector<std::unique_ptr<GridColumn>> GetGridColumns() {
	std::vector<std::unique_ptr<GridColumn>> ret;
	ret.push_back(make<GridColumnLineNumber>());
	ret.push_back(make<GridColumnLayer>());
	ret.push_back(make<GridColumnStartTime>());
	ret.push_back(make<GridColumnEndTime>());
	ret.push_back(make<GridColumnCPS>());
	ret.push_back(make<GridColumnStyle>());
	ret.push_back(make<GridColumnActor>());
	ret.push_back(make<GridColumnEffect>());
	ret.push_back(make<GridColumnMarginLeft>());
	ret.push_back(make<GridColumnMarginRight>());
	ret.push_back(make<GridColumnMarginVert>());
	ret.push_back(make<GridColumnText>());
	return ret;
}
