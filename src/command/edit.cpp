// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
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

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_compat.h"
#include "../ass_file.h"
#include "../ass_font_state.h"
#include "../ass_karaoke.h"
#include "../ass_style.h"
#include "../ass_style_resolution.h"
#include "../ass_time_projection.h"
#include "../compat.h"
#include "../dialog_font_face.h"
#include "../dialog_search_replace.h"
#include "../dialogs.h"
#include "../font_face_selection.h"
#include "../font_family_catalog.h"
#include "../font_family_catalog_ui.h"
#include "../font_variant_policy.h"
#include "../font_variant_resolver.h"
#include "../format.h"
#include "../include/aegisub/context.h"
#include "../include/aegisub/context_ui.h"
#include "../initial_line_state.h"
#include "../libresrc/libresrc.h"
#include "../options.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../subtitle_edit_ops.h"
#include "../subtitle_grid_folding.h"
#include "../subs_controller.h"
#include "../subs_edit_box.h"
#include "../async_video_provider.h"
#include "../source_frame.h"
#include "../text_selection_controller.h"
#include "../utils.h"
#include "../video_color_pick.h"
#include "../video_controller.h"
#include "../video_display.h"

#include <libaegisub/address_of_adaptor.h>
#include <libaegisub/character_count.h>
#include <libaegisub/exception.h>
#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

#include <wx/dataobj.h>
#include <wx/clipbrd.h>
#include <wx/msgdlg.h>
#include <wx/textentry.h>

namespace {
	using cmd::Command;

wxDataFormat dialogue_exact_clipboard_format() {
	static wxDataFormat format(wxS("AegisubInternalDialogueExactMsV1"));
	return format;
}

wxDataFormat dialogue_folding_clipboard_format() {
	static wxDataFormat format(wxS("AegisubInternalDialogueFoldingV1"));
	return format;
}

struct CopiedFoldGroup {
	size_t start;
	size_t end;
	bool collapsed;
};

std::vector<CopiedFoldGroup> copied_fold_groups(AssFile& ass, std::vector<AssDialogue *> const& selection) {
	std::vector<CopiedFoldGroup> result;
	for (auto const& group : ass.Folding().Groups()) {
		auto first = std::ranges::lower_bound(selection, group.start, {}, &AssDialogue::Row);
		int length = group.end - group.start + 1;
		if (first == selection.end() || (*first)->Row != group.start || selection.end() - first < length || (*(first + length - 1))->Row != group.end)
			continue;
		auto start = static_cast<size_t>(first - selection.begin());
		result.push_back({.start = start, .end = start + static_cast<size_t>(length) - 1, .collapsed = group.collapsed});
	}
	return result;
}

std::string serialize_copied_fold_groups(std::vector<CopiedFoldGroup> const& groups) {
	std::ostringstream stream;
	for (auto const& group : groups)
		stream << group.start << ' ' << group.end << ' ' << group.collapsed << '\n';
	return stream.str();
}

void restore_copied_fold_groups(AssFile& ass, EntryList<AssDialogue>& lines, std::string const& data) {
	std::vector<AssDialogue *> copies;
	for (auto& line : lines) {
		SubtitleGridFolding::StripMarkers(ass, line);
		copies.push_back(&line);
	}

	std::vector<CopiedFoldGroup> groups;
	std::istringstream stream(data);
	size_t start, end;
	int collapsed;
	while (stream >> std::ws && !stream.eof()) {
		if (!(stream >> start >> end >> collapsed) || start >= end || end >= copies.size() || (collapsed != 0 && collapsed != 1) || (!groups.empty() && start <= groups.back().end))
			return;
		groups.push_back({.start = start, .end = end, .collapsed = collapsed != 0});
	}
	for (auto const& group : groups)
		SubtitleGridFolding::AddCopiedGroup(ass, *copies[group.start], *copies[group.end], group.collapsed);
}

bool is_dialogue_clipboard_line(std::string const& data) {
	auto trimmed = agi::util::strings::trim_copy(data);
	return agi::util::strings::starts_with(trimmed, "Dialogue:")
		|| agi::util::strings::starts_with(trimmed, "Comment:");
}

std::string serialize_dialogue_for_exact_clipboard(AssDialogue const& line) {
	return line.GetEntryData(line.Start.GetAssFormatted(true), line.End.GetAssFormatted(true));
}

std::string get_exact_dialogue_clipboard_payload(std::string *fold_data = nullptr) {
	std::string data;
	wxClipboard *cb = wxClipboard::Get();
	if (cb->Open()) {
		auto const format = dialogue_exact_clipboard_format();
		if (cb->IsSupported(format)) {
			wxCustomDataObject raw_data(format);
			if (cb->GetData(raw_data) && raw_data.GetData() && raw_data.GetSize())
				data.assign(static_cast<char const*>(raw_data.GetData()), raw_data.GetSize());
		}
		if (fold_data && !data.empty() && cb->IsSupported(dialogue_folding_clipboard_format())) {
			wxCustomDataObject raw_data(dialogue_folding_clipboard_format());
			if (cb->GetData(raw_data) && raw_data.GetData() && raw_data.GetSize())
				fold_data->assign(static_cast<char const *>(raw_data.GetData()), raw_data.GetSize());
		}
		cb->Close();
	}
	return data;
}

void set_dialogue_clipboard(std::string const& text_data, std::string const& exact_data, std::string const& fold_data) {
	wxClipboard *cb = wxClipboard::Get();
	if (cb->Open()) {
		auto *composite = new wxDataObjectComposite;
		composite->Add(new wxTextDataObject(to_wx(text_data)), true);

		auto *exact_object = new wxCustomDataObject(dialogue_exact_clipboard_format());
		exact_object->SetData(exact_data.size(), exact_data.data());
		composite->Add(exact_object);
		if (!fold_data.empty()) {
			auto *fold_object = new wxCustomDataObject(dialogue_folding_clipboard_format());
			fold_object->SetData(fold_data.size(), fold_data.data());
			composite->Add(fold_object);
		}

		cb->SetData(composite);
		cb->Flush();
		cb->Close();
	}
}

wxTextEntryBase *focused_text_control() {
	// wxSTC can report focus on an internal child, so walk upward before falling back to line commands.
	for (auto *focus = wxWindow::FindFocus(); focus; focus = focus->GetParent()) {
		if (auto *ctrl = dynamic_cast<wxTextEntryBase*>(focus))
			return ctrl;
	}
	return nullptr;
}

bool copy_focused_text_control() {
	if (auto *ctrl = focused_text_control()) {
		ctrl->Copy();
		return true;
	}
	return false;
}

bool cut_focused_text_control() {
	if (auto *ctrl = focused_text_control()) {
		ctrl->Cut();
		return true;
	}
	return false;
}

bool paste_focused_text_control() {
	if (auto *ctrl = focused_text_control()) {
		ctrl->Paste();
		return true;
	}
	return false;
}

bool parse_dialogue_clipboard_data(std::string const& data, EntryList<AssDialogue> &parsed) {
	auto trimmed = agi::util::strings::trim_copy(data);
	if (!is_dialogue_clipboard_line(trimmed))
		return false;

	try {
		agi::util::strings::for_each_split_any(trimmed, "\r\n", [&](agi::util::strings::view line) {
			auto curdata = agi::util::strings::trim_copy(line);
			if (curdata.empty())
				return;
			parsed.push_back(*new AssDialogue(curdata));
		});
	}
	catch (...) {
		parsed.clear_and_dispose([](AssDialogue *e) { delete e; });
		return false;
	}

	return !parsed.empty();
}

size_t count_dialogue_lines(EntryList<AssDialogue> const& lines) {
	size_t count = 0;
	for (auto const& line : lines) {
		(void)line;
		++count;
	}
	return count;
}

size_t count_clipboard_paste_lines() {
	EntryList<AssDialogue> parsed;
	auto exact_data = get_exact_dialogue_clipboard_payload();
	if (!exact_data.empty() && parse_dialogue_clipboard_data(exact_data, parsed)) {
		auto count = count_dialogue_lines(parsed);
		parsed.clear_and_dispose([](AssDialogue *e) { delete e; });
		return count;
	}

	size_t count = 0;
	agi::util::strings::for_each_split_any(GetClipboard(), "\r\n", [&](agi::util::strings::view) {
		++count;
	});
	return count;
}

template<typename It>
size_t count_lines_until(It pos, It end) {
	size_t count = 0;
	for (; pos != end; ++pos)
		++count;
	return count;
}

bool confirm_paste_over_count_mismatch(wxWindow *parent, wxString const& message) {
	wxMessageDialog dialog(parent, message, _("Paste Lines Over"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
	dialog.SetYesNoLabels(_("Continue"), _("Cancel"));
	return dialog.ShowModal() == wxID_YES;
}

constexpr size_t PASTE_OVER_FIELD_COUNT = 11;

void normalize_paste_over_options(std::vector<bool>& options) {
	if (options.size() == PASTE_OVER_FIELD_COUNT - 1)
		options.insert(options.begin(), false);
	if (options.size() < PASTE_OVER_FIELD_COUNT)
		options.resize(PASTE_OVER_FIELD_COUNT, false);
}

struct validate_sel_nonempty : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->GetSelectedSet().size() > 0;
	}
};

struct validate_video_and_sel_nonempty : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		return core.project->VideoProvider() && !core.selectionController->GetSelectedSet().empty();
	}
};

struct validate_sel_multiple : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->GetSelectedSet().size() > 1;
	}
};

template<typename String>
AssDialogue *get_dialogue(String data) {
	agi::util::strings::trim_inplace(data);
	try {
		// Try to interpret the line as an ASS line
		return new AssDialogue(data);
	}
	catch (...) {
		// Line didn't parse correctly, assume it's plain text that
		// should be pasted in the Text field only
		auto d = new AssDialogue;
		d->End = 0;
		d->Text = data;
		return d;
	}
}

template<typename Paster>
void paste_lines(agi::Context *c, bool paste_over, Paster&& paste_line) {
	auto core = c->GetCore();

	AssDialogue *first = nullptr;
	Selection newsel;

	auto handle_line = [&](std::unique_ptr<AssDialogue> new_line) {
		AssDialogue *inserted = paste_line(new_line.get());
		if (!inserted)
			return false;

		newsel.insert(inserted);
		if (!first)
			first = inserted;
		if (inserted == new_line.get())
			new_line.release();
		return true;
	};

	EntryList<AssDialogue> exact_lines;
	std::string fold_data;
	auto exact_data = get_exact_dialogue_clipboard_payload(&fold_data);
	if (!exact_data.empty() && parse_dialogue_clipboard_data(exact_data, exact_lines)) {
		if (!paste_over)
			restore_copied_fold_groups(*core.ass, exact_lines, fold_data);
		for (auto const& line : exact_lines) {
			if (!handle_line(agi::make_unique<AssDialogue>(line)))
				break;
		}
		exact_lines.clear_and_dispose([](AssDialogue *e) { delete e; });
	}
	else {
		std::string data = GetClipboard();
		if (data.empty()) return;

		bool stop = false;
		agi::util::strings::for_each_split_any(data, "\r\n", [&](agi::util::strings::view line) {
			if (stop)
				return;

			std::unique_ptr<AssDialogue> new_line(get_dialogue(std::string(line)));
			SubtitleGridFolding::StripMarkers(*core.ass, *new_line);
			if (!handle_line(std::move(new_line)))
				stop = true;
		});
	}

	if (first) {
		core.ass->Commit(from_wx(_("paste")), paste_over ? AssFile::COMMIT_DIAG_FULL : AssFile::COMMIT_DIAG_ADDREM);

		if (!paste_over)
			core.selectionController->SetSelectionAndActive(std::move(newsel), first);
	}
}

AssDialogue *paste_over(wxWindow *parent, std::vector<bool>& pasteOverOptions, AssDialogue *new_line, AssDialogue *old_line) {
	if (pasteOverOptions.empty()) {
		if (!ShowPasteOverDialog(parent)) return nullptr;
		pasteOverOptions = OPT_GET("Tool/Paste Lines Over/Fields")->GetListBool();
	}
	normalize_paste_over_options(pasteOverOptions);

	if (pasteOverOptions[0])  old_line->Comment   = new_line->Comment;
	if (pasteOverOptions[1])  old_line->Layer     = new_line->Layer;
	if (pasteOverOptions[2])  old_line->Start     = new_line->Start;
	if (pasteOverOptions[3])  old_line->End       = new_line->End;
	if (pasteOverOptions[4])  old_line->Style     = new_line->Style;
	if (pasteOverOptions[5])  old_line->Actor     = new_line->Actor;
	if (pasteOverOptions[6])  old_line->Margin[0] = new_line->Margin[0];
	if (pasteOverOptions[7])  old_line->Margin[1] = new_line->Margin[1];
	if (pasteOverOptions[8])  old_line->Margin[2] = new_line->Margin[2];
	if (pasteOverOptions[9])  old_line->Effect    = new_line->Effect;
	if (pasteOverOptions[10]) old_line->Text      = new_line->Text;

	return old_line;
}

// The tag-writing and position helpers moved to subtitle_edit_ops so both hosts
// edit overrides through one implementation of reset shadowing and targeting.
using aegisub::subtitle_edit_ops::CaptureOverridePosition;
using aegisub::subtitle_edit_ops::CharacterPos;
using aegisub::subtitle_edit_ops::NormalizePos;
using aegisub::subtitle_edit_ops::ParsedLine;
using aegisub::subtitle_edit_ops::RemapPosForLine;
using aegisub::subtitle_edit_ops::ToggleStyleTag;

template<typename Func>
void update_lines(const agi::Context *c, std::string const& undo_msg, Func&& f) {
	auto core = c->GetCore();
	const auto active_line = core.selectionController->GetActiveLine();
	const int sel_start = core.textSelectionController->GetSelectionStart();
	const int sel_end = core.textSelectionController->GetSelectionEnd();
	auto const start_position = CaptureOverridePosition(active_line->Text, sel_start);
	auto const end_position = CaptureOverridePosition(active_line->Text, sel_end);
	int active_sel_shift = 0;

	for (const auto line : core.selectionController->GetSelectedSet()) {
		auto const start = start_position.ForLine(*line, line == active_line);
		auto const end = end_position.ForLine(*line, line == active_line);

		int shift = f(line, start.raw, end.raw, start.plain, end.plain);
		if (line == active_line)
			active_sel_shift = shift;
	}

	auto const& sel = core.selectionController->GetSelectedSet();
	core.ass->Commit(undo_msg, AssFile::COMMIT_DIAG_TEXT, -1, sel.size() == 1 ? *sel.begin() : nullptr);
	if (active_sel_shift != 0)
		core.textSelectionController->SetSelection(sel_start + active_sel_shift, sel_end + active_sel_shift);
}

void toggle_override_tag(const agi::Context *c, bool (AssStyle::*field), const char *tag, std::string const& undo_msg) {
	auto core = c->GetCore();
	AssStyle const fallback_style;
	aegisub::subtitle_edit_ops::ResetStyleResolver const resolve_reset = [&core](std::string_view name) -> AssStyle const* {
		return aegisub::ass_style_resolution::ResolveResetStyle(*core.ass, std::string(name));
	};
	std::string const tag_name(tag);
	update_lines(c, undo_msg, [&](AssDialogue *line, int sel_start, int sel_end, int norm_sel_start, int norm_sel_end) {
		AssStyle const* const style = core.ass->GetStyle(line->Style);
		AssStyle const& event_style = style ? *style : fallback_style;
		return ToggleStyleTag(*line, event_style, resolve_reset, field, tag_name, sel_start, sel_end, norm_sel_start, norm_sel_end);
	});
}

/// Which colour a colour-editing entry point manipulates: the ASS style field
/// holding the default, and the override tags carrying it (colour, alternate
/// spelling, alpha).
struct ColorEditTarget {
	agi::Color AssStyle::*field;
	const char *tag;
	const char *alt;
	const char *alpha;
};

struct PreparedColorEdit {
	struct LineState {
		agi::Color color; ///< Effective colour at this line's caret, incl. alpha
		ParsedLine parsed;
		int sel_start;      ///< Raw caret position in this line's text
		int norm_sel_start; ///< Plain-text caret position in this line's text
	};

	ColorEditTarget target;
	AssDialogue *active_line = nullptr;
	Selection selection;      ///< Snapshot of the lines the edit applies to
	int sel_start = 0;        ///< Active line raw caret at prepare time
	int sel_end = 0;          ///< Active line raw selection end at prepare time
	agi::Color initial_color; ///< Active line's colour, seeds dialogs
	std::vector<LineState> lines;
	int commit_id = -1; ///< Amended commit id from ApplyPreparedColorEdit
};

PreparedColorEdit PrepareColorEdit(agi::Context *c, ColorEditTarget target) {
	auto core = c->GetCore();
	const auto active_line = core.selectionController->GetActiveLine();
	const int sel_start = core.textSelectionController->GetSelectionStart();
	const int sel_end = core.textSelectionController->GetSelectionEnd();
	auto const start_position = CaptureOverridePosition(active_line->Text, sel_start);

	PreparedColorEdit edit;
	edit.target = target;
	edit.active_line = active_line;
	edit.selection = core.selectionController->GetSelectedSet();
	edit.sel_start = sel_start;
	edit.sel_end = sel_end;

	AssStyle const fallback_style;
	auto resolve_reset = [&core](std::string_view name) -> AssStyle const* {
		return aegisub::ass_style_resolution::ResolveResetStyle(*core.ass, std::string(name));
	};
	for (auto line : core.selectionController->GetSelectedSet()) {
		auto const start = start_position.ForLine(*line, line == active_line);
		int const line_sel_start = start.raw;
		int const line_norm_sel_start = start.plain;

		AssStyle const* const style = core.ass->GetStyle(line->Style);
		AssStyle const& event_style = style ? *style : fallback_style;
		agi::Color color;

		ParsedLine parsed(line);
		// Read from the same raw caret location that set_tag will modify.
		int blockn = parsed.block_for_read(line_sel_start);

		// Honour \r reset semantics so the colour shown in the picker matches
		// what renders at the cursor.
		auto lookup = parsed.find_tag_with_reset(blockn, target.tag, event_style, resolve_reset, target.alt);
		auto lookup_a = parsed.find_tag_with_reset(blockn, target.alpha, event_style, resolve_reset, "\\alpha");
		AssStyle const& base = *lookup.base;
		AssStyle const& base_a = *lookup_a.base;
		// ParseOverrideColor unconditionally assigns all four bytes, so the colour
		// tag would clobber alpha; apply alpha last and take its default from the
		// style (not the just-overwritten colour).
		color = base.*target.field;
		if (lookup.tag)
			color = lookup.tag->Params[0].Get<agi::Color>(color);
		int default_a = (base_a.*target.field).a;
		color.a = static_cast<unsigned char>(
			lookup_a.tag ? lookup_a.tag->Params[0].Get<int>(default_a) : default_a);

		if (line == active_line)
			edit.initial_color = color;

		edit.lines.push_back({color, std::move(parsed), line_sel_start, line_norm_sel_start});
	}
	return edit;
}

int ApplyPreparedColorEdit(
	agi::Context *c,
	PreparedColorEdit& edit,
	agi::Color new_color,
	std::string const& undo_text,
	bool preserve_line_alpha = false) {
	auto core = c->GetCore();
	int active_shift = 0;
	int active_written_block = -1;
	for (auto& line : edit.lines) {
		// Entry points without an alpha input (the video quick pick) must not
		// rewrite transparency: keep each line's effective alpha instead of
		// forcing the seed colour's alpha onto every selected line.
		agi::Color const applied = preserve_line_alpha
									   ? agi::Color(new_color.r, new_color.g, new_color.b, line.color.a)
									   : new_color;
		bool const is_active = line.parsed.line == edit.active_line;
		int shift = line.parsed.set_tag(
			edit.target.tag, AssCompat::FormatOverrideColor(applied),
			line.norm_sel_start, line.sel_start,
			is_active ? &active_written_block : nullptr);
		if (applied.a != line.color.a) {
			shift += line.parsed.set_tag(edit.target.alpha, AssCompat::FormatOverrideAlpha(applied.a), line.norm_sel_start, line.sel_start + shift);
			line.color.a = applied.a;
		}

		if (is_active)
			active_shift = shift;
	}

	edit.commit_id = core.ass->Commit(
		undo_text,
		AssFile::COMMIT_DIAG_TEXT,
		edit.commit_id,
		edit.selection.size() == 1 ? *edit.selection.begin() : nullptr);

	// Park the caret just past the colour tag on the active line rather than
	// shifting the old caret by the line-length delta: an in-place value
	// replacement has a zero delta (leaving the caret wherever the edit-box
	// reload put it), and re-serialization can shift positions by more than
	// the delta anyway. Locate on the final text via the block set_tag
	// reported writing into — a stale caret position resolves ambiguously
	// between same-named tags in neighbouring blocks — falling back to the
	// caret resolution, then the legacy remap.
	auto active = std::find_if(
		edit.lines.begin(), edit.lines.end(),
		[&](PreparedColorEdit::LineState const& line) { return line.parsed.line == edit.active_line; });
	auto const *active_parsed = active != edit.lines.end() ? &active->parsed : nullptr;
	std::optional<int> tag_end;
	if (active_parsed && active_written_block >= 0)
		tag_end = aegisub::subtitle_edit_ops::GetTagEndInWrittenBlock(
			active_parsed->blocks, active_written_block, edit.target.tag, edit.target.alt);
	if (!tag_end && active_parsed)
		tag_end = aegisub::subtitle_edit_ops::GetTagEndInBlock(
			active_parsed->blocks, edit.sel_start, edit.target.tag, edit.target.alt);
	if (tag_end)
		core.textSelectionController->SetSelection(*tag_end, *tag_end);
	else if (active_shift)
		core.textSelectionController->SetSelection(edit.sel_start + active_shift, edit.sel_start + active_shift);
	return edit.commit_id;
}

void show_color_picker(agi::Context *c, ColorEditTarget target) {
	auto core = c->GetCore();
	auto ui = c->GetUI();
	auto edit = PrepareColorEdit(c, target);

	bool ok = GetColorFromUser(ui.parent, edit.initial_color, true, [&](agi::Color new_color) {
		ApplyPreparedColorEdit(c, edit, new_color, from_wx(_("set color")));
	});

	if (!ok && edit.commit_id != -1) {
		core.subsController->Undo();
		core.textSelectionController->SetSelection(edit.sel_start, edit.sel_end);
	}
}

void show_color_picker(agi::Context *c, agi::Color(AssStyle::*field), const char *tag, const char *alt, const char *alpha) {
	show_color_picker(c, ColorEditTarget{field, tag, alt, alpha});
}

struct edit_color_primary final : public Command {
	CMD_NAME("edit/color/primary")
	CMD_ICON(button_color_one)
	STR_MENU("Primary Color...")
	STR_DISP("Primary Color")
	STR_HELP("Set the primary fill color (\\c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::primary, "\\c", "\\1c", "\\1a");
	}
};

struct edit_color_secondary final : public Command {
	CMD_NAME("edit/color/secondary")
	CMD_ICON(button_color_two)
	STR_MENU("Secondary Color...")
	STR_DISP("Secondary Color")
	STR_HELP("Set the secondary (karaoke) fill color (\\2c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::secondary, "\\2c", "", "\\2a");
	}
};

struct edit_color_outline final : public Command {
	CMD_NAME("edit/color/outline")
	CMD_ICON(button_color_three)
	STR_MENU("Outline Color...")
	STR_DISP("Outline Color")
	STR_HELP("Set the outline color (\\3c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::outline, "\\3c", "", "\\3a");
	}
};

struct edit_color_shadow final : public Command {
	CMD_NAME("edit/color/shadow")
	CMD_ICON(button_color_four)
	STR_MENU("Shadow Color...")
	STR_DISP("Shadow Color")
	STR_HELP("Set the shadow color (\\4c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::shadow, "\\4c", "", "\\4a");
	}
};

/// One-shot "click the video to fill this colour slot" session. The command
/// starts it; the actual ASS edit happens when the user later clicks inside
/// the video display. Anything that mutates subtitles, moves the caret or
/// selection, or replaces the video provider while waiting cancels the
/// session instead of writing stale edits.
class VideoQuickPickSession final {
	public:
	static void Begin(agi::Context *c, ColorEditTarget target, std::string owner, wxString mode_name) {
		auto core = c->GetCore();
		auto *display = c->GetUI().videoDisplay;
		if (!display || !core.project->VideoProvider() ||
			!core.selectionController->GetActiveLine() ||
			core.selectionController->GetSelectedSet().empty()) {
			c->ShowStatus(from_wx(_("Video color picking needs a loaded video and an active subtitle line.")));
			return;
		}

		try {
			auto session = std::make_shared<VideoQuickPickSession>(c, target, std::move(owner));
			display->BeginPointSelection(
				session->owner,
				1,
				false,
				[session](std::vector<std::pair<double, double>> points, int frame, bool cancelled) {
					session->Complete(std::move(points), frame, cancelled);
				},
				// Name the mode and both ways out: the pick is armed until the
				// user acts, and the eyedropper is what says so on screen.
				GetEyedropperCursor(),
				fmt_tl("%s: click the video to sample, hover to magnify; Escape or right-click cancels.",
					   mode_name),
				/*live_zoom=*/true);
		}
		catch (std::exception const& err) {
			c->ShowError(err.what(), "Video Color Pick");
		}
	}

	VideoQuickPickSession(agi::Context *context, ColorEditTarget target, std::string owner)
		: context(context), prepared(PrepareColorEdit(context, target)), owner(std::move(owner)) {
		auto core = context->GetCore();
		connections = agi::signal::make_vector({
			core.ass->AddCommitListener([this](int, AssDialogue const *) { Invalidate(); }),
			core.selectionController->AddActiveLineListener([this](AssDialogue *) { Invalidate(); }),
			core.selectionController->AddSelectionListener([this] { Invalidate(); }),
			// The prepared edit snapshots each line's caret position, so a
			// caret move in the edit box must cancel rather than write the
			// tags at the stale position.
			core.textSelectionController->AddSelectionListener([this] { Invalidate(); }),
			core.project->AddVideoProviderListener([this](AsyncVideoProvider *) { Invalidate(); }),
		});
	}

	void Complete(std::vector<std::pair<double, double>> points, int frame, bool cancelled) {
		// Disconnect first so our own commit below cannot re-trigger cancel
		// paths, then decide whether the snapshot is still trustworthy.
		connections.clear();
		if (cancelled) {
			// The user backed out with Escape or right-click; hand the
			// keyboard back to the edit box the armed mode took it from.
			// Invalidation cancels (subtitles/video changed) never reach
			// Complete, so focus the user moved elsewhere is not overridden.
			FocusEditBox();
			return;
		}

		auto core = context->GetCore();
		auto *provider = core.project->VideoProvider();
		bool const stale = frame < 0 || !provider || core.selectionController->GetSelectedSet() != prepared.selection || core.selectionController->GetActiveLine() != prepared.active_line;
		if (stale) {
			context->ShowStatus(from_wx(_("Video color pick cancelled: subtitles or video changed meanwhile.")));
			return;
		}

		if (points.empty())
			return;

		// The clicked point is in the provider's visible/display space while
		// the raw BGRA frame is full storage, so map through the frame
		// geometry to pick up any clean-aperture/crop offset.
		auto const storage = aegisub::color_pick::MapDisplayPointToStorage(
			provider->GetFrameGeometry(), points[0].first, points[0].second);
		if (storage.first < 0 || storage.second < 0) {
			context->ShowStatus(from_wx(_("Could not sample a colour at the clicked point.")));
			return;
		}

		auto frame_time = core.project->Timecodes().TimeAtFrame(frame);
		std::shared_ptr<VideoFrame> bgra;
		try {
			bgra = provider->GetFrameBgra(frame, frame_time, /*raw=*/true);
		}
		catch (agi::Exception const& err) {
			// agi::Exception deliberately does not derive from std::exception;
			// uncaught it would reach the app-wide handler and demand a
			// restart for what is an ordinary decode failure.
			context->ShowStatus(from_wx(_("Could not read the video frame for color picking: ")) + err.GetMessage());
			return;
		}
		if (!bgra || bgra->data.empty()) {
			context->ShowStatus(from_wx(_("Could not read the video frame for color picking.")));
			return;
		}

		int const x = mid(0, storage.first, static_cast<int>(bgra->width) - 1);
		int const y = mid(0, storage.second, static_cast<int>(bgra->height) - 1);

		auto const result = aegisub::color_pick::PickColor(*bgra, x, y, {});
		if (!result.pixels) {
			context->ShowStatus(from_wx(_("Could not sample a colour at the clicked point.")));
			return;
		}

		agi::Color const chosen{result.color.r, result.color.g, result.color.b, 0};
		ApplyPreparedColorEdit(
			context, prepared, chosen, from_wx(_("set color from video")),
			/*preserve_line_alpha=*/true);

		auto region_summary = [&]() {
			std::ostringstream summary;
			summary << from_wx(_("region")) << " " << result.pixels << " px ("
					<< result.bbox_w << "x" << result.bbox_h << "), "
					<< from_wx(_("confidence")) << " " << std::fixed << std::setprecision(2)
					<< result.confidence;
			return summary.str();
		};

		std::ostringstream status;
		status << chosen.GetHexFormatted() << " @ " << x << ',' << y << " - "
			   << from_wx(_("video color pick")) << ": " << region_summary();
		if (result.edge_snapped)
			status << ", " << from_wx(_("edge snapped"));
		if (result.fallback)
			status << ", " << from_wx(_("region unstable, using local median"));
		if (result.capped)
			status << ", " << from_wx(_("capped at size limit"));
		context->ShowStatus(status.str());
		// The pick is done and the colour is in; the next keystroke belongs
		// in the edit box, on the caret ApplyPreparedColorEdit just placed.
		FocusEditBox();
	}

	private:
		/// The armed mode moved the keyboard focus to the video canvas; put it
		/// back so the next keystroke lands in the edit box.
		void FocusEditBox() {
			auto ui = context->GetUI();
			if (ui.subsEditBox && ui.subsEditBox->CanFocusEditControl())
				ui.subsEditBox->FocusEditControl();
		}

		/// Drop the pending pick because subtitles or video changed underneath us.
	void Invalidate() {
		if (!connections.empty()) {
			connections.clear();
			context->ShowStatus(from_wx(_("Video color pick cancelled: subtitles or video changed meanwhile.")));
			auto *display = context->GetUI().videoDisplay;
			if (display)
				display->CancelPointSelection(owner, false);
		}
	}

	agi::Context *context;
	PreparedColorEdit prepared;
	std::string owner;
	std::vector<agi::signal::Connection> connections;
};

struct edit_color_quick_pick_video_base : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	virtual ColorEditTarget Target() const = 0;
	virtual const char *PickOwner() const = 0;

	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		// The active line can be null independently of the selected set
		// (SetActiveLine(nullptr) is legal and does not touch the selection),
		// and the prepared edit dereferences it.
		return core.selectionController->GetActiveLine() != nullptr && !core.selectionController->GetSelectedSet().empty() && !!core.project->VideoProvider() && !!c->GetUI().videoDisplay && core.videoController->GetFrameN() >= 0;
	}

	void operator()(agi::Context *c) override {
		if (!Validate(c)) {
			c->ShowStatus(from_wx(_("Video color picking needs a loaded video and an active subtitle line.")));
			return;
		}
		VideoQuickPickSession::Begin(c, Target(), PickOwner(), StrDisplay(c));
	}
};

struct edit_color_primary_pick_video final : public edit_color_quick_pick_video_base {
	CMD_NAME("edit/color/primary/pick/video")
	CMD_ICON(button_color_one)
	STR_MENU("Primary Color from Video...")
	STR_DISP("Primary Color from Video")
	STR_HELP("Pick the primary fill color (\\c) from a raw video frame")

	ColorEditTarget Target() const override { return {&AssStyle::primary, "\\c", "\\1c", "\\1a"}; }
	const char *PickOwner() const override { return "edit/color/primary/pick/video"; }
};

struct edit_color_secondary_pick_video final : public edit_color_quick_pick_video_base {
	CMD_NAME("edit/color/secondary/pick/video")
	CMD_ICON(button_color_two)
	STR_MENU("Secondary Color from Video...")
	STR_DISP("Secondary Color from Video")
	STR_HELP("Pick the secondary (karaoke) fill color (\\2c) from a raw video frame")

	ColorEditTarget Target() const override { return {&AssStyle::secondary, "\\2c", "", "\\2a"}; }
	const char *PickOwner() const override { return "edit/color/secondary/pick/video"; }
};

struct edit_color_outline_pick_video final : public edit_color_quick_pick_video_base {
	CMD_NAME("edit/color/outline/pick/video")
	CMD_ICON(button_color_three)
	STR_MENU("Outline Color from Video...")
	STR_DISP("Outline Color from Video")
	STR_HELP("Pick the outline color (\\3c) from a raw video frame")

	ColorEditTarget Target() const override { return {&AssStyle::outline, "\\3c", "", "\\3a"}; }
	const char *PickOwner() const override { return "edit/color/outline/pick/video"; }
};

struct edit_color_shadow_pick_video final : public edit_color_quick_pick_video_base {
	CMD_NAME("edit/color/shadow/pick/video")
	CMD_ICON(button_color_four)
	STR_MENU("Shadow Color from Video...")
	STR_DISP("Shadow Color from Video")
	STR_HELP("Pick the shadow color (\\4c) from a raw video frame")

	ColorEditTarget Target() const override { return {&AssStyle::shadow, "\\4c", "", "\\4a"}; }
	const char *PickOwner() const override { return "edit/color/shadow/pick/video"; }
};

struct edit_style_bold final : public Command {
	CMD_NAME("edit/style/bold")
	CMD_ICON(button_bold)
	STR_MENU("Toggle Bold")
	STR_DISP("Toggle Bold")
	STR_HELP("Toggle bold (\\b) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::bold, "\\b", from_wx(_("toggle bold")));
	}
};

struct edit_style_italic final : public Command {
	CMD_NAME("edit/style/italic")
	CMD_ICON(button_italics)
	STR_MENU("Toggle Italics")
	STR_DISP("Toggle Italics")
	STR_HELP("Toggle italics (\\i) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::italic, "\\i", from_wx(_("toggle italic")));
	}
};

struct edit_style_underline final : public Command {
	CMD_NAME("edit/style/underline")
	CMD_ICON(button_underline)
	STR_MENU("Toggle Underline")
	STR_DISP("Toggle Underline")
	STR_HELP("Toggle underline (\\u) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::underline, "\\u", from_wx(_("toggle underline")));
	}
};

struct edit_style_strikeout final : public Command {
	CMD_NAME("edit/style/strikeout")
	CMD_ICON(button_strikeout)
	STR_MENU("Toggle Strikeout")
	STR_DISP("Toggle Strikeout")
	STR_HELP("Toggle strikeout (\\s) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::strikeout, "\\s", from_wx(_("toggle strikeout")));
	}
};

struct edit_font final : public Command {
	CMD_NAME("edit/font")
	CMD_ICON(button_fontname)
	STR_MENU("Font Face...")
	STR_DISP("Font Face")
	STR_HELP("Select a font face and size")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		auto ui = c->GetUI();
		auto font_model = BuildFontFamilyCatalogUiModel();
		const ParsedLine active(core.selectionController->GetActiveLine());
		const int active_insertion_point = core.textSelectionController->GetInsertionPoint();
		const size_t insertion_chars = CharacterPos(active.line->Text, core.textSelectionController->GetInsertionPoint());

		struct line_font_state {
			FontFaceDialogSelection displayed;
			bool valid = true;
		};

		auto font_for_line = [&](ParsedLine const& line, int insertion_point) -> line_font_state {
			// Use the raw caret position so a caret inside an override reads the
			// same block that set_tag will modify.
			const int blockn = line.block_for_read(insertion_point);

			const AssStyle *style = aegisub::ass_style_resolution::ResolveEventStyle(
				*core.ass, line.line->Style);
			const AssStyle default_style;
			if (!style)
				style = &default_style;
			aegisub::ass::AssFontStateEvaluator evaluator(
				aegisub::ass::MakeAssFontStyleBaseline(*style));
			auto resolve_reset = [&](std::string_view name)
				-> std::optional<aegisub::ass::AssFontStyleBaseline> {
				auto const *reset = aegisub::ass_style_resolution::ResolveResetStyle(
					*core.ass, std::string(name));
				if (!reset)
					return std::nullopt;
				return aegisub::ass::MakeAssFontStyleBaseline(*reset);
			};
			for (int index = 0; index <= blockn && index < static_cast<int>(line.blocks.size()); ++index) {
				if (auto const *override_block =
						dynamic_cast<AssDialogueBlockOverride const *>(line.blocks[index].get()))
					evaluator.ApplyBlock(*override_block, resolve_reset);
			}
			auto const& request = evaluator.Request();

			line_font_state state;
			state.valid = request.valid;
			state.displayed.face_name = request.family;
			state.displayed.point_size = static_cast<int>(std::lround(request.height));
			state.displayed.charset = request.charset;
			state.displayed.effective_weight = request.effective_weight;
			state.displayed.bold = request.effective_weight == 700;
			state.displayed.italic = request.italic;
			state.displayed.has_explicit_weight = request.has_explicit_bold;
			state.displayed.has_explicit_italic = request.has_explicit_italic;

			// Underline is part of the font selection and must reflect the state at
			// the cursor, including any \r reset.
			auto resolve_reset_style = [&](std::string_view name) -> AssStyle const* {
				return aegisub::ass_style_resolution::ResolveResetStyle(*core.ass, std::string(name));
			};
			state.displayed.underline = line.get_value_with_reset(blockn, &AssStyle::underline, "\\u", *style, resolve_reset_style);
			return state;
		};

		// The dialog previews onto the real document, so every pass has to start
		// from the same text and the same caret positions. Snapshot both once:
		// set_tag is not idempotent (it only writes a tag when the value differs
		// from the line's current state), and reading the caret after a preview
		// would let it drift by the length of the tags the previous pass inserted.
		struct preview_line {
			AssDialogue *line = nullptr;
			std::string pristine_text;
			std::string rendered_text;
			int sel_start = 0;
			int sel_end = 0;
			int norm_sel_start = 0;
			int norm_sel_end = 0;
			int insertion_point = 0;
		};

		std::vector<preview_line> preview_lines;
		{
			auto const *active_line = core.selectionController->GetActiveLine();
			const int sel_start = core.textSelectionController->GetSelectionStart();
			const int sel_end = core.textSelectionController->GetSelectionEnd();
			const size_t sel_start_chars = CharacterPos(active_line->Text, sel_start);
			const size_t sel_end_chars = CharacterPos(active_line->Text, sel_end);

			auto const& selected_set = core.selectionController->GetSelectedSet();
			preview_lines.reserve(selected_set.size());
			for (auto *line : selected_set) {
				preview_line snapshot;
				snapshot.line = line;
				snapshot.pristine_text = line->Text.get();
				snapshot.rendered_text = snapshot.pristine_text;
				if (line == active_line) {
					snapshot.sel_start = sel_start;
					snapshot.sel_end = sel_end;
					snapshot.norm_sel_start = NormalizePos(line->Text, sel_start);
					snapshot.norm_sel_end = NormalizePos(line->Text, sel_end);
					snapshot.insertion_point = active_insertion_point;
				}
				else {
					auto start = RemapPosForLine(line, sel_start_chars);
					auto end = RemapPosForLine(line, sel_end_chars);
					snapshot.sel_start = start.raw;
					snapshot.sel_end = end.raw;
					snapshot.norm_sel_start = start.plain;
					snapshot.norm_sel_end = end.plain;
					snapshot.insertion_point = RemapPosForLine(line, insertion_chars).raw;
				}
				preview_lines.push_back(std::move(snapshot));
			}
		}
		auto *active_preview = [&]() -> preview_line * {
			auto found = std::find_if(
				preview_lines.begin(), preview_lines.end(),
				[&](preview_line const& snapshot) { return snapshot.line == active.line; });
			return found == preview_lines.end() ? nullptr : &*found;
		}();

		std::optional<bool> native_override_permission;
		// Hoisted so the resolver's GDI probe memo survives across previews.
		std::unique_ptr<FontVariantResolver> live_resolver;
		auto apply_selection = [&](FontFaceDialogSelection const& selected, bool permanent) {
			// Build every result from the state at dialog entry. The temporary
			// restore and rewrite happen on the UI thread before a refresh is sent,
			// so no intermediate frame is visible.
			for (auto& snapshot : preview_lines) {
				if (snapshot.line->Text.get() != snapshot.pristine_text)
					snapshot.line->Text = snapshot.pristine_text;
			}
			// Preview caret shifts update the current undo snapshot. Put its saved
			// caret back before creating the final snapshot so Undo restores both
			// the entry text and selection from dialog entry.
			if (permanent && active_preview)
				core.textSelectionController->SetSelection(
					active_preview->sel_start, active_preview->sel_end);

			bool allow_replace_explicit = selected.allow_replace_explicit;
			// A preview must never raise a modal prompt of its own; without a
			// decision yet, preview conservatively as "don't replace".
			if (selected.from_native_dialog && selected.variant_modified &&
			    !allow_replace_explicit && !(!permanent && !native_override_permission)) {
				bool has_conflict = false;
				for (auto const& snapshot : preview_lines) {
					ParsedLine parsed(snapshot.line);
					auto const current = font_for_line(parsed, snapshot.insertion_point);
					if ((current.displayed.has_explicit_weight &&
					     current.displayed.effective_weight != selected.effective_weight) ||
					    (current.displayed.has_explicit_italic &&
					     current.displayed.italic != selected.italic)) {
						has_conflict = true;
						break;
					}
				}
				if (has_conflict) {
					if (!native_override_permission) {
						native_override_permission = wxMessageBox(
							_("The selected lines contain explicit \\b or \\i overrides. Replace those explicit variants for this operation?"),
							_("Replace explicit font variants?"),
							wxYES_NO | wxICON_QUESTION,
							ui.parent) == wxYES;
					}
					allow_replace_explicit = *native_override_permission;
				}
			}

			auto may_write_weight = [&](line_font_state const& startfont) {
				return !startfont.displayed.has_explicit_weight || allow_replace_explicit ||
					   selected.weight_modified;
			};
			auto may_write_italic = [&](line_font_state const& startfont) {
				return !startfont.displayed.has_explicit_italic || allow_replace_explicit ||
					   selected.italic_modified;
			};

			struct line_variant_selection {
				int weight = 400;
				bool italic = false;
				bool automatic_variant_reliable = true;
			};

			FontFamilyRecord const *selected_record = nullptr;
			if (selected.implicit_variant_pinned && !selected.variant_modified &&
			    font_model.catalog && !font_model.catalog->empty()) {
				if (selected.selected_family_id)
					selected_record = font_model.catalog->Find(*selected.selected_family_id);
				if (!selected_record) {
					auto resolved = font_model.catalog->Resolve(selected.face_name);
					if (resolved.family)
						selected_record = font_model.catalog->Find(*resolved.family);
				}
				if (selected_record && !live_resolver)
					live_resolver = CreatePlatformFontVariantResolver();
			}

			// Family and height are fixed within one pass, so the profile only
			// varies by charset; most selections share one, collapsing N probes
			// into one.
			std::map<int, std::optional<FontFamilyVariantProfile>> profile_cache;

			auto variant_for_line = [&](line_font_state const& startfont) {
				line_variant_selection target{
					selected.effective_weight,
					selected.italic,
					true};
				if (!selected.implicit_variant_pinned || selected.variant_modified)
					return target;

				if (!startfont.valid || !selected_record || !live_resolver) {
					target.automatic_variant_reliable = false;
					return target;
				}

				auto cached = profile_cache.find(startfont.displayed.charset);
				if (cached == profile_cache.end()) {
					cached = profile_cache.emplace(
						startfont.displayed.charset,
						BuildFontVariantProfileForFamily(
							*live_resolver, *selected_record,
							startfont.displayed.charset,
							static_cast<double>(selected.point_size))).first;
				}
				auto const& profile = cached->second;
				if (!profile || !profile->automatic_pinning_reliable) {
					target.automatic_variant_reliable = false;
					return target;
				}
				FontVariantSelection current{
					startfont.displayed.effective_weight,
					startfont.displayed.italic,
					startfont.displayed.has_explicit_weight,
					startfont.displayed.has_explicit_italic};
				auto adjusted = AdjustFamilySelection(
					current, *profile, {true, allow_replace_explicit});
				if (adjusted.applied_implicit_selection) {
					target.weight = adjusted.selection.weight;
					target.italic = adjusted.selection.italic;
					return target;
				}

				// An explicit override remains authoritative. Likewise, a complete
				// profile with no implicit fallback keeps this line's existing intent.
				if (adjusted.blocked_by_explicit ||
				    (current.weight != 400 && current.weight != 700)) {
					target.weight = current.weight;
					target.italic = current.italic;
					return target;
				}
				auto const& outcome = profile->For(current.weight == 700, current.italic);
				auto canonical = CanonicalizeFontVariant(outcome);
				if (canonical && canonical->weight == current.weight &&
				    canonical->italic == current.italic) {
					target.weight = current.weight;
					target.italic = current.italic;
					return target;
				}
				target.automatic_variant_reliable = false;
				return target;
			};

			// Single pass: derive from pristine text, write the tags, then compare
			// both with the last rendered preview and the dialog-entry state.
			int unknown_skipped = 0;
			int active_shift = 0;
			bool has_final_changes = false;
			std::vector<AssDialogue const *> refresh_lines;
			std::vector<AssDialogue const *> commit_lines;
			refresh_lines.reserve(preview_lines.size());
			commit_lines.reserve(preview_lines.size());

			for (auto& snapshot : preview_lines) {
				AssDialogue *line = snapshot.line;
				ParsedLine parsed(line);
				auto const startfont = font_for_line(parsed, snapshot.insertion_point);
				auto const variant = variant_for_line(startfont);
				if (!variant.automatic_variant_reliable)
					++unknown_skipped;

				int shift = 0;
				auto do_set_tag = [&](const char *tag_name, std::string const& value) {
					shift += parsed.set_tag(
						tag_name, value, snapshot.norm_sel_start, snapshot.sel_start + shift);
				};

				if (ShouldWriteFontFace(
						startfont.displayed.face_name,
						selected.face_name,
						selected.face_modified))
					do_set_tag("\\fn", selected.face_name);
				if (selected.point_size != startfont.displayed.point_size)
					do_set_tag("\\fs", std::to_string(selected.point_size));
				if (variant.automatic_variant_reliable && may_write_weight(startfont) &&
				    variant.weight != startfont.displayed.effective_weight) {
					std::string value = variant.weight == 400 ? "0" :
					                    variant.weight == 700 ? "1" :
					                    std::to_string(variant.weight);
					do_set_tag("\\b", value);
				}
				if (variant.automatic_variant_reliable && may_write_italic(startfont) &&
				    variant.italic != startfont.displayed.italic)
					do_set_tag("\\i", std::to_string(variant.italic));
				if (selected.underline != startfont.displayed.underline)
					do_set_tag("\\u", std::to_string(selected.underline));

				if (line == active.line)
					active_shift = shift;

				bool const needs_refresh = line->Text.get() != snapshot.rendered_text;
				bool const differs_from_entry = line->Text.get() != snapshot.pristine_text;
				if (needs_refresh)
					refresh_lines.push_back(line);
				if (permanent && (needs_refresh || differs_from_entry))
					commit_lines.push_back(line);
				has_final_changes |= differs_from_entry;
			}

			auto show_unknown_summary = [&] {
				if (!permanent || unknown_skipped == 0)
					return;
				wxMessageBox(
					wxString::Format(
						_("%d line(s) kept their existing weight/italic because their font variant state could not be resolved reliably."),
						unknown_skipped),
					_("Font variant not changed"),
					wxOK | wxICON_WARNING,
					ui.parent);
			};

			if (permanent && has_final_changes) {
				core.ass->Commit(
					from_wx(_("set font")), AssFile::COMMIT_DIAG_TEXT, -1,
					commit_lines.size() == 1 ? const_cast<AssDialogue *>(commit_lines.front()) : nullptr,
					AssDialogueCommitSpan(commit_lines.data(), commit_lines.size()));
			}
			else if (!refresh_lines.empty()) {
				// Empty descriptions are broadcast to renderers and editors but are
				// ignored by SubsController's undo/redo and autosave-on-change path.
				core.ass->Commit(
					"", AssFile::COMMIT_DIAG_TEXT, -1,
					refresh_lines.size() == 1 ? const_cast<AssDialogue *>(refresh_lines.front()) : nullptr,
					AssDialogueCommitSpan(refresh_lines.data(), refresh_lines.size()));
			}

			for (auto& snapshot : preview_lines)
				snapshot.rendered_text = snapshot.line->Text.get();

			if (active_preview)
				core.textSelectionController->SetSelection(
					active_preview->sel_start + active_shift,
					active_preview->sel_end + active_shift);

			show_unknown_summary();
		};

		auto initial = font_for_line(active, active_insertion_point);

		FontFaceDialogHooks hooks;
		hooks.on_preview = [&](FontFaceDialogSelection const& selected) {
			apply_selection(selected, false);
		};
	hooks.on_commit = [&](FontFaceDialogSelection const& selected) {
			apply_selection(selected, true);
		};
		hooks.on_revert = [&] {
			// Restore the unshifted selection while the longer preview text is
			// still displayed; shrinking the text first would briefly leave the
			// edit control with a range past end-of-text.
			if (active_preview)
				core.textSelectionController->SetSelection(
					active_preview->sel_start, active_preview->sel_end);

			std::vector<AssDialogue const *> restored;
			restored.reserve(preview_lines.size());
			for (auto& snapshot : preview_lines) {
				if (snapshot.line->Text.get() != snapshot.pristine_text) {
					snapshot.line->Text = snapshot.pristine_text;
					restored.push_back(snapshot.line);
				}
				snapshot.rendered_text = snapshot.pristine_text;
			}
			if (!restored.empty()) {
				core.ass->Commit("", AssFile::COMMIT_DIAG_TEXT, -1,
					restored.size() == 1 ? const_cast<AssDialogue *>(restored.front()) : nullptr,
					AssDialogueCommitSpan(restored.data(), restored.size()));
			}
		};

		auto autosave_inhibitor = core.subsController->InhibitAutosave();
		auto selected = ShowFontFaceDialog(
			ui.parent, initial.displayed, font_model, std::move(hooks));
		// OK already committed through on_commit; applying `selected` again would
		// double-write.
		(void)selected;
	}
};

struct edit_find_replace final : public Command {
	CMD_NAME("edit/find_replace")
	CMD_ICON(find_replace_menu)
	STR_MENU("Find and R&eplace...")
	STR_DISP("Find and Replace")
	STR_HELP("Find and replace words in subtitles")

	void operator()(agi::Context *c) override {
		c->GetCore().videoController->Stop();
		DialogSearchReplace::Show(c, true);
	}
};

static void copy_lines(agi::Context *c) {
	auto core = c->GetCore();
	auto selection = c->GetCore().selectionController->GetSortedSelection();
	std::vector<std::string> text_lines;
	std::vector<std::string> exact_lines;
	text_lines.reserve(selection.size());
	exact_lines.reserve(selection.size());
	for (auto* dialogue : selection) {
		AssDialogue copy(*dialogue);
		SubtitleGridFolding::StripMarkers(*core.ass, copy);
		text_lines.push_back(SerializeAssDialogueForOutput(copy, AssTimeOutputMode::LegacyRounding, &core.project->Timecodes()));
		exact_lines.push_back(serialize_dialogue_for_exact_clipboard(copy));
	}
	set_dialogue_clipboard(
		agi::util::strings::join(text_lines, "\r\n"),
		agi::util::strings::join(exact_lines, "\r\n"),
		serialize_copied_fold_groups(copied_fold_groups(*core.ass, selection)));
}

static void delete_lines(agi::Context *c, std::string const& commit_message) {
	auto core = c->GetCore();
	auto const& sel = core.selectionController->GetSelectedSet();

	// Find a line near the active line not being deleted to make the new active line
	AssDialogue *pre_sel = nullptr;
	AssDialogue *post_sel = nullptr;
	bool hit_selection = false;

	for (auto& diag : core.ass->Events) {
		if (sel.count(&diag))
			hit_selection = true;
		else if (hit_selection && !post_sel) {
			post_sel = &diag;
			break;
		}
		else
			pre_sel = &diag;
	}

	// Remove the selected lines, but defer the deletion until after we select
	// different lines. We can't just change the selection first because we may
	// need to create a new dialogue line for it, and we can't select dialogue
	// lines until after they're committed.
	std::vector<std::unique_ptr<AssDialogue>> to_delete;
	core.ass->Events.remove_and_dispose_if([&sel](AssDialogue const& e) {
		return sel.count(const_cast<AssDialogue *>(&e));
	}, [&](AssDialogue *e) {
		to_delete.emplace_back(e);
	});

	AssDialogue *new_active = post_sel;
	if (!new_active)
		new_active = pre_sel;
	// If we didn't get a new active line then we just deleted all the dialogue
	// lines, so make a new one
	if (!new_active) {
		new_active = new AssDialogue;
		core.ass->Events.push_back(*new_active);
	}

	core.ass->Commit(commit_message, AssFile::COMMIT_DIAG_ADDREM);
	core.selectionController->SetSelectionAndActive({ new_active }, new_active);
}

struct edit_line_copy final : public validate_sel_nonempty {
	CMD_NAME("edit/line/copy")
	CMD_ICON(copy_button)
	STR_MENU("&Copy Lines")
	STR_DISP("Copy Lines")
	STR_HELP("Copy subtitles to the clipboard")

	void operator()(agi::Context *c) override {
		// Ideally we'd let the control's keydown handler run and only deal
		// with the events not processed by it, but that doesn't seem to be
		// possible with how wx implements key event handling - the native
		// platform processing is evoked only if the wx event is unprocessed,
		// and there's no way to do something if the native platform code leaves
		// it unprocessed

		if (!copy_focused_text_control()) {
			copy_lines(c);
		}
	}
};

struct edit_line_cut: public validate_sel_nonempty {
	CMD_NAME("edit/line/cut")
	CMD_ICON(cut_button)
	STR_MENU("Cu&t Lines")
	STR_DISP("Cut Lines")
	STR_HELP("Cut subtitles")

	void operator()(agi::Context *c) override {
		if (!cut_focused_text_control()) {
			copy_lines(c);
			delete_lines(c, from_wx(_("cut lines")));
		}
	}
};

struct edit_line_delete final : public validate_sel_nonempty {
	CMD_NAME("edit/line/delete")
	CMD_ICON(delete_button)
	STR_MENU("De&lete Lines")
	STR_DISP("Delete Lines")
	STR_HELP("Delete currently selected lines")

	void operator()(agi::Context *c) override {
		delete_lines(c, from_wx(_("delete lines")));
	}
};

static void duplicate_lines(agi::Context *c, int shift) {
	auto core = c->GetCore();
	auto groups = copied_fold_groups(*core.ass, core.selectionController->GetSortedSelection());
	std::vector<AssDialogue *> copies;
	auto const& sel = core.selectionController->GetSelectedSet();
	auto in_selection = [&](AssDialogue const& d) { return sel.count(const_cast<AssDialogue *>(&d)); };

	Selection new_sel;
	AssDialogue *new_active = nullptr;

	auto start = core.ass->Events.begin();
	auto end = core.ass->Events.end();
	while (start != end) {
		// Find the first line in the selection
		start = std::find_if(start, end, in_selection);
		if (start == end) break;

		// And the last line in this contiguous selection
		auto insert_pos = std::find_if_not(start, end, in_selection);
		auto last = std::prev(insert_pos);

		// Duplicate each of the selected lines, inserting them in a block
		// after the selected block
		do {
			auto old_diag = &*start;
			auto new_diag = new AssDialogue(*old_diag);
			SubtitleGridFolding::StripMarkers(*core.ass, *new_diag);
			copies.push_back(new_diag);

			core.ass->Events.insert(insert_pos, *new_diag);
			new_sel.insert(new_diag);
			if (!new_active)
				new_active = new_diag;

			if (shift) {
				int cur_frame = core.videoController->GetFrameN();
				int old_start = core.videoController->FrameAtTime(new_diag->Start, agi::vfr::START);
				int old_end = core.videoController->FrameAtTime(new_diag->End, agi::vfr::END);

				// If the current frame isn't within the range of the line then
				// splitting doesn't make any sense, so instead just duplicate
				// the line and set the new one to just this frame
				if (cur_frame < old_start || cur_frame > old_end) {
					new_diag->Start = core.videoController->TimeAtFrame(cur_frame, agi::vfr::START);
					new_diag->End = core.videoController->TimeAtFrame(cur_frame, agi::vfr::END);
				}
				/// @todo This does dumb things when old_start == old_end
				else if (shift < 0) {
					old_diag->End = core.videoController->TimeAtFrame(cur_frame - 1, agi::vfr::END);
					new_diag->Start = core.videoController->TimeAtFrame(cur_frame, agi::vfr::START);
				}
				else {
					old_diag->End = core.videoController->TimeAtFrame(cur_frame, agi::vfr::END);
					new_diag->Start = core.videoController->TimeAtFrame(cur_frame + 1, agi::vfr::START);
				}

				/// @todo also split \t and \move?
			}
		} while (start++ != last);

		// Skip over the lines we just made
		start = insert_pos;
	}

	if (new_sel.empty()) return;
	for (auto const& group : groups)
		SubtitleGridFolding::AddCopiedGroup(*core.ass, *copies[group.start], *copies[group.end], group.collapsed);

	core.ass->Commit(from_wx(shift ? _("split") : _("duplicate lines")), AssFile::COMMIT_DIAG_ADDREM);

	core.selectionController->SetSelectionAndActive(std::move(new_sel), new_active);
}

struct edit_line_duplicate final : public validate_sel_nonempty {
	CMD_NAME("edit/line/duplicate")
	STR_MENU("&Duplicate Lines")
	STR_DISP("Duplicate Lines")
	STR_HELP("Duplicate the selected lines")

	void operator()(agi::Context *c) override {
		duplicate_lines(c, 0);
	}
};

struct edit_line_duplicate_shift final : public validate_video_and_sel_nonempty {
	CMD_NAME("edit/line/split/after")
	STR_MENU("Split lines after current frame")
	STR_DISP("Split lines after current frame")
	STR_HELP("Split the current line into a line which ends on the current frame and a line which starts on the next frame")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		duplicate_lines(c, 1);
	}
};

struct edit_line_duplicate_shift_back final : public validate_video_and_sel_nonempty {
	CMD_NAME("edit/line/split/before")
	STR_MENU("Split lines before current frame")
	STR_DISP("Split lines before current frame")
	STR_HELP("Split the current line into a line which ends on the previous frame and a line which starts on the current frame")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		duplicate_lines(c, -1);
	}
};

static void combine_lines(agi::Context *c, aegisub::subtitle_edit_ops::JoinMode mode, std::string const& message) {
	auto core = c->GetCore();
	auto sel = core.selectionController->GetSortedSelection();
	if (!aegisub::subtitle_edit_ops::JoinSelectionIntoFirst(sel, mode))
		return;

	AssDialogue *first = sel[0];
	for (size_t i = 1; i < sel.size(); ++i)
		delete sel[i];

	core.selectionController->SetSelectionAndActive({first}, first);

	core.ass->Commit(message, AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
}

struct edit_line_join_as_karaoke final : public validate_sel_multiple {
	CMD_NAME("edit/line/join/as_karaoke")
	STR_MENU("As &Karaoke")
	STR_DISP("As Karaoke")
	STR_HELP("Join selected lines in a single one, as karaoke")

	void operator()(agi::Context *c) override {
		combine_lines(c, aegisub::subtitle_edit_ops::JoinMode::Karaoke, from_wx(_("join as karaoke")));
	}
};

struct edit_line_join_concatenate final : public validate_sel_multiple {
	CMD_NAME("edit/line/join/concatenate")
	STR_MENU("&Concatenate")
	STR_DISP("Concatenate")
	STR_HELP("Join selected lines in a single one, concatenating text together")

	void operator()(agi::Context *c) override {
		combine_lines(c, aegisub::subtitle_edit_ops::JoinMode::Concatenate, from_wx(_("join lines")));
	}
};

struct edit_line_join_with_line_breaks final : public validate_sel_multiple {
	CMD_NAME("edit/line/join/with_line_breaks")
	STR_MENU("With \\N")
	STR_DISP("Join with \\N")
	STR_HELP("Join selected lines in a single one, inserting \\N between lines")

	void operator()(agi::Context *c) override {
		combine_lines(c, aegisub::subtitle_edit_ops::JoinMode::WithLineBreaks, from_wx(_("join lines")));
	}
};

struct edit_line_join_keep_first final : public validate_sel_multiple {
	CMD_NAME("edit/line/join/keep_first")
	STR_MENU("Keep &First")
	STR_DISP("Keep First")
	STR_HELP("Join selected lines in a single one, keeping text of first and discarding remaining")

	void operator()(agi::Context *c) override {
		combine_lines(c, aegisub::subtitle_edit_ops::JoinMode::KeepFirst, from_wx(_("join lines")));
	}
};

static bool try_paste_lines(agi::Context *c) {
	auto core = c->GetCore();
	EntryList<AssDialogue> parsed;
	std::string fold_data;
	auto exact_data = get_exact_dialogue_clipboard_payload(&fold_data);
	bool exact_parsed = !exact_data.empty() && parse_dialogue_clipboard_data(exact_data, parsed);
	if (!exact_parsed && !parse_dialogue_clipboard_data(GetClipboard(), parsed))
		return false;
	restore_copied_fold_groups(*core.ass, parsed, exact_parsed ? fold_data : std::string{});

	AssDialogue *new_active = &*parsed.begin();
	Selection new_selection;
	for (auto& line : parsed)
		new_selection.insert(&line);

	auto pos = core.ass->iterator_to(*core.selectionController->GetActiveLine());
	core.ass->Events.splice(pos, parsed, parsed.begin(), parsed.end());
	core.ass->Commit(from_wx(_("paste")), AssFile::COMMIT_DIAG_ADDREM);
	core.selectionController->SetSelectionAndActive(std::move(new_selection), new_active);

	return true;
}

struct edit_line_paste final : public Command {
	CMD_NAME("edit/line/paste")
	CMD_ICON(paste_button)
	STR_MENU("&Paste Lines")
	STR_DISP("Paste Lines")
	STR_HELP("Paste subtitles")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *) override {
		bool can_paste = false;
		if (wxTheClipboard->Open()) {
			can_paste = wxTheClipboard->IsSupported(wxDF_TEXT) || wxTheClipboard->IsSupported(wxDF_UNICODETEXT);
			wxTheClipboard->Close();
		}
		return can_paste;
	}

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		if (focused_text_control()) {
			if (!try_paste_lines(c))
				paste_focused_text_control();
		}
		else {
			auto pos = core.ass->iterator_to(*core.selectionController->GetActiveLine());
			paste_lines(c, false, [=](AssDialogue *new_line) -> AssDialogue * {
				core.ass->Events.insert(pos, *new_line);
				return new_line;
			});
		}
	}
};

struct edit_line_paste_over final : public Command {
	CMD_NAME("edit/line/paste/over")
	STR_MENU("Paste Lines &Over...")
	STR_DISP("Paste Lines Over")
	STR_HELP("Paste subtitles over others")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		bool can_paste = !c->GetCore().selectionController->GetSelectedSet().empty();
		if (can_paste && wxTheClipboard->Open()) {
			can_paste = wxTheClipboard->IsSupported(wxDF_TEXT) || wxTheClipboard->IsSupported(wxDF_UNICODETEXT);
			wxTheClipboard->Close();
		}
		return can_paste;
	}

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		auto ui = c->GetUI();
		auto const& sel = core.selectionController->GetSelectedSet();
		if (sel.empty())
			return;

		AssDialogue *active_line = nullptr;
		std::vector<AssDialogue *> sorted_selection;
		for (auto& line : core.ass->Events) {
			if (&line == core.selectionController->GetActiveLine())
				active_line = &line;
			if (sel.count(&line))
				sorted_selection.push_back(&line);
		}
		if (sorted_selection.empty())
			return;

		auto clipboard_lines = count_clipboard_paste_lines();
		if (!clipboard_lines)
			return;
		std::vector<bool> pasteOverOptions;

		// Only one line selected, so paste over downwards from the active line
		if (sel.size() < 2) {
			if (!active_line)
				active_line = sorted_selection.front();

			auto pos = core.ass->iterator_to(*active_line);
			auto available_lines = count_lines_until(pos, core.ass->Events.end());
			if (clipboard_lines > available_lines) {
				if (!confirm_paste_over_count_mismatch(ui.parent, fmt_tl(
					"Clipboard line count: %u\nAvailable target line count: %u\n\nContinue to paste over the available target lines and ignore the remaining clipboard content?",
					clipboard_lines, available_lines)))
					return;
			}

			paste_lines(c, true, [&](AssDialogue *new_line) -> AssDialogue * {
				if (pos == core.ass->Events.end()) return nullptr;

				AssDialogue *ret = paste_over(ui.parent, pasteOverOptions, new_line, &*pos);
				if (ret)
					++pos;
				return ret;
			});
		}
		else {
			// Multiple lines selected, so paste over the selection
			if (clipboard_lines != sorted_selection.size()) {
				if (!confirm_paste_over_count_mismatch(ui.parent, clipboard_lines > sorted_selection.size()
					? fmt_tl(
						"Clipboard line count: %u\nSelected line count: %u\n\nContinue to paste over all selected lines and ignore the remaining clipboard content?",
						clipboard_lines, sorted_selection.size())
					: fmt_tl(
						"Clipboard line count: %u\nSelected line count: %u\n\nContinue to paste over selected lines until the clipboard is exhausted and leave the rest unchanged?",
						clipboard_lines, sorted_selection.size())))
					return;
			}

			auto pos = begin(sorted_selection);
			paste_lines(c, true, [&](AssDialogue *new_line) -> AssDialogue * {
				if (pos == end(sorted_selection)) return nullptr;

				AssDialogue *ret = paste_over(ui.parent, pasteOverOptions, new_line, *pos);
				if (ret) ++pos;
				return ret;
			});
		}
	}
};

struct edit_line_recombine final : public validate_sel_multiple {
	CMD_NAME("edit/line/recombine")
	STR_MENU("Recom&bine Lines")
	STR_DISP("Recombine Lines")
	STR_HELP("Recombine subtitles which have been split and merged")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		auto const& sel_set = core.selectionController->GetSelectedSet();
		if (sel_set.size() < 2) return;

		auto active_line = core.selectionController->GetActiveLine();

		std::vector<AssDialogue*> sel(sel_set.begin(), sel_set.end());
		auto result = aegisub::subtitle_edit_ops::RecombineSelection(sel);
		for (auto *line : result.lines_to_remove)
			delete line;

		// Remove now non-existent lines from the selection
		Selection lines, new_sel;
		for (auto& line : core.ass->Events)
			lines.insert(&line);
		std::set_intersection(lines.begin(), lines.end(), sel_set.begin(), sel_set.end(), inserter(new_sel, new_sel.begin()));

		if (new_sel.empty())
			new_sel.insert(*lines.begin());

		// Restore selection
		if (!new_sel.count(active_line))
			active_line = *new_sel.begin();
		core.selectionController->SetSelectionAndActive(std::move(new_sel), active_line);

		core.ass->Commit(from_wx(_("combining")), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
	}
};

struct edit_line_split_by_karaoke final : public validate_sel_nonempty {
	CMD_NAME("edit/line/split/by_karaoke")
	STR_MENU("Split Lines (by karaoke)")
	STR_DISP("Split Lines (by karaoke)")
	STR_HELP("Use karaoke timing to split line into multiple smaller lines")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		auto sel = core.selectionController->GetSortedSelection();
		if (sel.empty()) return;

		Selection new_sel;
		AssKaraoke kara;

		std::vector<std::unique_ptr<AssDialogue>> to_delete;
		for (auto line : sel) {
			kara.SetLine(line);

			// If there aren't at least two tags there's nothing to split
			if (kara.size() < 2) continue;

			for (auto const& syl : kara) {
				auto new_line = new AssDialogue(*line);
				SubtitleGridFolding::StripMarkers(*core.ass, *new_line);

				new_line->Start = syl.start_time;
				new_line->End = syl.start_time + syl.duration;
				new_line->Text = syl.GetText(false);

				core.ass->Events.insert(core.ass->iterator_to(*line), *new_line);

				new_sel.insert(new_line);
			}

			core.ass->Events.erase(core.ass->iterator_to(*line));
			to_delete.emplace_back(line);
		}

		if (to_delete.empty()) return;

		core.ass->Commit(from_wx(_("splitting")), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);

		AssDialogue *new_active = core.selectionController->GetActiveLine();
		if (!new_sel.count(core.selectionController->GetActiveLine()))
			new_active = *new_sel.begin();
		core.selectionController->SetSelectionAndActive(std::move(new_sel), new_active);
	}
};

void split_lines(agi::Context *c, AssDialogue *&n1, AssDialogue *&n2) {
	auto core = c->GetCore();
	int pos = core.textSelectionController->GetSelectionStart();

	n1 = core.selectionController->GetActiveLine();
	n2 = new AssDialogue(*n1);
	SubtitleGridFolding::StripMarkers(*core.ass, *n2);
	core.ass->Events.insert(++core.ass->iterator_to(*n1), *n2);

	std::string orig = n1->Text;
	auto split_text = aegisub::subtitle_edit_ops::SplitTextAtPosition(orig, pos);
	n1->Text = std::move(split_text.first);
	n2->Text = std::move(split_text.second);
}

template<typename Func>
void split_lines(agi::Context *c, Func&& set_time) {
	auto core = c->GetCore();
	AssDialogue *n1, *n2;
	split_lines(c, n1, n2);
	set_time(n1, n2);

	core.ass->Commit(from_wx(_("split")), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
}

struct edit_line_split_estimate final : public validate_video_and_sel_nonempty {
	CMD_NAME("edit/line/split/estimate")
	STR_MENU("Split at cursor (estimate times)")
	STR_DISP("Split at cursor (estimate times)")
	STR_HELP("Split the current line at the cursor, dividing the original line's duration between the new ones")

	void operator()(agi::Context *c) override {
		split_lines(c, [](AssDialogue *n1, AssDialogue *n2) {
			auto split_time = aegisub::subtitle_edit_ops::EstimateSplitTime(n1->Start, n1->End, n1->Text.get(), n2->Text.get());
			if (!split_time)
				return;
			n2->Start = n1->End = *split_time;
		});
	}
};

struct edit_line_split_preserve final : public validate_sel_nonempty {
	CMD_NAME("edit/line/split/preserve")
	STR_MENU("Split at cursor (preserve times)")
	STR_DISP("Split at cursor (preserve times)")
	STR_HELP("Split the current line at the cursor, setting both lines to the original line's times")

	void operator()(agi::Context *c) override {
		split_lines(c, [](AssDialogue *, AssDialogue *) { });
	}
};

struct edit_line_split_video final : public validate_video_and_sel_nonempty {
	CMD_NAME("edit/line/split/video")
	STR_MENU("Split at cursor (at video frame)")
	STR_DISP("Split at cursor (at video frame)")
	STR_HELP("Split the current line at the cursor, dividing the line's duration at the current video frame")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		split_lines(c, [&](AssDialogue *n1, AssDialogue *n2) {
			int cur_frame = mid(
				core.videoController->FrameAtTime(n1->Start, agi::vfr::START),
				core.videoController->GetFrameN(),
				core.videoController->FrameAtTime(n1->End, agi::vfr::END));
			n1->End = n2->Start = core.videoController->TimeAtFrame(cur_frame, agi::vfr::END);
		});
	}
};

struct edit_redo final : public Command {
	CMD_NAME("edit/redo")
	CMD_ICON(redo_button)
	STR_HELP("Redo last undone action")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_DYNAMIC_NAME)

	wxString StrMenu(const agi::Context *c) const override {
		auto core = c->GetCore();
		return core.subsController->IsRedoStackEmpty() ?
			_("Nothing to &redo") :
			fmt_tl("&Redo %s", to_wx(core.subsController->GetRedoDescription()));
	}
	wxString StrDisplay(const agi::Context *c) const override {
		auto core = c->GetCore();
		return core.subsController->IsRedoStackEmpty() ?
			_("Nothing to redo") :
			fmt_tl("Redo %s", to_wx(core.subsController->GetRedoDescription()));
	}

	bool Validate(const agi::Context *c) override {
		return !c->GetCore().subsController->IsRedoStackEmpty();
	}

	void operator()(agi::Context *c) override {
		c->GetCore().subsController->Redo();
	}
};

struct edit_undo final : public Command {
	CMD_NAME("edit/undo")
	CMD_ICON(undo_button)
	STR_HELP("Undo last action")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_DYNAMIC_NAME)

	wxString StrMenu(const agi::Context *c) const override {
		auto core = c->GetCore();
		return core.subsController->IsUndoStackEmpty() ?
			_("Nothing to &undo") :
			fmt_tl("&Undo %s", to_wx(core.subsController->GetUndoDescription()));
	}
	wxString StrDisplay(const agi::Context *c) const override {
		auto core = c->GetCore();
		return core.subsController->IsUndoStackEmpty() ?
			_("Nothing to undo") :
			fmt_tl("Undo %s", to_wx(core.subsController->GetUndoDescription()));
	}

	bool Validate(const agi::Context *c) override {
		return !c->GetCore().subsController->IsUndoStackEmpty();
	}

	void operator()(agi::Context *c) override {
		c->GetCore().subsController->Undo();
	}
};

struct edit_revert final : public Command {
	CMD_NAME("edit/revert")
	STR_DISP("Revert")
	STR_MENU("Revert")
	STR_HELP("Revert the active line to its initial state (shown in the upper editor)")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		AssDialogue *line = core.selectionController->GetActiveLine();
		line->Text = core.initialLineState->GetInitialText();
		core.ass->Commit(from_wx(_("revert line")), AssFile::COMMIT_DIAG_TEXT, -1, line);
	}
};

struct edit_clear final : public Command {
	CMD_NAME("edit/clear")
	STR_DISP("Clear")
	STR_MENU("Clear")
	STR_HELP("Clear the current line's text")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		AssDialogue *line = core.selectionController->GetActiveLine();
		line->Text = "";
		core.ass->Commit(from_wx(_("clear line")), AssFile::COMMIT_DIAG_TEXT, -1, line);
	}
};

struct edit_clear_text final : public Command {
	CMD_NAME("edit/clear/text")
	STR_DISP("Clear Text")
	STR_MENU("Clear Text")
	STR_HELP("Clear the current line's text, leaving override tags")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		AssDialogue *line = core.selectionController->GetActiveLine();
		line->Text = aegisub::subtitle_edit_ops::BuildTagOnlyText(*line);
		core.ass->Commit(from_wx(_("clear line")), AssFile::COMMIT_DIAG_TEXT, -1, line);
	}
};

struct edit_insert_original final : public Command {
	CMD_NAME("edit/insert_original")
	STR_DISP("Insert Original")
	STR_MENU("Insert Original")
	STR_HELP("Insert the original line text at the cursor")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		AssDialogue *line = core.selectionController->GetActiveLine();
		int sel_start = core.textSelectionController->GetSelectionStart();
		int sel_end = core.textSelectionController->GetSelectionEnd();

		auto const& original_text = core.initialLineState->GetInitialText();
		line->Text = aegisub::subtitle_edit_ops::ReplaceRangeWithText(line->Text.get(), sel_start, sel_end, original_text);
		core.ass->Commit(from_wx(_("insert original")), AssFile::COMMIT_DIAG_TEXT, -1, line);
		core.textSelectionController->SetSelection(sel_start, sel_start + original_text.length());
	}
};

}

namespace cmd {
	void init_edit() {
		reg(agi::make_unique<edit_color_primary>());
		reg(agi::make_unique<edit_color_secondary>());
		reg(agi::make_unique<edit_color_outline>());
		reg(agi::make_unique<edit_color_shadow>());
		reg(agi::make_unique<edit_color_primary_pick_video>());
		reg(agi::make_unique<edit_color_secondary_pick_video>());
		reg(agi::make_unique<edit_color_outline_pick_video>());
		reg(agi::make_unique<edit_color_shadow_pick_video>());
		reg(agi::make_unique<edit_font>());
		reg(agi::make_unique<edit_find_replace>());
		reg(agi::make_unique<edit_line_copy>());
		reg(agi::make_unique<edit_line_cut>());
		reg(agi::make_unique<edit_line_delete>());
		reg(agi::make_unique<edit_line_duplicate>());
		reg(agi::make_unique<edit_line_duplicate_shift>());
		reg(agi::make_unique<edit_line_duplicate_shift_back>());
		reg(agi::make_unique<edit_line_join_as_karaoke>());
		reg(agi::make_unique<edit_line_join_concatenate>());
		reg(agi::make_unique<edit_line_join_with_line_breaks>());
		reg(agi::make_unique<edit_line_join_keep_first>());
		reg(agi::make_unique<edit_line_paste>());
		reg(agi::make_unique<edit_line_paste_over>());
		reg(agi::make_unique<edit_line_recombine>());
		reg(agi::make_unique<edit_line_split_by_karaoke>());
		reg(agi::make_unique<edit_line_split_estimate>());
		reg(agi::make_unique<edit_line_split_preserve>());
		reg(agi::make_unique<edit_line_split_video>());
		reg(agi::make_unique<edit_style_bold>());
		reg(agi::make_unique<edit_style_italic>());
		reg(agi::make_unique<edit_style_underline>());
		reg(agi::make_unique<edit_style_strikeout>());
		reg(agi::make_unique<edit_redo>());
		reg(agi::make_unique<edit_undo>());
		reg(agi::make_unique<edit_revert>());
		reg(agi::make_unique<edit_insert_original>());
		reg(agi::make_unique<edit_clear>());
		reg(agi::make_unique<edit_clear_text>());
	}
}
