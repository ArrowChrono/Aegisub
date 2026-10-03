// Copyright (c) 2005, Rodrigo Braz Monteiro
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

/// @file dialog_style_editor.cpp
/// @brief Style Editor dialogue box
/// @ingroup style_editor
///

#include "dialog_style_editor.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "ass_style_storage.h"
#include "colour_button.h"
#include "compat.h"
#include "font_family_catalog.h"
#include "font_family_catalog_ui.h"
#include "font_name_combo_box.h"
#include "font_variant_policy.h"
#include "font_variant_resolver.h"
#include "help_button.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "persist_location.h"
#include "selection_controller.h"
#include "subs_preview.h"
#include "utils.h"
#include "validators.h"
#include "wx_style_editor_ui_host.h"

#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <memory>

#include <wx/bmpbuttn.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include <wx/numformatter.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

namespace {
bool IsBlank(wxString text) {
	text.Trim(true);
	text.Trim(false);
	return text.empty();
}

template<typename Control, typename SpinEvent, typename ApplyDefault>
void BindEmptyDefault(Control *ctrl, wxEventTypeTag<SpinEvent> spin_event, ApplyDefault apply_default) {
	auto text_was_empty = std::make_shared<bool>(false);
	ctrl->Bind(wxEVT_TEXT, [=](wxCommandEvent &evt) {
		*text_was_empty = IsBlank(evt.GetString());
		evt.Skip();
	});
	ctrl->Bind(wxEVT_KILL_FOCUS, [=](wxFocusEvent &evt) {
		if (*text_was_empty || IsBlank(ctrl->GetTextValue())) {
			apply_default();
			*text_was_empty = false;
		}
		evt.Skip();
	});
	ctrl->Bind(spin_event, [=](SpinEvent &evt) {
		*text_was_empty = false;
		evt.Skip();
	});
}

wxArrayString GetStyleEncodingStrings() {
	wxArrayString encoding_strings;
	encoding_strings.Add(wxS("0 - ") + _("ANSI"));
	encoding_strings.Add(wxS("1 - ") + _("Default"));
	encoding_strings.Add(wxS("2 - ") + _("Symbol"));
	encoding_strings.Add(wxS("77 - ") + _("Mac"));
	encoding_strings.Add(wxS("128 - ") + _("Shift_JIS"));
	encoding_strings.Add(wxS("129 - ") + _("Hangeul"));
	encoding_strings.Add(wxS("130 - ") + _("Johab"));
	encoding_strings.Add(wxS("134 - ") + _("GB2312"));
	encoding_strings.Add(wxS("136 - ") + _("Chinese BIG5"));
	encoding_strings.Add(wxS("161 - ") + _("Greek"));
	encoding_strings.Add(wxS("162 - ") + _("Turkish"));
	encoding_strings.Add(wxS("163 - ") + _("Vietnamese"));
	encoding_strings.Add(wxS("177 - ") + _("Hebrew"));
	encoding_strings.Add(wxS("178 - ") + _("Arabic"));
	encoding_strings.Add(wxS("186 - ") + _("Baltic"));
	encoding_strings.Add(wxS("204 - ") + _("Russian"));
	encoding_strings.Add(wxS("222 - ") + _("Thai"));
	encoding_strings.Add(wxS("238 - ") + _("East European"));
	encoding_strings.Add(wxS("255 - ") + _("OEM"));
	return encoding_strings;
}

wxString FontVariantLabel(FontVariantRole role) {
	switch (role) {
		case FontVariantRole::Regular: return _("Regular");
		case FontVariantRole::Bold: return _("Bold");
		case FontVariantRole::Italic: return _("Italic");
		case FontVariantRole::BoldItalic: return _("Bold Italic");
		case FontVariantRole::Unknown: break;
	}
	return _("Unknown");
}

bool ChoiceMatches(FontVariantChoice const& choice, bool bold, bool italic) {
	return choice.weight == (bold ? 700 : 400) && choice.italic == italic;
}

#if defined(__WXGTK__)
class TrimmedDoubleSpinCtrl : public wxSpinCtrlDouble {
public:
	using wxSpinCtrlDouble::wxSpinCtrlDouble;

	bool GTKOutput(wxString *text) const override {
		if (wxSpinCtrlDouble::GTKOutput(text))
			return true;

		*text = wxNumberFormatter::ToString(GetValue(), GetDigits(),
			wxNumberFormatter::Style_WithThousandsSep | wxNumberFormatter::Style_NoTrailingZeroes);
		return true;
	}
};
#elif !defined(wxHAS_NATIVE_SPINCTRLDOUBLE)
class TrimmedDoubleSpinCtrl : public wxSpinCtrlDouble {
public:
	using wxSpinCtrlDouble::wxSpinCtrlDouble;

protected:
	wxString DoValueToText(double val) override {
		auto text = wxSpinCtrlDouble::DoValueToText(val);
		wxNumberFormatter::RemoveTrailingZeroes(text);
		return text;
	}
};
#else
using TrimmedDoubleSpinCtrl = wxSpinCtrlDouble;
#endif
}

static constexpr int    EmptyMargin = 0;  //AssStyle::DefaultMargin
static constexpr double EmptyOutlineWidth = 0.; // AssStyle::DefaultOutlineWidth
static constexpr double EmptyShadowWidth = 0.; // AssStyle::DefaultShadowWidth

/// Style rename helper that walks a file searching for a style and optionally
/// updating references to it
class StyleRenamer {
	agi::Context *c;
	bool found_any = false;
	bool do_replace = false;
	std::string source_name;
	std::string new_name;

	/// Process a single override parameter to check if it's \r with this style name
	static void ProcessTag(std::string const& tag, AssOverrideParameter* param, void *userData) {
		StyleRenamer *self = static_cast<StyleRenamer*>(userData);
		if (tag == "\\r" && param->GetType() == VariableDataType::TEXT && param->Get<std::string>() == self->source_name) {
			if (self->do_replace)
				param->Set(self->new_name);
			else
				self->found_any = true;
		}
	}

	void Walk(bool replace) {
		found_any = false;
		do_replace = replace;
		auto core = c->GetCore();

		for (auto& diag : core.ass->Events) {
			if (diag.Style == source_name) {
				if (replace)
					diag.Style = new_name;
				else
					found_any = true;
			}

			auto blocks = diag.ParseTags();
			for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
				block->ProcessParameters(&StyleRenamer::ProcessTag, this);
			if (replace)
				diag.UpdateText(blocks);

			if (found_any) return;
		}
	}

public:
	StyleRenamer(agi::Context *c, std::string source_name, std::string new_name)
	: c(c)
	, source_name(std::move(source_name))
	, new_name(std::move(new_name))
	{
	}

	/// Check if there are any uses of the original style name in the file
	bool NeedsReplace() {
		Walk(false);
		return found_any;
	}

	/// Replace all uses of the original style name with the new one
	void Replace() {
		Walk(true);
	}
};

DialogStyleEditor::DialogStyleEditor(wxWindow *parent, AssStyle *style, agi::Context *c, AssStyleStorage *store, std::string const& new_name, FontFamilySelectionModel const& font_model)
: wxDialog (parent, -1, _("Style Editor"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(c)
, style(style)
, store(store)
, font_catalog(font_model.catalog)
, prefer_localized_font_names(font_model.prefer_localized)
, compact_vertical_font_list(font_model.UsesCompactVerticalToggle())
, vertical_capable_family_ids(font_model.vertical_capable_family_ids.begin(),
	font_model.vertical_capable_family_ids.end())
, vertical_capable_bare_names(font_model.vertical_capable_bare_names)
{
	notification_sink = agi::ResolveStyleEditorNotificationSink(c, this);
	interaction_sink = agi::ResolveStyleEditorInteractionSink(c, this);

	if (new_name.size()) {
		is_new = true;
		style = this->style = new AssStyle(*style);
		style->name = new_name;
	}
	else if (!style) {
		is_new = true;
		style = this->style = new AssStyle;
	}

	work = agi::make_unique<AssStyle>(*style);

	SetIcon(GETICON(style_toolbutton_16));

	auto add_with_label = [&](wxSizer *sizer, wxString const& label, wxWindow *ctrl) {
		sizer->Add(new wxStaticText(this, -1, label), wxSizerFlags().Center().Border(wxLEFT | wxRIGHT));
		sizer->Add(ctrl, wxSizerFlags(1).Left().Expand());
	};

	auto num_text_ctrl = [&](double *value, double min, double max, double step, double default_value, bool trim_trailing_zeroes=false) -> wxSpinCtrlDouble * {
		wxSpinCtrlDouble *scd = trim_trailing_zeroes ?
			static_cast<wxSpinCtrlDouble *>(new TrimmedDoubleSpinCtrl(this, -1, wxEmptyString, wxDefaultPosition,
				wxDefaultSize, wxSP_ARROW_KEYS, min, max, *value, step)) :
			new wxSpinCtrlDouble(this, -1, wxEmptyString, wxDefaultPosition,
				wxDefaultSize, wxSP_ARROW_KEYS, min, max, *value, step);
		scd->SetDigits(1);
		scd->SetValidator(DoubleSpinValidator(value, default_value));
		BindEmptyDefault(scd, wxEVT_SPINCTRLDOUBLE, [=] {
			scd->SetValue(default_value);
			*value = default_value;
			if (!updating)
				SubsPreview->SetStyle(*work);
		});
		scd->Bind(wxEVT_SPINCTRLDOUBLE, [=](wxSpinDoubleEvent &evt) {
			evt.Skip();
			if (updating) return;

			bool old = updating;
			updating = true;
			scd->GetValidator()->TransferFromWindow();
			updating = old;
			SubsPreview->SetStyle(*work);
		});
		return scd;
	};

	// Prepare control values
	wxString EncodingValue = std::to_wstring(style->encoding);
	wxString alignValues[9] = { wxS("7"), wxS("8"), wxS("9"), wxS("4"), wxS("5"), wxS("6"), wxS("1"), wxS("2"), wxS("3") };

	// Encoding options
	wxArrayString encodingStrings = GetStyleEncodingStrings();

	// Create sizers
	wxSizer *NameSizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Style Name"));
	wxSizer *FontSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Font"));
	wxSizer *ColorsSizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Colors"));
	wxSizer *MarginSizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Margins"));
	wxSizer *OutlineBox = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Outline"));
	wxSizer *MiscBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Miscellaneous"));
	wxSizer *PreviewBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Preview"));

	// Create controls
	StyleName = new wxTextCtrl(this, -1, to_wx(style->name));
	auto const contains_matching = OPT_GET("Subtitle/Font/Use Contains Matching")->GetBool();
	auto const auto_expand = OPT_GET("Subtitle/Font/Auto Expand List On Input")->GetBool();
	FontName = new FontNameComboBox(
		this, to_wx(style->font), wxSize(150, -1), font_model.choices,
		contains_matching, auto_expand);
	FontStyle = new wxComboBox(
		this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize,
		0, nullptr, wxCB_READONLY);
	FontVariantInfo = new wxStaticText(this, -1, wxEmptyString);
	FontSize = num_text_ctrl(&work->fontsize, 0, 10000.0, 1.0, AssStyle::DefaultFontSize, true);
	BoxBold = new wxCheckBox(this, -1, _("&Bold"));
	BoxItalic = new wxCheckBox(this, -1, _("&Italic"));
	BoxUnderline = new wxCheckBox(this, -1, _("&Underline"));
	BoxStrikeout = new wxCheckBox(this, -1, _("&Strikeout"));
	if (compact_vertical_font_list) {
		BoxVertical = new wxCheckBox(this, -1, _("&Vertical"));
		BoxVertical->SetToolTip(_(
			"Write a leading '@' on the ASS font face (GDI vertical face). "
			"Enabled only when GDI registered a vertical form of this family."));
	}
	ColourButton *colorButton[] = {
		new ColourButton(this, wxSize(55, 16), true, style->primary, ColorValidator(&work->primary)),
		new ColourButton(this, wxSize(55, 16), true, style->secondary, ColorValidator(&work->secondary)),
		new ColourButton(this, wxSize(55, 16), true, style->outline, ColorValidator(&work->outline)),
		new ColourButton(this, wxSize(55, 16), true, style->shadow, ColorValidator(&work->shadow))
	};
	for (int i = 0; i < 3; i++) {
		margin[i] = new wxSpinCtrl(this, -1, std::to_wstring(style->Margin[i]),
			wxDefaultPosition, wxDefaultSize,
			wxSP_ARROW_KEYS, AssStyle::MinMargin, AssStyle::MaxMargin, style->Margin[i]);
		BindEmptyDefault(margin[i], wxEVT_SPINCTRL, [=] {
			margin[i]->SetValue(EmptyMargin);
			work->Margin[i] = EmptyMargin;
			if (!updating)
				SubsPreview->SetStyle(*work);
		});
#if wxCHECK_VERSION(3, 1, 3)
		margin[i]->SetInitialSize(margin[i]->GetSizeFromText(wxS("00000")));
#else
		margin[i]->SetInitialSize(margin[i]->GetSizeFromTextSize(GetTextExtent(wxS("00000"))));
#endif
	}

	Alignment = new wxRadioBox(this, -1, _("Alignment"), wxDefaultPosition, wxDefaultSize, 9, alignValues, 3, wxRA_SPECIFY_COLS);
	auto Outline = num_text_ctrl(&work->outline_w, 0.0, 1000.0, 0.1, EmptyOutlineWidth);
	auto Shadow = num_text_ctrl(&work->shadow_w, 0.0, 1000.0, 0.1, EmptyShadowWidth);
	OutlineType = new wxCheckBox(this, -1, _("&Opaque box"));
	auto ScaleX = num_text_ctrl(&work->scalex, 0.0, 10000.0, 0.1, AssStyle::DefaultScale);
	auto ScaleY = num_text_ctrl(&work->scaley, 0.0, 10000.0, 0.1, AssStyle::DefaultScale);
	auto Angle = num_text_ctrl(&work->angle, -360.0, 360.0, 0.1, AssStyle::DefaultAngle);
	auto Spacing = num_text_ctrl(&work->spacing, 0.0, 1000.0, 0.1, AssStyle::DefaultSpacing);
	Encoding = new wxComboBox(this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, encodingStrings, wxCB_READONLY);

	// Set control tooltips
	StyleName->SetToolTip(_("Style name"));
	FontName->SetToolTip(_("Font face"));
	FontStyle->SetToolTip(_("Font style"));
	FontSize->SetToolTip(_("Font size"));
	colorButton[0]->SetToolTip(_("Choose primary color"));
	colorButton[1]->SetToolTip(_("Choose secondary color"));
	colorButton[2]->SetToolTip(_("Choose outline color"));
	colorButton[3]->SetToolTip(_("Choose shadow color"));
	margin[0]->SetToolTip(_("Distance from left edge, in pixels"));
	margin[1]->SetToolTip(_("Distance from right edge, in pixels"));
	margin[2]->SetToolTip(_("Distance from top/bottom edge, in pixels"));
	OutlineType->SetToolTip(_("When selected, display an opaque box behind the subtitles instead of an outline around the text"));
	Outline->SetToolTip(_("Outline width, in pixels"));
	Shadow->SetToolTip(_("Shadow distance, in pixels"));
	ScaleX->SetToolTip(_("Scale X, in percentage"));
	ScaleY->SetToolTip(_("Scale Y, in percentage"));
	Angle->SetToolTip(_("Angle to rotate in Z axis, in degrees"));
	Encoding->SetToolTip(_("Encoding, only useful in unicode if the font doesn't have the proper unicode mapping"));
	Spacing->SetToolTip(_("Character spacing, in pixels"));
	Alignment->SetToolTip(_("Alignment in screen, in numpad style"));

	// Set up controls
	BoxBold->SetValue(style->bold);
	BoxItalic->SetValue(style->italic);
	BoxUnderline->SetValue(style->underline);
	BoxStrikeout->SetValue(style->strikeout);
	OutlineType->SetValue(style->borderstyle == 3);
	Alignment->SetSelection(AlignToControl(style->alignment));
	// Fill font face list box
	FontName->ChangeValue(to_wx(font_model.PreferredName(style->font)));
	committed_font_family = from_wx(FontName->GetValue());

	// Set encoding value
	bool found = false;
	for (size_t i=0;i<encodingStrings.Count();i++) {
		if (encodingStrings[i].StartsWith(EncodingValue)) {
			Encoding->Select(i);
			found = true;
			break;
		}
	}
	if (!found) Encoding->Select(0);

	// Style name sizer
	NameSizer->Add(StyleName, 1, wxALL, 0);

	// Font sizer
	wxSizer *FontSizerTop = new wxBoxSizer(wxHORIZONTAL);
	wxSizer *FontSizerBottom = new wxBoxSizer(wxHORIZONTAL);
	FontSizerTop->Add(FontName, 1, wxALL, 0);
	FontSizerTop->Add(FontStyle, 0, wxLEFT, 5);
	FontSizerTop->Add(FontSize, 0, wxLEFT, 5);
	FontSizerBottom->AddStretchSpacer(1);
	FontSizerBottom->Add(BoxBold, 0, 0, 0);
	FontSizerBottom->Add(BoxItalic, 0, wxLEFT, 5);
	FontSizerBottom->Add(BoxUnderline, 0, wxLEFT, 5);
	FontSizerBottom->Add(BoxStrikeout, 0, wxLEFT, 5);
	if (BoxVertical)
		FontSizerBottom->Add(BoxVertical, 0, wxLEFT, 5);
	FontSizerBottom->AddStretchSpacer(1);
	FontSizer->Add(FontSizerTop, 1, wxALL | wxEXPAND, 0);
	FontSizer->Add(FontSizerBottom, 1, wxTOP | wxEXPAND, 5);
	FontSizer->Add(FontVariantInfo, 0, wxTOP | wxEXPAND, 5);
	FontStyle->Hide();
	FontVariantInfo->Hide();

	// Colors sizer
	wxString colorLabels[] = { _("Primary"), _("Secondary"), _("Outline"), _("Shadow") };
	ColorsSizer->AddStretchSpacer(1);
	for (int i = 0; i < 4; ++i) {
		auto sizer = new wxBoxSizer(wxVERTICAL);
		sizer->Add(new wxStaticText(this, -1, colorLabels[i]), 0, wxBOTTOM | wxALIGN_CENTER, 5);
		sizer->Add(colorButton[i], 0, wxBOTTOM | wxALIGN_CENTER, 5);
		ColorsSizer->Add(sizer, 0, wxLEFT, i?5:0);
	}
	ColorsSizer->AddStretchSpacer(1);

	// Margins
	wxString marginLabels[] = { _("Left"), _("Right"), _("Vert") };
	MarginSizer->AddStretchSpacer(1);
	for (int i=0;i<3;i++) {
		auto sizer = new wxBoxSizer(wxVERTICAL);
		sizer->AddStretchSpacer(1);
		sizer->Add(new wxStaticText(this, -1, marginLabels[i]), 0, wxCENTER, 0);
		sizer->Add(margin[i], 0, wxTOP | wxCENTER, 5);
		sizer->AddStretchSpacer(1);
		MarginSizer->Add(sizer, 0, wxEXPAND | wxLEFT, i?5:0);
	}
	MarginSizer->AddStretchSpacer(1);

	// Margins+Alignment
	wxSizer *MarginAlign = new wxBoxSizer(wxHORIZONTAL);
	MarginAlign->Add(MarginSizer, 1, wxLEFT | wxEXPAND, 0);
	MarginAlign->Add(Alignment, 0, wxLEFT | wxEXPAND, 5);

	// Outline
	add_with_label(OutlineBox, _("Outline:"), Outline);
	add_with_label(OutlineBox, _("Shadow:"), Shadow);
	OutlineBox->Add(OutlineType, 0, wxLEFT | wxALIGN_CENTER, 5);

	// Misc
	auto MiscBoxTop = new wxFlexGridSizer(2, 4, 5, 5);
	add_with_label(MiscBoxTop, _("Scale X%:"), ScaleX);
	add_with_label(MiscBoxTop, _("Scale Y%:"), ScaleY);
	add_with_label(MiscBoxTop, _("Rotation:"), Angle);
	add_with_label(MiscBoxTop, _("Spacing:"), Spacing);

	wxSizer *MiscBoxBottom = new wxBoxSizer(wxHORIZONTAL);
	add_with_label(MiscBoxBottom, _("Encoding:"), Encoding);

	MiscBox->Add(MiscBoxTop, wxSizerFlags().Expand());
	MiscBox->Add(MiscBoxBottom, wxSizerFlags().Expand().Border(wxTOP));

	// Preview
	auto previewButton = new ColourButton(this, wxSize(45, 16), false, OPT_GET("Colour/Style Editor/Background/Preview")->GetColor());
	PreviewText = new wxTextCtrl(this, -1, to_wx(OPT_GET("Tool/Style Editor/Preview Text")->GetString()));
	SubsPreview = new SubtitlesPreview(
		this,
		wxSize(100, 60),
		wxSUNKEN_BORDER,
		OPT_GET("Colour/Style Editor/Background/Preview")->GetColor(),
		c ? c->GetCore().ass->GetTransientFonts() : std::shared_ptr<const TransientFontSet>(),
		notification_sink);

	SubsPreview->SetToolTip(_("Preview of current style"));
	SubsPreview->SetStyle(*style);
	SubsPreview->SetText(from_wx(PreviewText->GetValue()));
	PreviewText->SetToolTip(_("Text to be used for the preview"));
	previewButton->SetToolTip(_("Color of preview background"));

	wxSizer *PreviewBottomSizer = new wxBoxSizer(wxHORIZONTAL);
	PreviewBottomSizer->Add(PreviewText, 1, wxEXPAND | wxRIGHT, 5);
	PreviewBottomSizer->Add(previewButton, 0, wxEXPAND, 0);
	PreviewBox->Add(SubsPreview, 1, wxEXPAND | wxBOTTOM, 5);
	PreviewBox->Add(PreviewBottomSizer, 0, wxEXPAND | wxBOTTOM, 0);

	// Buttons
	auto ButtonSizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxAPPLY | wxHELP);

	// Left side sizer
	wxSizer *LeftSizer = new wxBoxSizer(wxVERTICAL);
	LeftSizer->Add(NameSizer, 0, wxBOTTOM | wxEXPAND, 5);
	LeftSizer->Add(FontSizer, 0, wxBOTTOM | wxEXPAND, 5);
	LeftSizer->Add(ColorsSizer, 0, wxBOTTOM | wxEXPAND, 5);
	LeftSizer->Add(MarginAlign, 0, wxBOTTOM | wxEXPAND, 0);

	// Right side sizer
	wxSizer *RightSizer = new wxBoxSizer(wxVERTICAL);
	RightSizer->Add(OutlineBox, wxSizerFlags().Expand().Border(wxBOTTOM));
	RightSizer->Add(MiscBox, wxSizerFlags().Expand().Border(wxBOTTOM));
	RightSizer->Add(PreviewBox, wxSizerFlags(1).Expand());

	// Controls Sizer
	wxSizer *ControlSizer = new wxBoxSizer(wxHORIZONTAL);
	ControlSizer->Add(LeftSizer, 0, wxEXPAND, 0);
	ControlSizer->Add(RightSizer, 1, wxLEFT | wxEXPAND, 5);

	// General Layout
	wxSizer *MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(ControlSizer, 1, wxALL | wxEXPAND, 5);
	MainSizer->Add(ButtonSizer, 0, wxBOTTOM | wxEXPAND, 5);

	SetSizerAndFit(MainSizer);

	// Force the style name text field to scroll based on its final size, rather
	// than its initial size
	StyleName->SetInsertionPoint(0);
	StyleName->SetInsertionPoint(-1);

	persist = agi::make_unique<PersistLocation>(this, "Tool/Style Editor", true);

	Bind(wxEVT_CHILD_FOCUS, &DialogStyleEditor::OnChildFocus, this);

	Bind(wxEVT_CHECKBOX, &DialogStyleEditor::OnCommandPreviewUpdate, this);
	Bind(wxEVT_COMBOBOX, &DialogStyleEditor::OnCommandPreviewUpdate, this);
	Bind(wxEVT_SPINCTRL, &DialogStyleEditor::OnCommandPreviewUpdate, this);

	previewButton->Bind(EVT_COLOR, &DialogStyleEditor::OnPreviewColourChange, this);
	FontName->Bind(wxEVT_TEXT_ENTER, &DialogStyleEditor::OnFontFamilyChanged, this);
	FontName->Bind(wxEVT_COMBOBOX, &DialogStyleEditor::OnFontFamilyChanged, this);
	FontName->Bind(wxEVT_KILL_FOCUS, &DialogStyleEditor::OnFontFamilyFocusLost, this);
	FontStyle->Bind(wxEVT_COMBOBOX, &DialogStyleEditor::OnFontVariantChanged, this);
	BoxBold->Bind(wxEVT_CHECKBOX, &DialogStyleEditor::OnFontVariantChanged, this);
	BoxItalic->Bind(wxEVT_CHECKBOX, &DialogStyleEditor::OnFontVariantChanged, this);
	if (BoxVertical) {
		BoxVertical->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
			if (updating)
				return;
			auto const current = from_wx(FontName->GetValue());
			auto bare = FontFamilyCatalog::SplitVerticalPrefix(current).second;
			auto const next = FontFamilyCatalog::JoinVerticalPrefix(
				BoxVertical->IsChecked(), bare);
			if (next != current && !bare.empty())
				FontName->ChangeValue(to_wx(next));
			CommitFontFamilyChange();
		});
		SyncVerticalControl();
	}
	PreviewText->Bind(wxEVT_TEXT, &DialogStyleEditor::OnPreviewTextChange, this);

	Bind(wxEVT_BUTTON, std::bind(&DialogStyleEditor::Apply, this, true, true), wxID_OK);
	Bind(wxEVT_BUTTON, std::bind(&DialogStyleEditor::Apply, this, true, false), wxID_APPLY);
	Bind(wxEVT_BUTTON, std::bind(&DialogStyleEditor::Apply, this, false, true), wxID_CANCEL);
	Bind(wxEVT_BUTTON, std::bind(&HelpButton::OpenPage, "Style Editor"), wxID_HELP);

	for (auto const& elem : colorButton)
		elem->Bind(EVT_COLOR, &DialogStyleEditor::OnSetColor, this);

	UpdateFontVariantControls(false);
}

DialogStyleEditor::~DialogStyleEditor() {
	if (is_new)
		delete style;
}

std::string DialogStyleEditor::GetStyleName() const {
	return style->name;
}

void DialogStyleEditor::Apply(bool apply, bool close) {
	if (apply) {
		std::string new_name = from_wx(StyleName->GetValue());
		std::replace(new_name.begin(), new_name.end(), ',', ';');

		// Get list of existing styles
		std::vector<std::string> styles = store ? store->GetNames() : c->GetCore().ass->GetStyles();

		// Check if style name is unique
		AssStyle *existing = store ? store->GetStyle(new_name) : c->GetCore().ass->GetStyle(new_name);
		if (existing && existing != style) {
			notification_sink->ShowError(
				from_wx(_("Style name conflict")),
				from_wx(_("There is already a style with this name. Please choose another name.")));
			return;
		}

		// Style name change
		bool did_rename = false;
		if (work->name != new_name) {
			if (!store && !is_new) {
				StyleRenamer renamer(c, work->name, new_name);
				if (renamer.NeedsReplace()) {
					// See if user wants to update style name through script
					auto answer = interaction_sink->Request({
						from_wx(_("Update script?")),
						from_wx(_("Do you want to change all instances of this style in the script to this new name?")),
						agi::InteractionButtons::YesNoCancel,
						agi::InteractionIcon::Question
					});

					if (answer == agi::InteractionResult::Cancel) return;

					if (answer == agi::InteractionResult::Yes) {
						did_rename = true;
						renamer.Replace();
					}
				}
			}

			work->name = new_name;
		}

		CommitFontFamilyChange();
		ApplyLiveFontVariantProbe();
		UpdateWorkStyle();
		// The freshly probed state is now the committed user intent. A later
		// family change must start from it rather than restoring an older pin.
		font_family_selection_changed = false;
		font_variant_user_modified = false;
		font_variant_implicit_pinned = false;
		font_variant_base_bold = BoxBold->GetValue();
		font_variant_base_italic = BoxItalic->GetValue();

		*style = *work;
		style->UpdateData();
		if (is_new) {
			if (store)
				store->push_back(std::unique_ptr<AssStyle>(style));
			else
				c->GetCore().ass->Styles.push_back(*style);
			is_new = false;
		}
		if (!store)
			c->GetCore().ass->Commit(from_wx(_("style change")), AssFile::COMMIT_STYLES | (did_rename ? AssFile::COMMIT_DIAG_FULL : 0));

		// Update preview
		if (!close) SubsPreview->SetStyle(*style);
	}

	if (close) {
		EndModal(apply);
		if (PreviewText)
			OPT_SET("Tool/Style Editor/Preview Text")->SetString(from_wx(PreviewText->GetValue()));
	}
}

void DialogStyleEditor::UpdateFontVariantControls(bool family_changed) {
	if (updating_font_variant)
		return;
	updating_font_variant = true;
	auto finish = [&] { updating_font_variant = false; };

	font_variant_choices.clear();
	if (family_changed && !font_variant_user_modified) {
		// A catalog profile is only a preview. Preserve the user's pre-family
		// intent so Apply can replace a stale automatic pin with a fresh probe.
		if (font_variant_implicit_pinned) {
			BoxBold->SetValue(font_variant_base_bold);
			BoxItalic->SetValue(font_variant_base_italic);
		}
		font_variant_base_bold = BoxBold->GetValue();
		font_variant_base_italic = BoxItalic->GetValue();
		font_variant_implicit_pinned = false;
	}
	auto const face = from_wx(FontName->GetValue());
	// Soft hint only: still enumerate and write names longer than 31 UTF-16 units.
	bool const exceeds_gdi_limit = FontFamilyCatalog::ExceedsGdiFaceNameLimit(face);

	auto const *record = SelectedFontRecord();
	if (!record) {
		FontStyle->Hide();
		if (exceeds_gdi_limit) {
			FontVariantInfo->SetLabel(_("Exceeds GDI face limit (31)"));
			FontVariantInfo->Show();
		} else {
			FontVariantInfo->Hide();
		}
		finish();
		Layout();
		return;
	}

	font_variant_choices = BuildVariantChoices(record->variant_profile);
	if (family_changed && !font_variant_user_modified) {
		FontVariantSelection current{
			BoxBold->GetValue() ? 700 : 400,
			BoxItalic->GetValue(),
			false,
			false};
		auto adjusted = AdjustFamilySelection(
			current, record->variant_profile, {true, false});
		if (adjusted.applied_implicit_selection) {
			BoxBold->SetValue(adjusted.selection.weight == 700);
			BoxItalic->SetValue(adjusted.selection.italic);
			font_variant_implicit_pinned = true;
		}
	}

	FontStyle->Clear();
	int selection = wxNOT_FOUND;
	for (std::size_t index = 0; index < font_variant_choices.size(); ++index) {
		FontStyle->Append(FontVariantLabel(font_variant_choices[index].role));
		if (ChoiceMatches(font_variant_choices[index], BoxBold->GetValue(), BoxItalic->GetValue()))
			selection = static_cast<int>(index);
	}
	if (selection != wxNOT_FOUND)
		FontStyle->SetSelection(selection);
	else
		FontStyle->SetSelection(wxNOT_FOUND);
	FontStyle->Show(font_variant_choices.size() > 1);

	bool has_uncertain = !record->variant_profile.automatic_pinning_reliable;
	for (auto const& outcome : record->variant_profile.outcomes) {
		if (outcome.status == FontVariantStatus::Unknown ||
		    outcome.status == FontVariantStatus::NonCanonical ||
		    outcome.status == FontVariantStatus::Synthetic) {
			has_uncertain = true;
			break;
		}
	}
	if (exceeds_gdi_limit) {
		FontVariantInfo->SetLabel(_("Exceeds GDI face limit (31)"));
		FontVariantInfo->Show();
	}
	else if (!record->variant_profile.automatic_pinning_reliable) {
		FontVariantInfo->SetLabel(_("This variant profile is report-only; automatic pinning is disabled."));
		FontVariantInfo->Show();
	}
	else if (has_uncertain) {
		FontVariantInfo->SetLabel(_("Some font variants cannot be pinned safely."));
		FontVariantInfo->Show();
	}
	else {
		FontVariantInfo->Hide();
	}
	finish();
	Layout();
}

FontFamilyRecord const* DialogStyleEditor::SelectedFontRecord() const {
	if (!font_catalog || font_catalog->empty())
		return nullptr;
	if (auto const id = FontName->SelectedFamilyId())
		return font_catalog->Find(*id);
	auto const resolved = font_catalog->Resolve(from_wx(FontName->GetValue()));
	return resolved.family ? font_catalog->Find(*resolved.family) : nullptr;
}

bool DialogStyleEditor::SupportsVerticalWriting(
	FontFamilyId family_id,
	std::string_view face_name) const {
	if (family_id != 0 && vertical_capable_family_ids.contains(family_id))
		return true;
	auto const bare = FontFamilyCatalog::SplitVerticalPrefix(face_name).second;
	if (bare.empty())
		return false;
	if (vertical_capable_bare_names.contains(std::string(bare)))
		return true;
	if (font_catalog && !font_catalog->empty()) {
		auto const resolved = font_catalog->Resolve(bare);
		return resolved.family &&
			vertical_capable_family_ids.contains(*resolved.family);
	}
	return false;
}

void DialogStyleEditor::SyncVerticalControl() {
	if (!BoxVertical)
		return;
	auto const face = from_wx(FontName->GetValue());
	bool const has_at = !FontFamilyCatalog::SplitVerticalPrefix(face).first.empty();
	FontFamilyId id = 0;
	if (auto const selected = FontName->SelectedFamilyId())
		id = *selected;
	else if (auto const *record = SelectedFontRecord())
		id = record->id;
	BoxVertical->Enable(SupportsVerticalWriting(id, face));
	updating = true;
	BoxVertical->SetValue(has_at);
	updating = false;
}

void DialogStyleEditor::CommitFontFamilyChange() {
	// Compact mode: list labels are bare; preserve the toggle only when the
	// newly selected family itself has a live GDI '@' face.
	auto current = from_wx(FontName->GetValue());
	FontFamilyId current_id = 0;
	if (auto const selected = FontName->SelectedFamilyId())
		current_id = *selected;
	else if (auto const *record = SelectedFontRecord())
		current_id = record->id;
	if (BoxVertical && BoxVertical->IsChecked() &&
	    SupportsVerticalWriting(current_id, current)) {
		auto bare = FontFamilyCatalog::SplitVerticalPrefix(current).second;
		auto const next = FontFamilyCatalog::JoinVerticalPrefix(true, bare);
		if (next != current && !bare.empty()) {
			FontName->ChangeValue(to_wx(next));
			current = next;
		}
	}
	auto const& current_family = current;
	auto const current_family_id = FontName->SelectedFamilyId();
	if (current_family != committed_font_family || current_family_id != committed_font_family_id) {
		committed_font_family = current_family;
		committed_font_family_id = current_family_id;
		font_family_selection_changed = true;
		font_variant_user_modified = false;
		UpdateFontVariantControls(true);
	}

	SyncVerticalControl();
	UpdateWorkStyle();
	SubsPreview->SetStyle(*work);
}

void DialogStyleEditor::OnFontFamilyChanged(wxCommandEvent &event) {
	CommitFontFamilyChange();
	event.Skip();
}

void DialogStyleEditor::OnFontFamilyFocusLost(wxFocusEvent &event) {
	CommitFontFamilyChange();
	event.Skip();
}

void DialogStyleEditor::OnFontVariantChanged(wxCommandEvent &event) {
	if (!updating_font_variant) {
		font_variant_user_modified = true;
		font_variant_implicit_pinned = false;
		if (event.GetEventObject() == FontStyle) {
			auto const selection = FontStyle->GetSelection();
			if (selection >= 0 && static_cast<std::size_t>(selection) < font_variant_choices.size()) {
				auto const& choice = font_variant_choices[selection];
				updating_font_variant = true;
				BoxBold->SetValue(choice.weight == 700);
				BoxItalic->SetValue(choice.italic);
				updating_font_variant = false;
			}
		}
		else {
			for (std::size_t index = 0; index < font_variant_choices.size(); ++index) {
				if (ChoiceMatches(font_variant_choices[index], BoxBold->GetValue(), BoxItalic->GetValue())) {
					FontStyle->SetSelection(static_cast<int>(index));
					break;
				}
			}
		}
	}
	event.Skip();
}

void DialogStyleEditor::ApplyLiveFontVariantProbe() {
	if (!font_family_selection_changed || font_variant_user_modified ||
	    !font_catalog || font_catalog->empty())
		return;
	auto const *record = SelectedFontRecord();
	if (!record)
		return;
	long charset = aegisub::ass::DefaultCharset;
	Encoding->GetValue().BeforeFirst('-').ToLong(&charset);
	auto resolver = CreatePlatformFontVariantResolver();
	auto live_profile = BuildFontVariantProfileForFamily(
		*resolver, *record, static_cast<int>(charset), FontSize->GetValue());
	if (!live_profile) {
		BoxBold->SetValue(font_variant_base_bold);
		BoxItalic->SetValue(font_variant_base_italic);
		font_variant_implicit_pinned = false;
		return;
	}
	auto adjusted = AdjustFamilySelection(
		{font_variant_base_bold ? 700 : 400, font_variant_base_italic, false, false},
		*live_profile,
		{true, false});
	if (adjusted.applied_implicit_selection) {
		BoxBold->SetValue(adjusted.selection.weight == 700);
		BoxItalic->SetValue(adjusted.selection.italic);
		font_variant_implicit_pinned = true;
	}
	else {
		// The immutable catalog may have become stale after a font install or
		// removal. Never commit a pin that the current backend cannot prove.
		BoxBold->SetValue(font_variant_base_bold);
		BoxItalic->SetValue(font_variant_base_italic);
		font_variant_implicit_pinned = false;
	}
}

void DialogStyleEditor::UpdateWorkStyle() {
	for (size_t i = 0; i < 3; ++i) {
		if (IsBlank(margin[i]->GetTextValue()))
			margin[i]->SetValue(EmptyMargin);
	}

	updating = true;
	TransferDataFromWindow();
	updating = false;

	work->font = from_wx(FontName->GetValue());

	long templ = 0;
	Encoding->GetValue().BeforeFirst('-').ToLong(&templ);
	work->encoding = templ;

	work->borderstyle = OutlineType->IsChecked() ? 3 : 1;

	work->alignment = ControlToAlign(Alignment->GetSelection());

	for (size_t i = 0; i < 3; ++i)
		work->Margin[i] = margin[i]->GetValue();

	work->bold = BoxBold->IsChecked();
	work->italic = BoxItalic->IsChecked();
	work->underline = BoxUnderline->IsChecked();
	work->strikeout = BoxStrikeout->IsChecked();
}

void DialogStyleEditor::OnSetColor(ValueEvent<agi::Color>&) {
	TransferDataFromWindow();
	SubsPreview->SetStyle(*work);
}

void DialogStyleEditor::OnChildFocus(wxChildFocusEvent &event) {
	UpdateWorkStyle();
	SubsPreview->SetStyle(*work);
	event.Skip();
}

void DialogStyleEditor::OnPreviewTextChange (wxCommandEvent &event) {
	SubsPreview->SetText(from_wx(PreviewText->GetValue()));
	event.Skip();
}

void DialogStyleEditor::OnPreviewColourChange(ValueEvent<agi::Color> &evt) {
	SubsPreview->SetColour(evt.Get());
	OPT_SET("Colour/Style Editor/Background/Preview")->SetColor(evt.Get());
}

void DialogStyleEditor::OnCommandPreviewUpdate(wxCommandEvent &event) {
	UpdateWorkStyle();
	SubsPreview->SetStyle(*work);
	event.Skip();
}

int DialogStyleEditor::ControlToAlign(int n) {
	switch (n) {
		case 0: return 7;
		case 1: return 8;
		case 2: return 9;
		case 3: return 4;
		case 4: return 5;
		case 5: return 6;
		case 6: return 1;
		case 7: return 2;
		case 8: return 3;
		default: return 2;
	}
}

int DialogStyleEditor::AlignToControl(int n) {
	switch (n) {
		case 7: return 0;
		case 8: return 1;
		case 9: return 2;
		case 4: return 3;
		case 5: return 4;
		case 6: return 5;
		case 1: return 6;
		case 2: return 7;
		case 3: return 8;
		default: return 7;
	}
}
