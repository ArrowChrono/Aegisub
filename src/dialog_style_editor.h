// Copyright(c) 2005, Rodrigo Braz Monteiro
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
// CONSEQUENTIAL DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "font_family_catalog.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <wx/dialog.h>

class AssStyle;
class AssStyleStorage;
struct FontVariantChoice;
struct FontFamilySelectionModel;
class FontNameComboBox;
class PersistLocation;
class SubtitlesPreview;
class wxArrayString;
class wxCheckBox;
class wxChildFocusEvent;
class wxComboBox;
class wxCommandEvent;
class wxFocusEvent;
class wxRadioBox;
class wxSpinCtrlDouble;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;
class wxThreadEvent;
class wxWindow;
namespace agi { struct Context; struct Color; }
namespace agi { class InteractionSink; class NotificationSink; }
template<typename T> class ValueEvent;

class DialogStyleEditor final : public wxDialog {
	agi::Context *c;
	std::unique_ptr<PersistLocation> persist;

	/// If true, the style was just created and so the user should not be
	/// asked if they want to change any existing lines should they rename
	/// the style
	bool is_new = false;

	bool updating = false;

	/// The style currently being edited
	AssStyle *style;

	/// Copy of style passed to the subtitles preview to avoid making changes
	/// before Apply is clicked
	std::unique_ptr<AssStyle> work;

	/// The style storage style is in, if applicable
	AssStyleStorage *store;
	std::shared_ptr<agi::NotificationSink> notification_sink;
	std::shared_ptr<agi::InteractionSink> interaction_sink;

	wxTextCtrl *StyleName;
	FontNameComboBox *FontName;
	wxComboBox *FontStyle = nullptr;
	wxStaticText *FontVariantInfo = nullptr;
	wxSpinCtrlDouble *FontSize = nullptr;
	wxCheckBox *BoxBold;
	wxCheckBox *BoxItalic;
	wxCheckBox *BoxUnderline;
	wxCheckBox *BoxStrikeout;
	wxCheckBox *BoxVertical = nullptr;
	wxSpinCtrl *margin[3];
	wxRadioBox *Alignment;
	wxCheckBox *OutlineType;
	wxComboBox *Encoding;
	wxTextCtrl *PreviewText;
	SubtitlesPreview *SubsPreview;
	std::shared_ptr<FontFamilyCatalog const> font_catalog;
	bool compact_vertical_font_list = false;
	std::unordered_set<FontFamilyId> vertical_capable_family_ids;
	std::unordered_set<std::string> vertical_capable_bare_names;
	std::vector<FontVariantChoice> font_variant_choices;
	bool updating_font_variant = false;
	bool font_family_selection_changed = false;
	bool font_variant_user_modified = false;
	bool font_variant_implicit_pinned = false;
	bool font_variant_base_bold = false;
	bool font_variant_base_italic = false;
	std::string committed_font_family;
	std::optional<std::uint32_t> committed_font_family_id;

	void SetBitmapColor(int n,wxColour color);
	int AlignToControl(int n);
	int ControlToAlign(int n);
	void UpdateWorkStyle();
	void UpdateFontVariantControls(bool family_changed);
	FontFamilyRecord const* SelectedFontRecord() const;
	bool SupportsVerticalWriting(FontFamilyId family_id, std::string_view face_name) const;
	void CommitFontFamilyChange();
	void SyncVerticalControl();
	void OnFontFamilyChanged(wxCommandEvent &event);
	void OnFontFamilyFocusLost(wxFocusEvent &event);
	void OnFontVariantChanged(wxCommandEvent &event);
	void ApplyLiveFontVariantProbe();

	void OnChildFocus(wxChildFocusEvent &event);
	void OnCommandPreviewUpdate(wxCommandEvent &event);

	void OnPreviewTextChange(wxCommandEvent &event);
	void OnPreviewColourChange(ValueEvent<agi::Color> &event);

	/// @brief Maybe apply changes and maybe close the dialog
	/// @param apply Should changes be applied?
	/// @param close Should the dialog be closed?
	void Apply(bool apply,bool close);
	/// @brief Sets color for one of the four color buttons
	void OnSetColor(ValueEvent<agi::Color>& evt);

public:
	DialogStyleEditor(wxWindow *parent, AssStyle *style, agi::Context *c, AssStyleStorage *store, std::string const& new_name, FontFamilySelectionModel const& font_model);
	~DialogStyleEditor();

	std::string GetStyleName() const;
};
