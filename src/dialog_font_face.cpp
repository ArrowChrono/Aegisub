#include "dialog_font_face.h"

#include "compat.h"
#include "font_family_catalog.h"
#include "font_family_catalog_ui.h"
#include "font_name_combo_box.h"
#include "font_variant_policy.h"
#include "font_variant_resolver.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "utils.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/font.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <libaegisub/charset_conv_win.h>
#include "gdi_font_resolver.h"
#include <wx/fontutil.h>
#endif

namespace {

wxString MatchName(FontFamilyMatchKind match) {
	switch (match) {
		case FontFamilyMatchKind::Exact: return _("Exact");
		case FontFamilyMatchKind::CaseInsensitiveExact: return _("Case-insensitive exact");
		case FontFamilyMatchKind::Ambiguous: return _("Ambiguous");
		case FontFamilyMatchKind::None: return _("Not found");
	}
	return _("Not found");
}

wxString NameKind(FontFamilyNameKind kind) {
	switch (kind) {
		case FontFamilyNameKind::Win32Family: return _("Win32 family");
		case FontFamilyNameKind::TypographicFamily: return _("Typographic family");
		case FontFamilyNameKind::FullName: return _("Full name");
		case FontFamilyNameKind::PostScript: return _("PostScript name");
		case FontFamilyNameKind::PlatformAlias: return _("Platform alias");
	}
	return _("Other name");
}

wxString VariantLabel(FontVariantRole role) {
	switch (role) {
		case FontVariantRole::Regular: return _("Regular");
		case FontVariantRole::Bold: return _("Bold");
		case FontVariantRole::Italic: return _("Italic");
		case FontVariantRole::BoldItalic: return _("Bold Italic");
		case FontVariantRole::Unknown: break;
	}
	return _("Unknown");
}

wxString VariantStatusLabel(FontVariantStatus status) {
	switch (status) {
		case FontVariantStatus::Canonical: return _("Canonical");
		case FontVariantStatus::NonCanonical: return _("Noncanonical");
		case FontVariantStatus::Synthetic: return _("Synthetic");
		case FontVariantStatus::Unknown: return _("Unknown");
	}
	return _("Unknown");
}

bool VariantMatches(FontVariantChoice const& choice, int weight, bool italic) {
	return choice.weight == weight && choice.italic == italic;
}

#ifdef _WIN32

FontFaceDialogSelection SelectionFromNativeFont(LOGFONTW const& logfont, wxWindow *parent) {
	wxFont font(wxNativeFontInfo(logfont, parent));
	if (!font.IsOk() || !logfont.lfFaceName[0])
		return {};

	FontFaceDialogSelection result;
	result.face_name = agi::charset::ConvertW(logfont.lfFaceName);
	result.point_size = font.GetPointSize();
	result.charset = logfont.lfCharSet;
	result.effective_weight = std::clamp<int>(logfont.lfWeight, 0, 1000);
	result.bold = result.effective_weight >= FW_BOLD;
	result.italic = logfont.lfItalic != 0;
	result.underline = logfont.lfUnderline != 0;
	result.has_explicit_weight = true;
	result.has_explicit_italic = true;
	result.variant_modified = true;
	result.from_native_dialog = true;
	return result;
}

FontFaceDialogSelection NormalizeNativeSelection(
	FontFaceDialogSelection selection,
	FontFamilySelectionModel const& font_model,
	int gdi_height) {
	if (!font_model.catalog || font_model.catalog->empty())
		return selection;
	auto const *record = font_model.ResolveRecord(selection.face_name);
	if (!record)
		return selection;
	GdiFontResolver resolver;
	auto const probe = resolver.Probe(
		record->localized_family_name,
		selection.effective_weight,
		selection.italic,
		selection.charset,
		gdi_height);
	if (probe.outcome.status != FontVariantStatus::Canonical)
		return selection;
	selection.face_name = font_model.PreferredName(selection.face_name);
	selection.selected_family_id = record->id;
	selection.effective_weight =
		probe.outcome.role == FontVariantRole::Bold ||
		probe.outcome.role == FontVariantRole::BoldItalic ? 700 : 400;
	selection.bold = selection.effective_weight == 700;
	selection.italic = probe.outcome.role == FontVariantRole::Italic ||
		probe.outcome.role == FontVariantRole::BoldItalic;
	return selection;
}

struct NativeFontDialogState {
	wxWindow *parent = nullptr;
	FontFamilySelectionModel const *font_model = nullptr;
	std::function<void(FontFaceDialogSelection const&)> const *on_apply = nullptr;
};

constexpr wchar_t NativeFontDialogStateProperty[] = L"AegisubFontDialogState";
constexpr int NativeFontDialogApplyButtonId = 0x0402; // psh3 in the common dialog template

UINT_PTR CALLBACK NativeFontDialogHook(
	HWND hwnd,
	UINT message,
	WPARAM wparam,
	LPARAM lparam)
{
	if (message == WM_INITDIALOG) {
		auto const *choose_font = reinterpret_cast<CHOOSEFONTW const *>(lparam);
		auto *state = reinterpret_cast<NativeFontDialogState *>(choose_font->lCustData);
		SetPropW(hwnd, NativeFontDialogStateProperty, state);
		wxString title = _("Select Font");
		SetWindowTextW(hwnd, title.wc_str());
		return 0;
	}

	auto *state = reinterpret_cast<NativeFontDialogState *>(
		GetPropW(hwnd, NativeFontDialogStateProperty));
	if (!state)
		return 0;

	if (message == WM_COMMAND
		&& LOWORD(wparam) == NativeFontDialogApplyButtonId
		&& HIWORD(wparam) == BN_CLICKED) {
		LOGFONTW logfont{};
		SendMessageW(hwnd, WM_CHOOSEFONT_GETLOGFONT, 0,
			reinterpret_cast<LPARAM>(&logfont));
		if (state->on_apply) {
			auto selection = SelectionFromNativeFont(logfont, state->parent);
			if (state->font_model)
				selection = NormalizeNativeSelection(
					std::move(selection), *state->font_model, logfont.lfHeight);
			if (!selection.face_name.empty())
				state->on_apply->operator()(selection);
		}
		return 1;
	}

	if (message == WM_DESTROY)
		RemovePropW(hwnd, NativeFontDialogStateProperty);
	return 0;
}

std::optional<FontFaceDialogSelection> ShowNativeFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilySelectionModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> const& on_apply)
{
	wxFont initial_font(
		initial.point_size,
		wxFONTFAMILY_DEFAULT,
		initial.italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
		initial.bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL,
		initial.underline,
		to_wx(initial.face_name));
	LOGFONTW logfont = initial_font.GetNativeFontInfo()->lf;
	logfont.lfWeight = std::clamp(initial.effective_weight, 0, 1000);
	logfont.lfItalic = initial.italic ? TRUE : FALSE;
	logfont.lfCharSet = static_cast<BYTE>(std::clamp(initial.charset, 0, 255));
	NativeFontDialogState state{parent, &font_model, &on_apply};

	CHOOSEFONTW choose_font{};
	choose_font.lStructSize = sizeof(choose_font);
	choose_font.hwndOwner = parent ? parent->GetHWND() : nullptr;
	choose_font.lpLogFont = &logfont;
	choose_font.lCustData = reinterpret_cast<LPARAM>(&state);
	choose_font.lpfnHook = NativeFontDialogHook;
	choose_font.Flags = CF_SCREENFONTS
		| CF_INITTOLOGFONTSTRUCT
		| CF_EFFECTS
		| CF_APPLY
		| CF_ENABLEHOOK;

	if (!ChooseFontW(&choose_font))
		return std::nullopt;

	auto result = SelectionFromNativeFont(logfont, parent);
	if (result.face_name.empty())
		return std::nullopt;
	return NormalizeNativeSelection(std::move(result), font_model, logfont.lfHeight);
}

#else

std::optional<FontFaceDialogSelection> ShowNativeFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilySelectionModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> const& on_apply);

#endif

/// Coalesce live document previews to at most one update per display frame.
constexpr int LivePreviewIntervalMs = 16;

class FontFaceDialog final : public wxDialog {
	FontFamilySelectionModel const& font_model;
	FontFaceDialogHooks hooks;
	/// Kept for the dialog's lifetime so the resolver's internal probe memo
	/// survives across previews; a per-call resolver would re-probe GDI on
	/// every keystroke.
	std::unique_ptr<FontVariantResolver> variant_resolver;
	wxTimer live_timer;
	/// Set once construction is done, so building the initial UI state cannot
	/// fire a preview.
	bool live_ready = false;
	/// A preview has been applied to the document and not yet made permanent.
	bool live_dirty = false;
	/// Guards against GetSelection()'s own control updates re-entering the
	/// debounce request.
	bool emitting_live = false;
	std::optional<FontFaceDialogSelection> sealed_selection;
	FontNameComboBox *face_name;
	wxComboBox *font_style;
	wxSpinCtrl *point_size;
	wxCheckBox *bold;
	wxCheckBox *italic;
	wxCheckBox *underline;
	wxCheckBox *vertical = nullptr;
	wxCheckBox *allow_replace_explicit;
	wxStaticText *variant_status;
	wxTextCtrl *font_information;
	std::vector<FontVariantChoice> variant_choices;
	int effective_weight = 400;
	int charset = 1;
	bool vertical_requested = false;
	bool variant_modified = false;
	bool weight_modified = false;
	bool italic_modified = false;
	bool implicit_variant_pinned = false;
	bool syncing_variant = false;
	bool initial_explicit_weight = false;
	bool initial_explicit_italic = false;
	bool face_selected = false;
	bool family_changed = false;
	int implicit_base_weight = 400;
	bool implicit_base_italic = false;
	std::string initial_face_name;
	std::string committed_family;
	std::optional<FontFamilyId> committed_family_id;

	FontFamilyRecord const* SelectedRecord() const {
		auto const selected = from_wx(face_name->GetValue());
		if (auto const id = face_name->SelectedFamilyId())
			return font_model.ResolveChoice(*id);
		if (auto const *record = font_model.ResolveRecord(selected))
			return record;
		if (!font_model.catalog)
			return nullptr;
		auto informational = font_model.catalog->ResolveInformationalName(selected);
		return informational.family
			? font_model.catalog->Find(*informational.family)
			: nullptr;
	}

	void UpdateVariantControls(bool family_changed) {
		if (syncing_variant)
			return;
		syncing_variant = true;
		auto const *record = SelectedRecord();
		variant_choices = record
			? BuildVariantChoices(record->variant_profile)
			: std::vector<FontVariantChoice>{};

		if (family_changed && !variant_modified) {
			if (implicit_variant_pinned) {
				effective_weight = implicit_base_weight;
				bold->SetValue(effective_weight == 700);
				italic->SetValue(implicit_base_italic);
			}
			implicit_base_weight = effective_weight;
			implicit_base_italic = italic->GetValue();
			implicit_variant_pinned = false;
		}

		if (record && family_changed && !variant_modified) {
			auto adjusted = AdjustFamilySelection(
				{implicit_base_weight, implicit_base_italic,
				 initial_explicit_weight, initial_explicit_italic},
				record->variant_profile,
				{true, allow_replace_explicit->GetValue()});
			if (adjusted.applied_implicit_selection) {
				effective_weight = adjusted.selection.weight;
				bold->SetValue(effective_weight == 700);
				italic->SetValue(adjusted.selection.italic);
				implicit_variant_pinned = true;
			}
		}

		font_style->Clear();
		int selection = wxNOT_FOUND;
		for (std::size_t index = 0; index < variant_choices.size(); ++index) {
			font_style->Append(VariantLabel(variant_choices[index].role));
			if (VariantMatches(variant_choices[index], effective_weight, italic->GetValue()))
				selection = static_cast<int>(index);
		}
		font_style->SetSelection(selection);
		font_style->Show(variant_choices.size() > 1);

		bool uncertain = record && !record->variant_profile.automatic_pinning_reliable;
		if (record) {
			for (auto const& outcome : record->variant_profile.outcomes) {
				if (outcome.status != FontVariantStatus::Canonical) {
					uncertain = true;
					break;
				}
			}
		}
		// GDI/VSFilter face slot is 31 UTF-16 units; still write the full name.
		if (FontFamilyCatalog::ExceedsGdiFaceNameLimit(from_wx(face_name->GetValue())))
			variant_status->SetLabel(_("Exceeds GDI face limit (31)"));
		else if (!record)
			variant_status->SetLabel(_("The selected name is not a confirmed font family alias."));
		else if (!record->variant_profile.automatic_pinning_reliable)
			variant_status->SetLabel(_("This variant profile is report-only; automatic pinning is disabled."));
		else if (uncertain)
			variant_status->SetLabel(_("Some requested variants are unknown, noncanonical, or synthetic."));
		else
			variant_status->SetLabel(wxEmptyString);
		variant_status->Show(!variant_status->GetLabel().empty());
		syncing_variant = false;
		Layout();
	}

	bool FaceIsVertical() const {
		return !FontFamilyCatalog::SplitVerticalPrefix(
			from_wx(face_name->GetValue())).first.empty();
	}

	std::string WithSelectedVerticalPrefix(std::string_view name) const {
		bool const vert = FaceIsVertical();
		auto bare = FontFamilyCatalog::SplitVerticalPrefix(name).second;
		return FontFamilyCatalog::JoinVerticalPrefix(vert, bare);
	}

	void SyncVerticalControl() {
		if (!vertical)
			return;
		auto const face = from_wx(face_name->GetValue());
		bool const has_at = FaceIsVertical();
		FontFamilyId id = 0;
		if (auto const selected = face_name->SelectedFamilyId())
			id = *selected;
		else if (auto const *record = font_model.ResolveRecord(face))
			id = record->id;
		bool const capable = font_model.SupportsVerticalWriting(id, face);
		// An exact bare-name match came from the same live GDI enumeration that
		// supplied fallback choices, so it is sufficient installation evidence
		// when the catalog is unavailable.
		vertical->Enable(capable);
		syncing_variant = true;
		vertical->SetValue(has_at || (vertical_requested && capable));
		syncing_variant = false;
	}

	void ApplyVerticalToggle(bool want_vertical) {
		vertical_requested = want_vertical;
		auto const current = from_wx(face_name->GetValue());
		auto bare = FontFamilyCatalog::SplitVerticalPrefix(current).second;
		if (bare.empty())
			return;
		auto next = FontFamilyCatalog::JoinVerticalPrefix(want_vertical, bare);
		if (next == current)
			return;
		face_name->ChangeValue(to_wx(next));
		committed_family = next;
		UpdateInformation();
		UpdatePreview();
	}

	wxString BuildFontInformation() const {
		wxString text;
		auto append = [&](wxString const& label, wxString const& value) {
			text += label;
			text += wxS(": ");
			text += value;
			text += wxS("\n");
		};

		auto const selected_wx = face_name->GetValue();
		auto const selected = from_wx(selected_wx);
		append(_("Selected ASS name"), selected_wx);

		if (!font_model.catalog || font_model.catalog->empty()) {
			append(_("Catalog match"), _("Font catalog unavailable"));
			return text;
		}

		FontFamilyResolution resolved;
		if (auto const id = face_name->SelectedFamilyId()) {
			if (font_model.ResolveChoice(*id)) {
				resolved.match = FontFamilyMatchKind::Exact;
				resolved.family = id;
			}
		}
		else {
			resolved = font_model.catalog->Resolve(selected);
		}
		append(_("Catalog match"), MatchName(resolved.match));
		if (!resolved.family)
			return text;

		auto const *record = font_model.catalog->Find(*resolved.family);
		if (!record)
			return text;

		append(_("Localized family name"),
			to_wx(WithSelectedVerticalPrefix(record->localized_family_name)));
		append(_("English Win32 family name"),
			record->english_win32_family_name.empty()
				? _("Not available")
				: to_wx(WithSelectedVerticalPrefix(record->english_win32_family_name)));
		if (effective_weight == 400 || effective_weight == 700) {
			auto const& outcome = record->variant_profile.For(effective_weight == 700, italic->GetValue());
			append(_("Variant status"), VariantStatusLabel(outcome.status));
			if (outcome.realized_weight > 0)
				append(_("Realized weight"), wxString::Format(wxS("%d"), outcome.realized_weight));
			if (outcome.role != FontVariantRole::Unknown)
				append(_("Physical style"), VariantLabel(outcome.role));
		}
		else {
			append(_("Variant status"), _("Numeric weight preserved; automatic pinning disabled"));
			append(_("Requested weight"), wxString::Format(wxS("%d"), effective_weight));
		}

		if (!record->names.empty()) {
			text += _("Known font names");
			text += wxS(":\n");
			std::set<std::pair<std::string, std::string>> seen;
			for (auto const& name : record->names) {
				auto display_name = WithSelectedVerticalPrefix(name.value);
				if (!seen.emplace(display_name, name.locale).second)
					continue;
				text += wxS("  ");
				text += to_wx(display_name);
				text += wxS(" — ");
				text += NameKind(name.kind);
				if (!name.locale.empty()) {
					text += wxS(" [");
					text += to_wx(name.locale);
					text += wxS("]");
				}
				text += wxS("\n");
			}
		}
		return text;
	}

	void UpdateInformation() {
		SyncVerticalControl();
		font_information->ChangeValue(BuildFontInformation());

		auto value = face_name->GetValue();
		value.Trim(true).Trim(false);
		if (auto *ok = FindWindow(wxID_OK))
			ok->Enable(!value.empty());
	}

	void UpdatePreview() { RequestLivePreview(); }

	/// Schedule a document preview. Every caller of UpdatePreview() is a point
	/// where the effective selection changed, so this rides along with the
	/// in-dialog preview instead of duplicating the event wiring.
	void RequestLivePreview() {
		if (!live_ready || emitting_live || !hooks.on_preview)
			return;
		if (!live_timer.IsRunning())
			live_timer.StartOnce(LivePreviewIntervalMs);
	}

	FontFaceDialogSelection ReadSelectionForCallback(bool commit_family_change) {
		emitting_live = true;
		auto selection = GetSelection(commit_family_change);
		emitting_live = false;
		return selection;
	}

	void EmitLivePreview() {
		if (!hooks.on_preview)
			return;
		// Preview reads the current edit value without committing the combo box's
		// provisional family state, so an open drop-down stays open while typing.
		auto const selection = ReadSelectionForCallback(false);
		hooks.on_preview(selection);
		live_dirty = true;
	}

	/// Apply any deferred preview immediately so the document matches the
	/// controls before the state is read or made permanent.
	void FlushLivePreview() {
		if (!live_timer.IsRunning())
			return;
		live_timer.Stop();
		EmitLivePreview();
	}

	void SealSelection() {
		FlushLivePreview();
		auto selection = ReadSelectionForCallback(true);
		if (hooks.on_commit)
			hooks.on_commit(selection);
		sealed_selection = std::move(selection);
		live_dirty = false;
	}

	void OnFaceText(wxCommandEvent &event) {
		if (vertical) {
			vertical_requested = UpdateVerticalWritingIntent(
				vertical_requested, from_wx(face_name->GetValue()),
				face_name->IsCommittingListSelection());
		}
		// Refresh soft GDI-limit / match status while typing without committing.
		UpdateVariantControls(false);
		UpdateInformation();
		UpdatePreview();
		event.Skip();
	}

	void CommitFaceFamilyChange() {
		// Compact mode: list labels are bare; preserve the toggle only when the
		// newly selected family itself has a live GDI '@' face. This avoids
		// carrying '@' from the previous family onto an unsupported selection.
		auto current = from_wx(face_name->GetValue());
		FontFamilyId resolved_family_id = 0;
		if (auto const selected = face_name->SelectedFamilyId())
			resolved_family_id = *selected;
		else if (auto const *record = font_model.ResolveRecord(current))
			resolved_family_id = record->id;
		bool const has_at = !FontFamilyCatalog::SplitVerticalPrefix(current).first.empty();
		bool const vertical_capable =
			font_model.SupportsVerticalWriting(resolved_family_id, current);
		if (vertical && vertical_requested && vertical_capable) {
			auto bare = FontFamilyCatalog::SplitVerticalPrefix(current).second;
			auto const next = FontFamilyCatalog::JoinVerticalPrefix(true, bare);
			if (next != current && !bare.empty()) {
				face_name->ChangeValue(to_wx(next));
				current = next;
			}
		}
		else if (vertical && !has_at) {
			// A bare selection without a live '@' face ends the previous intent.
			vertical_requested = false;
		}
		auto const current_id = face_name->SelectedFamilyId();
		if (current != committed_family || current_id != committed_family_id) {
			committed_family = current;
			committed_family_id = current_id;
			family_changed = true;
			variant_modified = false;
			weight_modified = false;
			italic_modified = false;
			UpdateVariantControls(true);
		}
		UpdateInformation();
		UpdatePreview();
	}

	void OnFaceUpdate(wxCommandEvent &event) {
		face_selected |= event.GetEventType() == wxEVT_COMBOBOX;
		CommitFaceFamilyChange();
		event.Skip();
	}

	void OnFaceKillFocus(wxFocusEvent &event) {
		CommitFaceFamilyChange();
		event.Skip();
	}

	void OnStyleUpdate(wxCommandEvent &event) {
		auto *source = event.GetEventObject();
		bool const changes_variant =
			source == font_style || source == bold || source == italic;
		if (!syncing_variant && changes_variant) {
			variant_modified = true;
			weight_modified |= source == font_style || source == bold;
			italic_modified |= source == font_style || source == italic;
			implicit_variant_pinned = false;
			if (source == font_style) {
				auto const selection = font_style->GetSelection();
				if (selection >= 0 && static_cast<std::size_t>(selection) < variant_choices.size()) {
					auto const& choice = variant_choices[selection];
					effective_weight = choice.weight;
					syncing_variant = true;
					bold->SetValue(choice.weight == 700);
					italic->SetValue(choice.italic);
					syncing_variant = false;
				}
			}
			else if (source == bold) {
				effective_weight = bold->GetValue() ? 700 : 400;
			}
			for (std::size_t index = 0; index < variant_choices.size(); ++index) {
				if (VariantMatches(variant_choices[index], effective_weight, italic->GetValue())) {
					font_style->SetSelection(static_cast<int>(index));
					break;
				}
			}
		}
		UpdateInformation();
		UpdatePreview();
		event.Skip();
	}

	void OnSizeUpdate(wxSpinEvent &event) {
		UpdatePreview();
		event.Skip();
	}

	void OnCopyInformation(wxCommandEvent &) {
		SetClipboard(from_wx(font_information->GetValue()));
	}

	void OnOk(wxCommandEvent &) {
		SealSelection();
		EndModal(wxID_OK);
	}

public:
	FontFaceDialog(
		wxWindow *parent,
		FontFaceDialogSelection const& initial,
		FontFamilySelectionModel const& font_model,
		FontFaceDialogHooks hooks)
	: wxDialog(parent, -1, _("Select Font"), wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, font_model(font_model)
	, hooks(std::move(hooks))
	, variant_resolver(CreatePlatformFontVariantResolver())
	, effective_weight(initial.effective_weight)
	, charset(initial.charset)
	, vertical_requested(!FontFamilyCatalog::SplitVerticalPrefix(
		initial.face_name).first.empty())
	, initial_explicit_weight(initial.has_explicit_weight)
	, initial_explicit_italic(initial.has_explicit_italic)
	, initial_face_name(initial.face_name)
	{
		if (effective_weight == 400 && initial.bold)
			effective_weight = 700;
		implicit_base_weight = effective_weight;
		implicit_base_italic = initial.italic;
		auto const contains_matching = OPT_GET("Subtitle/Font/Use Contains Matching")->GetBool();
		auto const auto_expand = OPT_GET("Subtitle/Font/Auto Expand List On Input")->GetBool();
		face_name = new FontNameComboBox(
			this, to_wx(initial.face_name), wxSize(400, -1),
			font_model.choices, contains_matching, auto_expand);
		committed_family = from_wx(face_name->GetValue());
		committed_family_id = face_name->SelectedFamilyId();
		face_name->SetToolTip(_("Font face; this exact name will be written to ASS"));
		font_style = new wxComboBox(
			this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize,
			0, nullptr, wxCB_READONLY);
		font_style->SetToolTip(_("Font style"));
		point_size = new wxSpinCtrl(
			this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize,
			wxSP_ARROW_KEYS, 0, 10000, std::clamp(initial.point_size, 0, 10000));
		point_size->SetToolTip(_("Font size"));
		bold = new wxCheckBox(this, -1, _("&Bold"));
		italic = new wxCheckBox(this, -1, _("&Italic"));
		underline = new wxCheckBox(this, -1, _("&Underline"));
		if (font_model.UsesCompactVerticalToggle()) {
			vertical = new wxCheckBox(this, -1, _("&Vertical"));
			vertical->SetToolTip(_(
				"Write a leading '@' on the ASS font face (GDI vertical face). "
				"Enabled only when GDI registered a vertical form of this family. "
				"The font text box shows the full name including '@' when on."));
		}
		allow_replace_explicit = new wxCheckBox(this, -1, _("Allow replacing explicit weight/italic"));
		variant_status = new wxStaticText(this, -1, wxEmptyString);
		bold->SetValue(initial.bold);
		italic->SetValue(initial.italic);
		underline->SetValue(initial.underline);

		auto *font_top = new wxBoxSizer(wxHORIZONTAL);
		font_top->Add(face_name, wxSizerFlags(1).Expand());
		font_top->Add(font_style, wxSizerFlags().Border(wxLEFT, 5));
		font_top->Add(point_size, wxSizerFlags().Border(wxLEFT, 5));
		auto *font_bottom = new wxBoxSizer(wxHORIZONTAL);
		font_bottom->AddStretchSpacer();
		font_bottom->Add(bold);
		font_bottom->Add(italic, wxSizerFlags().Border(wxLEFT, 5));
		font_bottom->Add(underline, wxSizerFlags().Border(wxLEFT, 5));
		if (vertical)
			font_bottom->Add(vertical, wxSizerFlags().Border(wxLEFT, 5));
		font_bottom->AddStretchSpacer();
		auto *font_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Font"));
		font_box->Add(font_top, wxSizerFlags().Expand());
		font_box->Add(font_bottom, wxSizerFlags().Expand().Border(wxTOP, 5));
		font_box->Add(allow_replace_explicit, wxSizerFlags().Border(wxTOP, 5));
		font_box->Add(variant_status, wxSizerFlags().Expand().Border(wxTOP, 5));
		font_style->Hide();
		variant_status->Hide();

		font_information = new wxTextCtrl(
			this, -1, wxEmptyString, wxDefaultPosition, wxSize(-1, 130),
			wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
		font_information->SetToolTip(_("Select text and copy it, or use Copy All"));
		auto *copy_information = new wxButton(this, -1, _("Copy All"));
		auto *information_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Selected Font Information"));
		information_box->Add(font_information, wxSizerFlags(1).Expand());
		information_box->Add(copy_information, wxSizerFlags().Right().Border(wxTOP, 5));

		auto *main_sizer = new wxBoxSizer(wxVERTICAL);
		main_sizer->Add(font_box, wxSizerFlags().Expand().Border(wxALL, 10));
		main_sizer->Add(information_box, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 10));
		main_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL),
			wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 10));
		SetSizerAndFit(main_sizer);
		SetMinSize(GetSize());
		CentreOnParent();

		face_name->Bind(wxEVT_TEXT, &FontFaceDialog::OnFaceText, this);
		face_name->Bind(wxEVT_COMBOBOX, &FontFaceDialog::OnFaceUpdate, this);
		face_name->Bind(wxEVT_TEXT_ENTER, &FontFaceDialog::OnFaceUpdate, this);
		face_name->Bind(wxEVT_KILL_FOCUS, &FontFaceDialog::OnFaceKillFocus, this);
		point_size->Bind(wxEVT_SPINCTRL, &FontFaceDialog::OnSizeUpdate, this);
		bold->Bind(wxEVT_CHECKBOX, &FontFaceDialog::OnStyleUpdate, this);
		italic->Bind(wxEVT_CHECKBOX, &FontFaceDialog::OnStyleUpdate, this);
		underline->Bind(wxEVT_CHECKBOX, &FontFaceDialog::OnStyleUpdate, this);
		if (vertical) {
			vertical->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
				if (syncing_variant)
					return;
				ApplyVerticalToggle(vertical->GetValue());
			});
		}
		font_style->Bind(wxEVT_COMBOBOX, &FontFaceDialog::OnStyleUpdate, this);
		allow_replace_explicit->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) {
			UpdateVariantControls(true);
			// The checkbox changes which tags get written, so the document
			// preview has to be refreshed even though no font control moved.
			UpdatePreview();
			event.Skip();
		});
		copy_information->Bind(wxEVT_BUTTON, &FontFaceDialog::OnCopyInformation, this);
		Bind(wxEVT_BUTTON, &FontFaceDialog::OnOk, this, wxID_OK);
		point_size->Bind(wxEVT_TEXT, [this](wxCommandEvent &event) {
			UpdatePreview();
			event.Skip();
		});
		live_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { EmitLivePreview(); });

		UpdateVariantControls(false);
		UpdateInformation();
		// Opening the dialog must not touch the document; only user edits from
		// here on schedule a live preview.
		live_ready = true;
	}

	/// Discard an unsealed preview. Called after ShowModal() returns anything
	/// other than OK, which covers Cancel, Escape and the close box in one
	/// place rather than chasing every close path.
	void CancelLivePreview() {
		live_timer.Stop();
		if (!live_dirty)
			return;
		live_dirty = false;
		if (hooks.on_revert)
			hooks.on_revert();
	}

	std::optional<FontFaceDialogSelection> const& SealedSelection() const {
		return sealed_selection;
	}

	FontFaceDialogSelection GetSelection(bool commit_family_change) {
		if (commit_family_change)
			CommitFaceFamilyChange();
		FontFaceDialogSelection result;
		result.face_name = from_wx(face_name->GetValue());
		result.selected_family_id = face_name->SelectedFamilyId();
		result.point_size = point_size->GetValue();
		result.charset = charset;
		result.effective_weight = effective_weight;
		result.bold = bold->GetValue();
		result.italic = italic->GetValue();
		result.underline = underline->GetValue();
		result.has_explicit_weight = initial_explicit_weight;
		result.has_explicit_italic = initial_explicit_italic;
		result.face_modified = face_selected || result.face_name != initial_face_name;
		result.variant_modified = variant_modified;
		result.weight_modified = weight_modified;
		result.italic_modified = italic_modified;
		result.implicit_variant_pinned = implicit_variant_pinned;
		result.allow_replace_explicit = allow_replace_explicit->GetValue();
		if (result.face_modified && font_model.catalog && !face_name->SelectedFamilyId() &&
			!font_model.ResolveRecord(result.face_name)) {
			auto informational = font_model.catalog->ResolveInformationalName(result.face_name);
			if (informational.family && informational.variant_role != FontVariantRole::Unknown) {
				auto const *record = font_model.catalog->Find(*informational.family);
				if (record) {
					result.selected_family_id = record->id;
					result.effective_weight =
						informational.variant_role == FontVariantRole::Bold ||
						informational.variant_role == FontVariantRole::BoldItalic ? 700 : 400;
					result.bold = result.effective_weight == 700;
					result.italic = informational.variant_role == FontVariantRole::Italic ||
						informational.variant_role == FontVariantRole::BoldItalic;
					result.variant_modified = true;
				}
			}
		}
		if ((family_changed || result.implicit_variant_pinned) &&
		    !result.variant_modified) {
			bool pin_confirmed = false;
			auto const *record = SelectedRecord();
			if (record && font_model.catalog && variant_resolver) {
				auto profile = BuildFontVariantProfileForFamily(
					*variant_resolver, *record, result.charset,
					static_cast<double>(result.point_size));
				if (profile) {
					auto adjusted = AdjustFamilySelection(
						{implicit_base_weight, implicit_base_italic,
						 result.has_explicit_weight, result.has_explicit_italic},
						*profile,
						{true, allow_replace_explicit->GetValue()});
					if (adjusted.applied_implicit_selection) {
						result.effective_weight = adjusted.selection.weight;
						result.bold = result.effective_weight == 700;
						result.italic = adjusted.selection.italic;
						result.implicit_variant_pinned = true;
						pin_confirmed = true;
					}
				}
			}
			if (!pin_confirmed) {
				result.effective_weight = implicit_base_weight;
				result.bold = result.effective_weight == 700;
				result.italic = implicit_base_italic;
				result.implicit_variant_pinned = false;
			}
		}
		return result;
	}

};

#ifndef _WIN32

std::optional<FontFaceDialogSelection> ShowNativeFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilySelectionModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> const& on_apply)
{
	FontFaceDialogHooks hooks;
	hooks.on_commit = [&on_apply](FontFaceDialogSelection const& selection) {
		if (on_apply)
			on_apply(selection);
	};
	FontFaceDialog dialog(parent, initial, font_model, std::move(hooks));
	if (dialog.ShowModal() != wxID_OK)
		return std::nullopt;
	return dialog.SealedSelection();
}

#endif

} // namespace

std::optional<FontFaceDialogSelection> ShowFontFaceDialog(
	wxWindow *parent,
	FontFaceDialogSelection const& initial,
	FontFamilySelectionModel const& font_model,
	FontFaceDialogHooks hooks)
{
	// Both name preferences use the custom dialog. BuildFontFamilyCatalogUiModel
	// keeps the localized-vs-English choice in the model and supplies enumerator
	// fallback choices if the catalog is not ready yet.
	FontFaceDialog dialog(parent, initial, font_model, std::move(hooks));
	auto const result = dialog.ShowModal();
	if (result != wxID_OK) {
		dialog.CancelLivePreview();
		return std::nullopt;
	}
	// OnOk already sealed the selection through the commit hook, so the caller
	// must not apply it again.
	return dialog.SealedSelection();
}
