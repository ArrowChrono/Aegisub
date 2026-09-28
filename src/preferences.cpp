// Copyright (c) 2010, Amar Takhar <verm@aegisub.org>
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

/// @file preferences.cpp
/// @brief Preferences dialogue
/// @ingroup configuration_ui

#include "preferences.h"

#include "ass_style.h"
#include "ass_style_storage.h"
#include "audio_playback_section.h"
#include "audio_provider_factory.h"
#include "audio_renderer_waveform.h"
#include "command/command.h"
#include "compat.h"
#include "discord_presence.h"
#include "help_button.h"
#include "hotkey_data_view_model.h"
#include "include/aegisub/audio_player.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/subtitles_provider.h"
#include "include/aegisub/toolbar.h"
#include "libresrc/libresrc.h"
#include "main.h"
#include "options.h"
#include "perf_trace.h"
#include "persist_location.h"
#include "preferences_base.h"
#include "video_provider_manager.h"
#include "wx_preferences_ui_host.h"

#ifdef WITH_PORTAUDIO
#include "audio_player_portaudio.h"
#endif

#include <libaegisub/hotkey.h>
#include <libaegisub/fs_fwd.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <chrono>

#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/dc.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/propgrid/advprops.h>
#include <wx/propgrid/propgrid.h>
#include <wx/settings.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/treectrl.h>
#include <wx/treebook.h>

namespace {
constexpr char const *kCommandButtonCommandsOption = "Subtitle/Edit Box/Command Buttons/Commands";

wxColour BlendColour(wxColour const& base, wxColour const& accent, int accent_percent) {
	int const base_percent = 100 - accent_percent;
	return wxColour(
		(base.Red() * base_percent + accent.Red() * accent_percent) / 100,
		(base.Green() * base_percent + accent.Green() * accent_percent) / 100,
		(base.Blue() * base_percent + accent.Blue() * accent_percent) / 100);
}

agi::fs::path FindExistingDialogDirectory(agi::fs::path path) {
	if (path.empty())
		return {};

	if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
		return path;

	if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path))
		path = path.parent_path();
	else if (!path.has_extension())
		path = path.parent_path().empty() ? path : path;
	else
		path = path.parent_path();

	while (!path.empty() && !std::filesystem::exists(path))
		path = path.parent_path();

	return path;
}

void NormalizeAudioDisplayDormantOptions() {
	auto const render_backend = OPT_GET("Audio/Display/Draw/Render Backend")->GetInt();
	if (render_backend == 2 || render_backend < 0 || render_backend > 3)
		OPT_SET("Audio/Display/Draw/Render Backend")->SetInt(0);
}

class TokenizedDirProperty final : public wxLongStringProperty {
	Preferences *prefs = nullptr;

	public:
	TokenizedDirProperty(Preferences *prefs, wxString const& label, wxString const& name, wxString const& value)
		: wxLongStringProperty(label, name, value), prefs(prefs) {}

	protected:
	bool DisplayEditorDialog(wxPropertyGrid *pg, wxVariant& value) override {
		auto const token_path = from_wx(value.GetString());
		auto const current_path = config::path
									  ? config::path->Decode(token_path)
									  : agi::fs::PathFromString(token_path);
		auto path = prefs->RequestSelectDirectory({from_wx(_("Please choose the folder:")),
												   agi::fs::PathToString(FindExistingDialogDirectory(current_path))});
		if (path.empty())
			return false;

		auto const encoded = config::path
								 ? config::path->Encode(path)
								 : agi::fs::PathToString(path);
		value = to_wx(encoded);
		return true;
	}
};

class TokenizedFileProperty final : public wxLongStringProperty {
	Preferences *prefs = nullptr;
	wxString wildcard;

	public:
	TokenizedFileProperty(Preferences *prefs, wxString const& label, wxString const& name, wxString const& value, wxString const& wildcard)
		: wxLongStringProperty(label, name, value), prefs(prefs), wildcard(wildcard) {}

	protected:
	bool DisplayEditorDialog(wxPropertyGrid *pg, wxVariant& value) override {
		auto const token_path = from_wx(value.GetString());
		auto const current_path = config::path
									  ? config::path->Decode(token_path)
									  : agi::fs::PathFromString(token_path);
		wxFileName current(current_path.wstring());
		auto const existing_dir = FindExistingDialogDirectory(current_path);
		auto path = prefs->RequestOpenFile({from_wx(_("Please choose the file:")),
											"",
											current.IsOk() ? from_wx(current.GetFullName()) : std::string(),
											"",
											from_wx(wildcard),
											agi::fs::PathToString(existing_dir),
											true});
		if (path.empty())
			return false;

		auto const encoded = config::path
								 ? config::path->Encode(path)
								 : agi::fs::PathToString(path);
		value = to_wx(encoded);
		return true;
	}
};

class PropertyGridOptionBinder {
	Preferences *prefs;
	wxPropertyGrid *grid;
	std::unordered_map<wxPGProperty *, std::function<void(wxVariant const&)>> updaters;

	template <typename OptionValue, typename Value>
	void QueueOptionChange(std::string const& name, Value value) {
		prefs->SetOption(agi::make_unique<OptionValue>(name, std::move(value)));
	}

	wxPGChoices MakeChoices(wxArrayString const& choices) const {
		wxPGChoices pg_choices;
		for (unsigned i = 0; i < choices.size(); ++i)
			pg_choices.Add(choices[i], i);
		return pg_choices;
	}

	int ClampChoiceSelection(int selected, size_t count) const {
		return count ? std::clamp<int>(selected, 0, static_cast<int>(count) - 1) : 0;
	}

	void ApplyTheme(wxWindow *page) {
		auto const window = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
		auto const window_text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
		auto const button = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
		auto const button_text = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
		auto const highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
		auto const highlight_text = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
		auto const gray_text = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

		grid->SetBackgroundColour(window);
		grid->SetEmptySpaceColour(window);
		grid->SetCellBackgroundColour(window);
		grid->SetCellTextColour(window_text);
		grid->SetCellDisabledTextColour(gray_text);
		grid->SetMarginColour(BlendColour(button, window, 35));
		grid->SetLineColour(BlendColour(button, window_text, 12));
		grid->SetCaptionBackgroundColour(BlendColour(button, highlight, 10));
		grid->SetCaptionTextColour(button_text);
		grid->SetSelectionBackgroundColour(highlight);
		grid->SetSelectionTextColour(highlight_text);
		grid->SetVerticalSpacing(page->FromDIP(2));
	}

	public:
	explicit PropertyGridOptionBinder(OptionPage *page)
		: prefs(page->parent) {
		static bool editors_registered = false;
		if (!editors_registered) {
			wxPropertyGrid::RegisterAdditionalEditors();
			editors_registered = true;
		}

		grid = new wxPropertyGrid(
			page,
			wxID_ANY,
			wxDefaultPosition,
			wxDefaultSize,
			wxPG_BOLD_MODIFIED | wxPG_SPLITTER_AUTO_CENTER | wxPG_TOOLTIPS);
		grid->SetExtraStyle(wxPG_EX_HELP_AS_TOOLTIPS);
		grid->SetMinSize(page->FromDIP(wxSize(520, 360)));
		ApplyTheme(page);
	}

	void BindEvents(std::shared_ptr<PropertyGridOptionBinder> self) {
		grid->Bind(wxEVT_PG_CHANGED, [self = std::move(self)](wxPropertyGridEvent& evt) {
			auto it = self->updaters.find(evt.GetProperty());
			if (it != self->updaters.end())
				it->second(evt.GetPropertyValue());
			evt.Skip();
		});
	}

	wxPropertyGrid *GetGrid() const { return grid; }

	wxPGProperty *AddCategory(wxString const& label) {
		return grid->Append(new wxPropertyCategory(label));
	}

	wxPGProperty *AddBool(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxBoolProperty(label, to_wx(opt_name), opt->GetBool()));
		prop->SetAttribute(wxPG_BOOL_USE_CHECKBOX, true);
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueBool>(name, value.GetBool());
		});
		return prop;
	}

	wxPGProperty *AddInt(wxString const& label, const char *opt_name, int min, int max) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxIntProperty(label, to_wx(opt_name), opt->GetInt()));
		prop->SetAttribute(wxPG_ATTR_MIN, static_cast<long>(min));
		prop->SetAttribute(wxPG_ATTR_MAX, static_cast<long>(max));
		prop->SetAttribute(wxPG_ATTR_SPINCTRL_STEP, 1L);
		prop->SetEditor(wxS("SpinCtrl"));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueInt>(name, static_cast<int>(value.GetLong()));
		});
		return prop;
	}

	wxPGProperty *AddDouble(wxString const& label, const char *opt_name, double min, double max, double step, int precision = 2) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxFloatProperty(label, to_wx(opt_name), opt->GetDouble()));
		prop->SetAttribute(wxPG_ATTR_MIN, min);
		prop->SetAttribute(wxPG_ATTR_MAX, max);
		prop->SetAttribute(wxPG_ATTR_SPINCTRL_STEP, step);
		prop->SetAttribute(wxPG_FLOAT_PRECISION, precision);
		prop->SetEditor(wxS("SpinCtrl"));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueDouble>(name, value.GetDouble());
		});
		return prop;
	}

	wxPGProperty *AddString(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxStringProperty(label, to_wx(opt_name), to_wx(opt->GetString())));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueString>(name, from_wx(value.GetString()));
		});
		return prop;
	}

	wxPGProperty *AddFont(wxString const& label, std::string const& opt_prefix) {
		auto const face_name = opt_prefix + "Font Face";
		auto const font_size = opt_prefix + "Font Size";
		prefs->AddChangeableOption(face_name);
		prefs->AddChangeableOption(font_size);

		wxFont font;
		auto const face_opt = OPT_GET(face_name);
		auto const size_opt = OPT_GET(font_size);
		if (!face_opt->GetString().empty())
			font.SetFaceName(to_wx(face_opt->GetString()));
		if (size_opt->GetInt() > 0)
			font.SetPointSize(static_cast<int>(size_opt->GetInt()));

		auto *prop = grid->Append(new wxFontProperty(label, to_wx(opt_prefix), font));
		updaters.emplace(prop, [this, face_name, font_size](wxVariant const& value) {
			wxFont font;
			font << value;
			QueueOptionChange<agi::OptionValueString>(face_name, from_wx(font.GetFaceName()));
			QueueOptionChange<agi::OptionValueInt>(font_size, font.GetPointSize());
		});
		return prop;
	}

	wxPGProperty *AddDirectory(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new TokenizedDirProperty(prefs, label, to_wx(opt_name), to_wx(opt->GetString())));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueString>(name, from_wx(value.GetString()));
		});
		return prop;
	}

	wxPGProperty *AddColour(wxString const& label, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new wxColourProperty(label, to_wx(opt_name), to_wx(opt->GetColor())));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			wxColour colour;
			colour << value;
			QueueOptionChange<agi::OptionValueColor>(name, from_wx(colour));
		});
		return prop;
	}

	wxPGProperty *AddFile(wxString const& label, const char *opt_name, wxString const& wildcard) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		auto *prop = grid->Append(new TokenizedFileProperty(prefs, label, to_wx(opt_name), to_wx(opt->GetString()), wildcard));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueString>(name, from_wx(value.GetString()));
		});
		return prop;
	}

	wxPGProperty *AddChoice(wxString const& label, wxArrayString const& choices, const char *opt_name) {
		auto opt = OPT_GET(opt_name);
		if (opt->GetType() == agi::OptionType::String) {
			std::vector<std::pair<std::string, std::string>> mapped_choices;
			mapped_choices.reserve(choices.size());
			for (auto const& choice : choices)
				mapped_choices.emplace_back(from_wx(choice), from_wx(choice));
			return AddChoice(label, mapped_choices, opt_name);
		}

		prefs->AddChangeableOption(opt_name);
		int const selected = ClampChoiceSelection(opt->GetInt(), choices.size());
		auto pg_choices = MakeChoices(choices);
		auto *prop = grid->Append(new wxEnumProperty(label, to_wx(opt_name), pg_choices, selected));
		std::string name = opt_name;
		updaters.emplace(prop, [this, name](wxVariant const& value) {
			QueueOptionChange<agi::OptionValueInt>(name, static_cast<int>(value.GetLong()));
		});
		return prop;
	}

	wxPGProperty *AddChoice(wxString const& label, std::vector<std::pair<std::string, std::string>> const& choices, const char *opt_name) {
		prefs->AddChangeableOption(opt_name);
		auto opt = OPT_GET(opt_name);
		wxPGChoices pg_choices;
		int selected = 0;

		for (unsigned i = 0; i < choices.size(); ++i) {
			pg_choices.Add(to_wx(choices[i].first), i);
			if (opt->GetType() == agi::OptionType::String && choices[i].second == opt->GetString())
				selected = i;
		}

		if (opt->GetType() == agi::OptionType::Int)
			selected = ClampChoiceSelection(opt->GetInt(), choices.size());

		auto *prop = grid->Append(new wxEnumProperty(label, to_wx(opt_name), pg_choices, selected));
		if (opt->GetType() == agi::OptionType::Int) {
			std::string name = opt_name;
			updaters.emplace(prop, [this, name](wxVariant const& value) {
				QueueOptionChange<agi::OptionValueInt>(name, static_cast<int>(value.GetLong()));
			});
			return prop;
		}

		std::string name = opt_name;
		std::vector<std::string> values;
		values.reserve(choices.size());
		for (auto const& choice : choices)
			values.push_back(choice.second);
		updaters.emplace(prop, [this, name, values = std::move(values)](wxVariant const& value) {
			int const index = static_cast<int>(value.GetLong());
			if (index < 0 || index >= static_cast<int>(values.size()))
				return;
			QueueOptionChange<agi::OptionValueString>(name, values[index]);
		});
		return prop;
	}
};

/// General preferences page
void BuildGeneralPage(OptionPage *p) {
	auto general = p->PageSizer(_("General"));
	p->OptionAdd(general, _("Check for updates on startup"), "App/Auto/Check For Updates");
	p->OptionAdd(general, _("Show main toolbar"), "App/Show Toolbar");
	p->OptionAdd(general, _("Save UI state in subtitles files"), "App/Save UI State");
	p->CellSkip(general);

	p->OptionAdd(general, _("Toolbar Icon Size"), "App/Toolbar Icon Size");
	wxString autoload_modes[] = {_("Never"), _("Always"), _("Ask")};
	wxArrayString autoload_modes_arr(3, autoload_modes);
	p->OptionChoice(general, _("Automatically load linked files"), autoload_modes_arr, "App/Auto/Load Linked Files");
	p->OptionAdd(general, _("Undo Levels"), "Limits/Undo Levels", 2, 10000);

	auto recent = p->PageSizer(_("Recently Used Lists"));
	p->OptionAdd(recent, _("Files"), "Limits/MRU", 0, 16);
	p->OptionAdd(recent, _("Find/Replace"), "Limits/Find Replace");

	auto font_names = p->PageSizer(_("ASS Font Names"));
#ifdef _WIN32
	auto *prefer_localized = p->OptionAdd(
		font_names,
		_("Prefer localized font family names"),
		"Subtitle/Font/Prefer Localized Family Names");
	prefer_localized->SetToolTip(_(
		"When enabled, Style Editor and \\fn selectors display and write the "
		"system-localized family name. Both Style Editor and the \\fn Select Font "
		"command use Aegisub's custom font selectors. When disabled, both prefer "
		"validated English Win32 family "
		"names, falling back when unavailable (localized catalog name, or the "
		"enumerator list if the catalog is not ready), for better "
		"cross-language portability."));
#endif
	auto *contains_matching = p->OptionAdd(
		font_names,
		_("Use contains matching in custom font selectors"),
		"Subtitle/Font/Use Contains Matching");
	contains_matching->SetToolTip(_(
		"When enabled, typing in Style Editor or the custom Select Font dialog "
		"uses contains matching instead of the native prefix matching. Other "
		"combo behaviour is unchanged."));
	auto *auto_expand = p->OptionAdd(
		font_names,
		_("Automatically expand font list while typing"),
		"Subtitle/Font/Auto Expand List On Input");
	auto_expand->SetToolTip(_(
		"When enabled, typing a matching name in Style Editor or the custom "
		"Select Font dialog opens the candidate list automatically. Use the "
		"arrow keys and Enter to choose a font without the mouse. Manual font "
		"names remain supported."));
#ifdef _WIN32
	auto *compact_vertical = p->OptionAdd(
		font_names,
		_("Compact vertical font list (use Vertical checkbox)"),
		"Subtitle/Font/Compact Vertical Font List");
	compact_vertical->SetToolTip(_(
		"Default is off: custom font lists include both horizontal and GDI "
		"'@' vertical face names, similar to the Windows font list.\n\n"
		"When enabled, lists show only horizontal family names. A Vertical "
		"checkbox appears for families GDI registered with a leading '@'; "
		"the face text box still shows the full ASS name (with '@' when "
		"vertical is on). Uninstalled fonts keep the typed name; Vertical is "
		"disabled when capability cannot be verified."));
#endif

	p->SetSizerAndFit(p->sizer);
}

#ifdef WITH_DISCORD_PRESENCE
void BuildDiscordPage(OptionPage *p) {
	auto connection = p->PageSizer(_("Connection"));
	p->OptionAdd(connection, _("Connect to Discord"), "Discord/Enabled");
	p->CellSkip(connection);
	p->OptionAdd(connection, _("Application ID"), "Discord/Application ID");
	auto display = p->PageSizer(_("Display"));
	p->OptionAdd(display, _("Application display name"), "Discord/Application Name");
	p->OptionAdd(display, _("Large image asset key"), "Discord/Large Image Key");

	auto *status = new wxStaticText(p, wxID_ANY, wxEmptyString);
	status->SetMinSize(p->FromDIP(wxSize(400, 40)));
	p->sizer->Add(status, 0, wxEXPAND | wxALL, p->FromDIP(5));
	auto update_status = [status] {
		auto *presence = wxGetApp().GetDiscordPresence();
		status->SetLabel(presence ? wxGetTranslation(to_wx(presence->GetStatus())) : _("Disconnected"));
	};
	update_status();
	auto timer = std::make_shared<wxTimer>(p);
	p->Bind(wxEVT_TIMER, [timer, update_status](wxTimerEvent&) { update_status(); }, timer->GetId());
	timer->Start(1000);
	p->SetSizerAndFit(p->sizer);
}
#endif

void BuildGeneralDefaultStylesPage(OptionPage *p) {
	auto staticbox = new wxStaticBoxSizer(wxVERTICAL, p, _("Default style catalogs"));
	p->sizer->Add(staticbox, 0, wxEXPAND, 5);
	p->sizer->AddSpacer(8);

	auto instructions = new wxStaticText(p, wxID_ANY, _("The chosen style catalogs will be loaded when you start a new file or import files in the various formats.\n\nYou can set up style catalogs in the Style Manager."));
	p->sizer->Fit(p);
	instructions->Wrap(400);
	staticbox->Add(instructions, 0, wxALL, 5);
	staticbox->AddSpacer(16);

	auto general = new wxFlexGridSizer(2, 5, 5);
	general->AddGrowableCol(0, 1);
	staticbox->Add(general, 1, wxEXPAND, 5);

	// Build a list of available style catalogs, and wished-available ones
	auto const& avail_catalogs = AssStyleStorage::GetCatalogs();
	std::unordered_set<std::string> catalogs_set(begin(avail_catalogs), end(avail_catalogs));
	// Always include one named "Default" even if it doesn't exist (ensure there is at least one on the list)
	catalogs_set.insert("Default");
	// Include all catalogs named in the existing configuration
	static const char *formats[] = {"ASS", "MicroDVD", "SRT", "TTXT", "TXT"};
	for (auto formatname : formats)
		catalogs_set.insert(OPT_GET("Subtitle Format/" + std::string(formatname) + "/Default Style Catalog")->GetString());
	// Sorted version
	wxArrayString catalogs;
	for (auto const& cn : catalogs_set)
		catalogs.Add(to_wx(cn));
	catalogs.Sort();

	p->OptionChoice(general, _("New files"), catalogs, "Subtitle Format/ASS/Default Style Catalog");
	p->OptionChoice(general, _("MicroDVD import"), catalogs, "Subtitle Format/MicroDVD/Default Style Catalog");
	p->OptionChoice(general, _("SRT import"), catalogs, "Subtitle Format/SRT/Default Style Catalog");
	p->OptionChoice(general, _("TTXT import"), catalogs, "Subtitle Format/TTXT/Default Style Catalog");
	p->OptionChoice(general, _("Plain text import"), catalogs, "Subtitle Format/TXT/Default Style Catalog");

	p->SetSizerAndFit(p->sizer);
}

/// Audio preferences page
void BuildAudioPage(OptionPage *p) {
	NormalizeAudioDisplayDormantOptions();

	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Options"));
#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
	auto *skia_audio = binder->AddBool(
		_("Use Skia Audio Display (restart required)"),
		"Audio/Display/Skia/Enabled");
	skia_audio->SetHelpString(_(
		"Use the experimental Skia renderer for the complete audio display. "
		"The legacy renderer remains the default. Restart Aegisub after changing this option."));
#endif
	binder->AddBool(_("Default mouse wheel to zoom"), "Audio/Wheel Default to Zoom");
	binder->AddBool(_("Lock scroll on cursor"), "Audio/Lock Scroll on Cursor");
	binder->AddBool(_("Snap markers by default"), "Audio/Snap/Enable");
	binder->AddBool(_("Auto-focus on mouse over"), "Audio/Auto/Focus");
	binder->AddBool(_("Play audio when stepping in video"), "Audio/Plays When Stepping Video");
	binder->AddBool(_("Left-click-drag moves end marker"), "Audio/Drag Timing");
	binder->AddInt(_("Default timing length (ms)"), "Timing/Default Duration", 0, 36000);
	binder->AddInt(_("Default lead-in length (ms)"), "Audio/Lead/IN", 0, 36000);
	binder->AddInt(_("Default lead-out length (ms)"), "Audio/Lead/OUT", 0, 36000);
	binder->AddInt(
		_("Playback length before selection (ms)"),
		aegisub::audio_playback_section::BeforeOption,
		0,
		aegisub::audio_playback_section::MaximumDurationMs);
	binder->AddInt(
		_("Playback length after selection (ms)"),
		aegisub::audio_playback_section::AfterOption,
		0,
		aegisub::audio_playback_section::MaximumDurationMs);
	binder->AddInt(
		_("Playback length at selection start (ms)"),
		aegisub::audio_playback_section::BeginOption,
		0,
		aegisub::audio_playback_section::MaximumDurationMs);
	binder->AddInt(
		_("Playback length at selection end (ms)"),
		aegisub::audio_playback_section::EndOption,
		0,
		aegisub::audio_playback_section::MaximumDurationMs);

	binder->AddInt(_("Marker drag-start sensitivity (px)"), "Audio/Start Drag Sensitivity", 1, 15);
	auto *drag_dead_zone = binder->AddInt(
		_("Marker drag dead zone (px)"), "Audio/Drag Dead Zone", 0, 50);
	drag_dead_zone->SetHelpString(_(
		"Horizontal mouse movement ignored before marker dragging begins. "
		"Set to 0 to require any horizontal movement."));
	binder->AddInt(_("Line boundary thickness (px)"), "Audio/Line Boundaries Thickness", 1, 5);
	binder->AddInt(_("Maximum snap distance (px)"), "Audio/Snap/Distance", 0, 25);

	const wxString dtl_arr[] = {_("Don't show"), _("Show previous"), _("Show previous and next"), _("Show all")};
	wxArrayString choice_dtl(4, dtl_arr);
	binder->AddChoice(_("Show inactive lines"), choice_dtl, "Audio/Inactive Lines Display Mode");
	binder->AddBool(_("Include commented inactive lines"), "Audio/Display/Draw/Inactive Comments");

	binder->AddCategory(_("Display Visual Options"));
	binder->AddBool(_("Keyframes in dialogue mode"), "Audio/Display/Draw/Keyframes in Dialogue Mode");
	binder->AddBool(_("Keyframes in karaoke mode"), "Audio/Display/Draw/Keyframes in Karaoke Mode");
	binder->AddBool(_("Cursor time"), "Audio/Display/Draw/Cursor Time");
	binder->AddBool(_("Video position"), "Audio/Display/Draw/Video Position");
	binder->AddBool(_("Seconds boundaries"), "Audio/Display/Draw/Seconds");
	binder->AddChoice(_("Waveform Style"), AudioWaveformRenderer::GetWaveformStyles(), "Audio/Display/Waveform Style");

	const wxString sq_arr[4] = {_("Regular quality"), _("Better quality"), _("High quality"), _("Insane quality")};
	wxArrayString sq_choice(4, sq_arr);
	binder->AddChoice(_("Spectrum Quality"), sq_choice, "Audio/Renderer/Spectrum/Quality");

	const wxString sif_arr[2] = {_("Provider s16 mono"), _("Float32 per-channel")};
	wxArrayString sif_choice(2, sif_arr);
	binder->AddChoice(_("Spectrum Input Format"), sif_choice, "Audio/Renderer/Spectrum/Input Format");

	const wxString scm_arr[2] = {_("Legacy linear"), _("Frequency curve")};
	wxArrayString scm_choice(2, scm_arr);
	binder->AddChoice(_("Spectrum Computation Mode"), scm_choice, "Audio/Renderer/Spectrum/Computation Mode");

	const wxString smm_arr[3] = {
		_("Time-domain downmix"),
		_("Strongest channel per frequency bin"),
		_("Average channel energy per frequency bin")};
	wxArrayString smm_choice(3, smm_arr);
	binder->AddChoice(_("Spectrum Mono Mix Method"), smm_choice, "Audio/Renderer/Spectrum/Mono Mix Mode");

	const wxString sc_arr[5] = {_("Linear"), _("Extended"), _("Medium"), _("Compressed"), _("Logarithmic")};
	wxArrayString sc_choice(5, sc_arr);
	binder->AddChoice(_("Spectrum Frequency Mapping"), sc_choice, "Audio/Renderer/Spectrum/FreqCurve");

	binder->AddCategory(_("Audio overlay text"));
	binder->AddFont(_("Overlay labels and cursor font"), "Audio/Karaoke/");

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Video preferences page
void BuildVideoPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Options"));
	binder->AddBool(_("Show keyframes in slider"), "Video/Slider/Show Keyframes");
	binder->AddBool(_("Seek video to line start on selection change"), "Video/Subtitle Sync");
	binder->AddBool(_("Automatically open audio when opening video"), "Video/Open Audio");

	const wxString cscroll_arr[] = {
		_("Resizes the video box"),
		_("Resizes the video box (reversed)"),
		_("Zooms the video"),
		_("Zooms the video (reversed)"),
		_("Pans the video"),
		_("Pans the video (X/Y swapped)"),
		_("Does nothing")};
	wxArrayString choice_scroll(7, cscroll_arr);
	binder->AddChoice(_("Scrolling on the video display"), choice_scroll, "Video/Scroll Action");
	binder->AddChoice(_("Ctrl+Scrolling on the video display"), choice_scroll, "Video/Ctrl Scroll Action");
	binder->AddChoice(_("Shift+Scrolling on the video display"), choice_scroll, "Video/Shift Scroll Action");

	const wxString czoom_arr[24] = {
		wxS("12.5%"), wxS("25%"), wxS("37.5%"), wxS("50%"), wxS("62.5%"), wxS("75%"),
		wxS("87.5%"), wxS("100%"), wxS("112.5%"), wxS("125%"), wxS("137.5%"), wxS("150%"),
		wxS("162.5%"), wxS("175%"), wxS("187.5%"), wxS("200%"), wxS("212.5%"), wxS("225%"),
		wxS("237.5%"), wxS("250%"), wxS("262.5%"), wxS("275%"), wxS("287.5%"), wxS("300%")};
	wxArrayString choice_zoom(24, czoom_arr);
	binder->AddChoice(_("Default Zoom"), choice_zoom, "Video/Default Zoom");

	binder->AddInt(_("Fast jump step in frames"), "Video/Slider/Fast Jump Step", 0, INT_MAX);

	const wxString cscr_arr[3] = {wxS("?video"), wxS("?script"), wxS(".")};
	wxArrayString scr_res(3, cscr_arr);
	binder->AddChoice(_("Screenshot save path"), scr_res, "Path/Screenshot");

	binder->AddCategory(_("Script Resolution"));
	auto *auto_prop = binder->AddBool(_("Use resolution of first video opened"), "Subtitle/Default Resolution/Auto");
	auto *width_prop = binder->AddInt(_("Default width"), "Subtitle/Default Resolution/Width", 0, INT_MAX);
	auto *height_prop = binder->AddInt(_("Default height"), "Subtitle/Default Resolution/Height", 0, INT_MAX);
	auto update_resolution_enable = [grid, width_prop, height_prop]() {
		bool const auto_enabled = OPT_GET("Subtitle/Default Resolution/Auto")->GetBool();
		grid->EnableProperty(width_prop, !auto_enabled);
		grid->EnableProperty(height_prop, !auto_enabled);
	};
	update_resolution_enable();
	grid->Bind(wxEVT_PG_CHANGED, [auto_prop, update_resolution_enable](wxPropertyGridEvent& evt) {
		if (evt.GetProperty() == auto_prop)
			update_resolution_enable();
		evt.Skip();
	});

	const wxString cres_arr[] = {_("Never"), _("Ask"), _("Always set"), _("Always resample")};
	wxArrayString choice_res(4, cres_arr);
	binder->AddChoice(_("Match video resolution on open"), choice_res, "Video/Script Resolution Mismatch");

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Visual tools preferences page
void BuildVisualToolsPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Options"));
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	auto *skia_video_tools = binder->AddBool(
		_("Use Skia video tools (restart required)"),
		"Video/Skia Tools/Enabled");
	skia_video_tools->SetHelpString(_(
		"Use Skia for interactive video tool overlays. Video frame rendering and subtitles "
		"continue to use their existing renderers. Restart Aegisub after changing this option."));
#endif
	binder->AddBool(_("Only show visual tools when mouse is over video"), "Tool/Visual/Autohide");
	binder->AddInt(_("Coordinate font size"), "Tool/Visual/Coordinate Font Size", 6, 72);

	binder->AddCategory(_("Perspective"));
	auto *fit_text = binder->AddBool(_("Fit Text"), "Tool/Visual/Perspective/Fit Text");
	fit_text->SetHelpString(_(
		"Scale text to fit the target quadrilateral"));
	auto *fax_frz_only = binder->AddBool(
		_("Fax + Frz Only"),
		"Tool/Visual/Perspective/Fax Frz Only");
	fax_frz_only->SetHelpString(_(
		"Allow position and scale changes, but restrict Perspective to fax and frz"));
	auto *perspective_decimals = binder->AddInt(
		_("Decimal places"),
		"Tool/Visual/Perspective/Decimal Places",
		0, 6);
	perspective_decimals->SetHelpString(_(
		"Maximum digits after the decimal point in generated Perspective tags. "
		"The default is 4. Use 0 for integers. Tags still use the shortest "
		"digits that fit, so lowering this only trades precision."));
	auto *perspective_shape_tolerance = binder->AddDouble(
		_("Shape tolerance (pixels)"),
		"Tool/Visual/Perspective/Shape Tolerance", 0.001, 100, 0.1, 3);
	perspective_shape_tolerance->SetHelpString(_(
		"Allow simpler tags when the shape differs by at most this many output pixels. "
		"Fit Text still controls scaling; anchored edges and decimal precision keep their own limits."));
	binder->AddCategory(_("Nudge"));
	binder->AddDouble(_("Rotate step (degrees)"), "Tool/Visual/Nudge/Rotate Step", 0.01, 180, 0.1, 2);
	binder->AddDouble(_("Rotate large step (degrees)"), "Tool/Visual/Nudge/Rotate Step Large", 0.01, 180, 0.1, 2);
	binder->AddDouble(_("Scale step (percent)"), "Tool/Visual/Nudge/Scale Step", 0.01, 100, 0.1, 2);
	binder->AddDouble(_("Scale large step (percent)"), "Tool/Visual/Nudge/Scale Step Large", 0.01, 100, 0.1, 2);
	binder->AddDouble(_("Origin step (pixels)"), "Tool/Visual/Nudge/Origin Step", 0.01, 1000, 0.1, 2);
	binder->AddDouble(_("Origin large step (pixels)"), "Tool/Visual/Nudge/Origin Step Large", 0.01, 1000, 0.1, 2);
	binder->AddDouble(_("Perspective quad step (pixels)"), "Tool/Visual/Nudge/Perspective Step", 0.01, 1000, 0.1, 2);
	binder->AddDouble(_("Perspective quad large step (pixels)"), "Tool/Visual/Nudge/Perspective Step Large", 0.01, 1000, 0.1, 2);

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

void AddCommandButtonEditor(OptionPage *p);

/// Interface preferences page
void BuildInterfacePage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Edit Box"));
#ifdef WITH_WXSTC
	binder->AddBool(_("Use styled edit box"), "Subtitle/Use STC");
	binder->AddBool(_("Enable call tips"), "App/Call Tips");
#endif
	binder->AddBool(_("Overwrite in time boxes"), "Subtitle/Time Edit/Insert Mode");
#ifdef WITH_WXSTC
	binder->AddBool(_("Enable syntax highlighting"), "Subtitle/Highlight/Syntax");
	binder->AddBool(_("Show color swatches on color tags"), "Subtitle/Highlight/Color Swatches");
#ifdef __WXMSW__
	// Opt-in only: historical default-on DirectWrite was reverted on Win10.
	binder->AddBool(_("Use DirectWrite for styled edit box (experimental)"), "Subtitle/Edit Box/Use DirectWrite");
#endif
#endif
	binder->AddDirectory(_("Dictionaries path"), "Path/Dictionary");
	binder->AddFont(_("Font"), "Subtitle/Edit Box/");
	binder->AddInt(_("Edit box height"), "Subtitle/Edit Box/Display Height", -1, 2000);
	binder->AddInt(_("Margin spin step"), "Subtitle/Edit Box/Margin Spin Step", 1, AssStyle::MaxMargin);

#ifdef WITH_WXSTC
	binder->AddCategory(_("Character Markers"));
	binder->AddBool(_("Show normal spaces"), "Subtitle/Edit Box/Character Markers/Show/Space");
	binder->AddBool(_("Show ideographic spaces"), "Subtitle/Edit Box/Character Markers/Show/Ideographic Space");
	binder->AddBool(_("Show no-break and other Unicode spaces"), "Subtitle/Edit Box/Character Markers/Show/Unicode Whitespace");
	binder->AddBool(_("Show CR/LF"), "Subtitle/Edit Box/Character Markers/Show/Line Endings");
	binder->AddBool(_("Show control characters"), "Subtitle/Edit Box/Character Markers/Show/Control Characters");
	binder->AddBool(_("Show Unicode invisible characters"), "Subtitle/Edit Box/Character Markers/Show/Invisible Characters");

	binder->AddCategory(_("Character Error Highlights"));
	auto *error_enabled = binder->AddBool(_("Enable character error highlights"), "Subtitle/Edit Box/Character Markers/Error/Enabled");
	auto *error_space = binder->AddBool(_("Mark normal spaces as errors"), "Subtitle/Edit Box/Character Markers/Error/Space");
	auto *error_ideo = binder->AddBool(_("Mark ideographic spaces as errors"), "Subtitle/Edit Box/Character Markers/Error/Ideographic Space");
	auto *error_nbsp = binder->AddBool(_("Mark no-break spaces as errors"), "Subtitle/Edit Box/Character Markers/Error/No-Break Space");
	auto *error_other_ws = binder->AddBool(_("Mark other Unicode whitespace as errors"), "Subtitle/Edit Box/Character Markers/Error/Other Unicode Whitespace");
	auto *error_eol = binder->AddBool(_("Mark CR/LF as errors"), "Subtitle/Edit Box/Character Markers/Error/Line Endings");
	auto *error_tab = binder->AddBool(_("Mark TAB as errors"), "Subtitle/Edit Box/Character Markers/Error/Tab");
	auto *error_ctrl = binder->AddBool(_("Mark other control characters as errors"), "Subtitle/Edit Box/Character Markers/Error/Other Control Characters");
	auto *error_bidi = binder->AddBool(_("Mark bidi control characters as errors"), "Subtitle/Edit Box/Character Markers/Error/Bidi Controls");
	auto *error_join = binder->AddBool(_("Mark join control characters as errors"), "Subtitle/Edit Box/Character Markers/Error/Join Controls");
	auto *error_invis = binder->AddBool(_("Mark other invisible characters as errors"), "Subtitle/Edit Box/Character Markers/Error/Other Invisible Characters");
	const wxString context_policy_labels[] = {
		_("Always follow category setting"),
		_("Exempt recognized valid contexts"),
		_("Never mark as errors")};
	wxArrayString context_policy_choices(3, context_policy_labels);
	auto *join_policy = binder->AddChoice(_("Join control context policy"), context_policy_choices, "Subtitle/Edit Box/Character Markers/Error/Join Control Context Policy");
	auto *variation_selector_policy = binder->AddChoice(_("Variation selector context policy"), context_policy_choices, "Subtitle/Edit Box/Character Markers/Error/Variation Selector Context Policy");
	// Read the property value (pending UI state), not OPT_GET: option writes are
	// deferred until Apply, so OPT_GET stays stale while the user edits.
	auto update_error_enable = [grid, error_enabled, error_space, error_ideo, error_nbsp, error_other_ws, error_eol, error_tab, error_ctrl, error_bidi, error_join, error_invis, join_policy, variation_selector_policy]() {
		bool const enabled = error_enabled->GetValue().GetBool();
		grid->EnableProperty(error_space, enabled);
		grid->EnableProperty(error_ideo, enabled);
		grid->EnableProperty(error_nbsp, enabled);
		grid->EnableProperty(error_other_ws, enabled);
		grid->EnableProperty(error_eol, enabled);
		grid->EnableProperty(error_tab, enabled);
		grid->EnableProperty(error_ctrl, enabled);
		grid->EnableProperty(error_bidi, enabled);
		grid->EnableProperty(error_join, enabled);
		grid->EnableProperty(error_invis, enabled);
		grid->EnableProperty(join_policy, enabled);
		grid->EnableProperty(variation_selector_policy, enabled);
	};
	update_error_enable();
	grid->Bind(wxEVT_PG_CHANGED, [error_enabled, update_error_enable](wxPropertyGridEvent& evt) {
		if (evt.GetProperty() == error_enabled)
			update_error_enable();
		evt.Skip();
	});
#endif

	binder->AddCategory(_("Character Counter"));
	binder->AddInt(_("Maximum characters per line"), "Subtitle/Character Limit", 0, 1000);
	binder->AddInt(_("Characters Per Second Warning Threshold"), "Subtitle/Character Counter/CPS Warning Threshold", 0, 1000);
	binder->AddInt(_("Characters Per Second Error Threshold"), "Subtitle/Character Counter/CPS Error Threshold", 0, 1000);
	binder->AddBool(_("Ignore whitespace"), "Subtitle/Character Counter/Ignore Whitespace");
	binder->AddBool(_("Ignore punctuation"), "Subtitle/Character Counter/Ignore Punctuation");
	binder->AddBool(_("Show CPS with one decimal place"), "Subtitle/Character Counter/Show Decimal CPS");

	binder->AddCategory(_("Grid"));
#ifdef AEGISUB_WITH_SKIA_SUBTITLE_GRID
	auto *skia_grid = binder->AddBool(
		_("Use Skia subtitle grid (restart required)"),
		"Subtitle/Grid/Skia/Enabled");
	skia_grid->SetHelpString(_(
		"Use the experimental Skia CPU renderer for the subtitle grid. "
		"The wx renderer remains the compatibility fallback. Restart Aegisub after changing this option."));
#ifdef __WXMSW__
	auto *skia_grid_clear_type = binder->AddBool(
		_("Use ClearType in Skia subtitle grid"),
		"Subtitle/Grid/Skia/ClearType");
	skia_grid_clear_type->SetHelpString(_(
		"Keep disabled for grayscale natural-symmetric text. When enabled, ClearType is still used only "
		"for an eligible local, opaque, unscaled display; remote and uncertain sessions remain grayscale."));
#endif
#endif
	binder->AddBool(_("Focus grid on click"), "Subtitle/Grid/Focus Allow");
	binder->AddBool(_("Highlight visible subtitles"), "Subtitle/Grid/Highlight Subtitles in Frame");
	auto *hide_overrides_char = binder->AddString(_("Hide overrides symbol"), "Subtitle/Grid/Hide Overrides Char");
	grid->SetPropertyMaxLength(hide_overrides_char, 1);
	binder->AddFont(_("Font"), "Subtitle/Grid/");

	binder->AddCategory(_("Translation Assistant"));
	binder->AddBool(_("Skip over whitespace"), "Tool/Translation Assistant/Skip Whitespace");

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Interface command buttons preferences subpage
void BuildCommandButtonsPage(OptionPage *p) {
	auto general = p->PageSizer(_("Options"));
	p->OptionAdd(general, _("Show command button row"), "Subtitle/Edit Box/Command Buttons/Enabled");
	p->CellSkip(general);
	AddCommandButtonEditor(p);
	p->SetSizerAndFit(p->sizer);
}

/// Interface Colours preferences subpage
void BuildInterfaceColoursPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Audio Display"));
	binder->AddColour(_("Play cursor"), "Colour/Audio Display/Play Cursor");
	binder->AddColour(_("Line boundary start"), "Colour/Audio Display/Line boundary Start");
	binder->AddColour(_("Line boundary end"), "Colour/Audio Display/Line boundary End");
	binder->AddColour(_("Line boundary inactive line"), "Colour/Audio Display/Line Boundary Inactive Line");
	binder->AddColour(_("Syllable boundaries"), "Colour/Audio Display/Syllable Boundaries");
	binder->AddColour(_("Seconds boundaries"), "Colour/Audio Display/Seconds Line");

	binder->AddCategory(_("Syntax Highlighting"));
	binder->AddColour(_("Background"), "Colour/Subtitle/Background");
	binder->AddColour(_("Normal"), "Colour/Subtitle/Syntax/Normal");
#ifdef WITH_WXSTC
	binder->AddColour(_("Comments"), "Colour/Subtitle/Syntax/Comment");
	binder->AddColour(_("Drawing commands"), "Colour/Subtitle/Syntax/Drawing Command");
	binder->AddColour(_("Drawing X coordinates"), "Colour/Subtitle/Syntax/Drawing X");
	binder->AddColour(_("Drawing Y coordinates"), "Colour/Subtitle/Syntax/Drawing Y");
	binder->AddBool(_("Underline drawing curve endpoints"), "Colour/Subtitle/Syntax/Underline/Drawing Endpoint");
	binder->AddColour(_("Brackets"), "Colour/Subtitle/Syntax/Brackets");
	binder->AddColour(_("Slashes and Parentheses"), "Colour/Subtitle/Syntax/Slashes");
	binder->AddColour(_("Tags"), "Colour/Subtitle/Syntax/Tags");
	binder->AddColour(_("Parameters"), "Colour/Subtitle/Syntax/Parameters");
	binder->AddColour(_("Error"), "Colour/Subtitle/Syntax/Error");
	binder->AddColour(_("Error Background"), "Colour/Subtitle/Syntax/Background/Error");
	binder->AddColour(_("Line Break"), "Colour/Subtitle/Syntax/Line Break");
	binder->AddColour(_("Karaoke templates"), "Colour/Subtitle/Syntax/Karaoke Template");
	binder->AddColour(_("Karaoke variables"), "Colour/Subtitle/Syntax/Karaoke Variable");
	binder->AddColour(_("Character marker"), "Colour/Subtitle/Character Marker");
	binder->AddColour(_("Character marker error"), "Colour/Subtitle/Character Marker Error");
#endif

	binder->AddCategory(_("Audio Color Schemes"));
	wxArrayString schemes = to_wx(OPT_GET("Audio/Colour Schemes")->GetListString());
	binder->AddChoice(_("Spectrum"), schemes, "Colour/Audio Display/Spectrum");
	binder->AddChoice(_("Waveform"), schemes, "Colour/Audio Display/Waveform");

	binder->AddCategory(_("Subtitle Grid"));
	binder->AddColour(_("Standard foreground"), "Colour/Subtitle Grid/Standard");
	binder->AddColour(_("Standard background"), "Colour/Subtitle Grid/Background/Background");
	binder->AddColour(_("Selection foreground"), "Colour/Subtitle Grid/Selection");
	binder->AddColour(_("Selection background"), "Colour/Subtitle Grid/Background/Selection");
	binder->AddColour(_("Collision foreground"), "Colour/Subtitle Grid/Collision");
	binder->AddColour(_("In frame background"), "Colour/Subtitle Grid/Background/Inframe");
	binder->AddColour(_("Comment background"), "Colour/Subtitle Grid/Background/Comment");
	binder->AddColour(_("Selected comment background"), "Colour/Subtitle Grid/Background/Selected Comment");
	binder->AddColour(_("Header background"), "Colour/Subtitle Grid/Header");
	binder->AddColour(_("Left Column"), "Colour/Subtitle Grid/Left Column");
	binder->AddColour(_("Active Line Border"), "Colour/Subtitle Grid/Active Border");
	binder->AddColour(_("Lines"), "Colour/Subtitle Grid/Lines");
	binder->AddColour(_("CPS Error"), "Colour/Subtitle Grid/CPS Error");

	binder->AddCategory(_("Visual Typesetting Tools"));
	binder->AddColour(_("Primary Lines"), "Colour/Visual Tools/Lines Primary");
	binder->AddColour(_("Secondary Lines"), "Colour/Visual Tools/Lines Secondary");
	binder->AddColour(_("Primary Highlight"), "Colour/Visual Tools/Highlight Primary");
	binder->AddColour(_("Secondary Highlight"), "Colour/Visual Tools/Highlight Secondary");
	binder->AddColour(_("Perspective Invalid Line"), "Colour/Visual Tools/Perspective Invalid Line");
	binder->AddColour(_("Perspective Invalid Handle"), "Colour/Visual Tools/Perspective Invalid Handle");
	binder->AddDouble(_("Shaded Area"), "Colour/Visual Tools/Shaded Area Alpha", 0.0, 1.0, 0.1, 2);

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Backup preferences page
void BuildBackupPage(OptionPage *p) {
	auto save = p->PageSizer(_("Automatic Save"));
	wxControl *cb = p->OptionAdd(save, _("Enable"), "App/Auto/Save");
	p->CellSkip(save);
	p->EnableIfChecked(cb,
					   p->OptionAdd(save, _("Interval in seconds"), "App/Auto/Save Every Seconds", 1));
	p->OptionBrowse(save, _("Path"), "Path/Auto/Save", cb, true);
	p->OptionAdd(save, _("Autosave after every change"), "App/Auto/Save on Every Change");

	auto backup = p->PageSizer(_("Automatic Backup"));
	cb = p->OptionAdd(backup, _("Enable"), "App/Auto/Backup");
	p->CellSkip(backup);
	p->OptionBrowse(backup, _("Path"), "Path/Auto/Backup", cb, true);

	auto external = p->PageSizer(_("External Changes"));
	auto *reload_external = p->OptionAdd(
		external,
		_("Detect external modifications to open subtitle files"),
		"App/Auto/Reload External Changes");
	reload_external->SetToolTip(_(
		"When enabled, Aegisub watches the currently open subtitle file, prompts to reload external changes, and warns before saving over them. "
		"Disabling this stops external-change detection and both kinds of prompts. "
		"Takes effect when you click Apply or OK; no restart required."));

	p->SetSizerAndFit(p->sizer);
}

/// Automation preferences page
void BuildAutomationPage(OptionPage *p) {
	auto general = p->PageSizer(_("General"));

	p->OptionAdd(general, _("Base path"), "Path/Automation/Base");
	p->OptionAdd(general, _("Include path"), "Path/Automation/Include");
	p->OptionAdd(general, _("Auto-load path"), "Path/Automation/Autoload");

	const wxString tl_arr[6] = {_("0: Fatal"), _("1: Error"), _("2: Warning"), _("3: Hint"), _("4: Debug"), _("5: Trace")};
	wxArrayString tl_choice(6, tl_arr);
	p->OptionChoice(general, _("Trace level"), tl_choice, "Automation/Trace Level");

	const wxString ar_arr[4] = {_("No scripts"), _("Subtitle-local scripts"), _("Global autoload scripts"), _("All scripts")};
	wxArrayString ar_choice(4, ar_arr);
	p->OptionChoice(general, _("Autoreload on Export"), ar_choice, "Automation/Autoreload Mode");

	auto live_debug = p->PageSizer(_("Live Debug"));
	p->OptionAdd(live_debug, _("Listen port (0 = auto)"), "Automation/Debug/Listen Port", 0, 65535);
	auto require_token = p->OptionAdd(live_debug, _("Require attach token"), "Automation/Debug/Require Token");
	auto token = p->OptionAdd(live_debug, _("Attach token (blank = auto-generate)"), "Automation/Debug/Token");
	p->EnableIfChecked(require_token, token);

	auto *lua_workspace = p->PageSizer(_("Lua Workspace"));
	auto *enable_lua_ls = p->OptionAdd(lua_workspace, _("Enable LuaLS"), "Automation/Lua Workspace/Enable LuaLS");
	p->CellSkip(lua_workspace);
	p->OptionBrowse(lua_workspace, _("LuaLS directory"), "Automation/Lua Workspace/LuaLS Directory", enable_lua_ls, true);
	p->OptionAdd(lua_workspace, _("Wrap editor lines to window width"), "Automation/Lua Workspace/Editor/Wrap");
	p->CellSkip(lua_workspace);
	p->OptionFont(lua_workspace, "Automation/Lua Workspace/Editor/");

	p->SetSizerAndFit(p->sizer);
}

/// Advanced preferences page
void BuildAdvancedPage(OptionPage *p) {
	auto general = p->PageSizer(_("General"));

	auto warning = new wxStaticText(p, wxID_ANY, _("Changing these settings might result in bugs and/or crashes.  Do not touch these unless you know what you're doing."));
	warning->SetFont(wxFont(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
	p->sizer->Fit(p);
	warning->Wrap(400);
	general->Add(warning, 0, wxALL, 5);

	p->SetSizerAndFit(p->sizer);
}

/// Advanced Audio preferences subpage
void BuildAdvancedAudioPage(OptionPage *p) {
	NormalizeAudioDisplayDormantOptions();

	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Expert"));
	binder->AddChoice(_("Audio provider"), GetAudioProviderChoices(), "Audio/Provider");

	wxArrayString apl_choice = to_wx(AudioPlayerFactory::GetClasses());
	binder->AddChoice(_("Audio player"), apl_choice, "Audio/Player");

	binder->AddCategory(_("Cache"));
	const wxString ct_arr[3] = {_("None (Not recommended with Avisynth)"), _("RAM"), _("Hard Disk")};
	wxArrayString ct_choice(3, ct_arr);
	binder->AddChoice(_("Cache type"), ct_choice, "Audio/Cache/Type");
	binder->AddDirectory(_("Path"), "Audio/Cache/HD/Location");

	binder->AddCategory(_("Spectrum Cache"));
	binder->AddInt(_("Cache memory max (MB)"), "Audio/Renderer/Spectrum/Memory Max", 2, 1024);

#ifdef WITH_AVISYNTH
	binder->AddCategory(wxS("Avisynth"));
	const wxString adm_arr[4] = {wxS("None"), wxS("ConvertToMono"), wxS("GetLeftChannel"), wxS("GetRightChannel")};
	wxArrayString adm_choice(4, adm_arr);
	binder->AddChoice(_("Avisynth down-mixer"), adm_choice, "Audio/Downmixer");
	binder->AddInt(_("Force sample rate"), "Provider/Audio/AVS/Sample Rate", 0, INT_MAX);
#endif

#ifdef WITH_FFMS2
	binder->AddCategory(wxS("FFmpegSource"));

	const wxString error_modes[] = {_("Ignore"), _("Clear"), _("Stop"), _("Abort")};
	wxArrayString error_modes_choice(4, error_modes);
	binder->AddChoice(_("Audio indexing error handling mode"), error_modes_choice, "Provider/Audio/FFmpegSource/Decode Error Handling");

	binder->AddBool(_("Always index all audio tracks"), "Provider/FFmpegSource/Index All Tracks");
	binder->AddBool(_("Downmix to 16bit mono audio"), "Provider/Audio/FFmpegSource/Downmix");
#endif

#ifdef WITH_LSMASNATIVE
	binder->AddCategory(wxS("LsmasNative"));
	binder->AddBool(_("Downmix to 16bit mono audio"), "Provider/Audio/LsmasNative/Downmix");
#endif

#ifdef WITH_PORTAUDIO
	binder->AddCategory(wxS("Portaudio"));
	binder->AddChoice(_("Portaudio device"), PortAudioPlayer::GetOutputDevices(), "Player/Audio/PortAudio/Device Name");
#endif

#ifdef WITH_OSS
	binder->AddCategory(wxS("OSS"));
	binder->AddDirectory(_("OSS Device"), "Player/Audio/OSS/Device");
#endif

#if defined(WITH_DIRECTSOUND) && defined(WITH_XAUDIO2)
	binder->AddCategory(wxS("DirectSound / XAudio2"));
#elif defined(WITH_DIRECTSOUND)
	binder->AddCategory(wxS("DirectSound"));
#elif defined(WITH_XAUDIO2)
	binder->AddCategory(wxS("XAudio2"));
#endif
#if defined(WITH_DIRECTSOUND) || defined(WITH_XAUDIO2)
	binder->AddInt(_("Buffer latency"), "Player/Audio/DirectSound/Buffer Latency", 1, 1000);
	binder->AddInt(_("Buffer length"), "Player/Audio/DirectSound/Buffer Length", 1, 100);
#endif

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// Advanced Video preferences subpage
void BuildAdvancedVideoPage(OptionPage *p) {
	auto binder = std::make_shared<PropertyGridOptionBinder>(p);
	auto *grid = binder->GetGrid();
	binder->BindEvents(binder);

	binder->AddCategory(_("Expert"));
	binder->AddChoice(_("Video provider"), VideoProviderFactory::GetChoices(), "Video/Provider");
	wxArrayString renderer_choices;
	renderer_choices.Add(wxS("opengl"));
#ifdef WITH_LIBPLACEBO
	renderer_choices.Add(wxS("libplacebo"));
#endif
	binder->AddChoice(_("Video renderer"), renderer_choices, "Video/Renderer/Backend");

	wxArrayString sp_choice = to_wx(SubtitlesProviderFactory::GetClasses());
	binder->AddChoice(_("Subtitles provider"), sp_choice, "Subtitle/Provider");

	binder->AddCategory(_("Video"));
	binder->AddInt(_("Frame cache memory max (MB)"), "Provider/Video/Cache/Size", 0, INT_MAX);

#ifdef WITH_AVISYNTH
	binder->AddCategory(wxS("Avisynth"));
	binder->AddBool(_("Allow pre-2.56a Avisynth"), "Provider/Avisynth/Allow Ancient");
	binder->AddFile(_("Avisynth runtime library path"), "Provider/Avisynth/Runtime Path",
#ifdef _WIN32
					_("Dynamic libraries (*.dll)|*.dll|All files (*.*)|*.*")
#elif defined(__APPLE__)
					_("Dynamic libraries (*.dylib)|*.dylib|All files (*.*)|*.*")
#else
					_("Shared objects (*.so;*.so.*)|*.so;*.so.*|All files (*.*)|*.*")
#endif
	);
	binder->AddInt(_("Avisynth memory limit"), "Provider/Avisynth/Memory Max", 0, INT_MAX);
#endif

#ifdef WITH_FFMS2
	binder->AddCategory(wxS("FFmpegSource"));

	const wxString log_levels[] = {wxS("Quiet"), wxS("Panic"), wxS("Fatal"), wxS("Error"), wxS("Warning"), wxS("Info"), wxS("Verbose"), wxS("Debug")};
	wxArrayString log_levels_choice(8, log_levels);
	binder->AddChoice(_("Debug log verbosity"), log_levels_choice, "Provider/FFmpegSource/Log Level");

	binder->AddInt(_("Decoding threads"), "Provider/Video/FFmpegSource/Decoding Threads", -1, INT_MAX);
	binder->AddBool(_("Enable unsafe seeking"), "Provider/Video/FFmpegSource/Unsafe Seeking");
#endif

#ifdef WITH_LSMASNATIVE
	binder->AddCategory(wxS("LsmasNative"));
	binder->AddInt(_("Decoding threads"), "Provider/Video/LsmasNative/Decoding Threads", 0, INT_MAX);
#endif

#ifdef WITH_SCENECHANGE
	binder->AddCategory(wxS("SceneChange"));
	const wxString scenechange_backends[] = {wxS("auto"), wxS("scxvid"), wxS("wwxd")};
	wxArrayString scenechange_backend_choices(3, scenechange_backends);
	binder->AddChoice(_("SceneChange backend"), scenechange_backend_choices, "Provider/SceneChange/Backend");
#endif

	p->sizer->Add(grid, 1, wxEXPAND);
	p->SetSizerAndFit(p->sizer);
}

/// wxDataViewIconTextRenderer with command name autocompletion
class CommandRenderer final : public wxDataViewCustomRenderer {
	wxArrayString autocomplete;
	wxDataViewIconText value;
	static const int icon_width = 20;

	int GetIconWidth() const {
		auto view = GetView();
		return view ? view->FromDIP(icon_width) : icon_width;
	}

	wxSize GetDefaultSize() const {
		auto view = GetView();
		return view ? view->FromDIP(wxSize(80, 20)) : wxSize(80, 20);
	}

	public:
	CommandRenderer()
		: wxDataViewCustomRenderer(wxS("wxDataViewIconText"), wxDATAVIEW_CELL_EDITABLE), autocomplete(to_wx(cmd::get_registered_commands())) {
	}

	wxWindow *CreateEditorCtrl(wxWindow *parent, wxRect label_rect, wxVariant const& value) override {
		wxDataViewIconText iconText;
		iconText << value;

		wxString text = iconText.GetText();
		if (text == wxS("-"))
			text.clear();
		int iconWidth = GetIconWidth();

		// adjust the label rect to take the width of the icon into account
		label_rect.x += iconWidth;
		label_rect.width -= iconWidth;

		wxTextCtrl *ctrl = new wxTextCtrl(parent, -1, text, label_rect.GetPosition(), label_rect.GetSize(), wxTE_PROCESS_ENTER);
		ctrl->SetInsertionPointEnd();
		ctrl->SelectAll();
		ctrl->AutoComplete(autocomplete);
		return ctrl;
	}

	bool SetValue(wxVariant const& var) override {
		value << var;
		return true;
	}

	bool Render(wxRect rect, wxDC *dc, int state) override {
		wxIcon const& icon = value.GetIcon();
		int iconWidth = GetIconWidth();
		if (icon.IsOk())
			dc->DrawIcon(icon, rect.x, rect.y + (rect.height - icon.GetHeight()) / 2);

		RenderText(value.GetText(), iconWidth, rect, dc, state);

		return true;
	}

	wxSize GetSize() const override {
		if (!value.GetText().empty()) {
			wxSize size = GetTextExtent(value.GetText());
			size.x += GetIconWidth();
			return size;
		}
		return GetDefaultSize();
	}

	bool GetValueFromEditorCtrl(wxWindow *editor, wxVariant& var) override {
		wxTextCtrl *text = static_cast<wxTextCtrl *>(editor);
		wxDataViewIconText iconText(text->GetValue(), value.GetIcon());
		var << iconText;
		return true;
	}

	bool GetValue(wxVariant&) const override { return false; }
	bool HasEditorCtrl() const override { return true; }
};

std::string NormalizeCommandButtonValue(std::string value) {
	agi::util::strings::trim_inplace(value);
	return value == "-" ? std::string() : value;
}

std::vector<std::string> LoadCommandButtonCommands() {
	auto opt = OPT_GET(kCommandButtonCommandsOption);
	if (opt->GetType() == agi::OptionType::ListString)
		return opt->GetListString();

	std::vector<std::string> commands;
	agi::util::strings::for_each_split_any(opt->GetString(), "\r\n,;", false, [&](agi::util::strings::view item) {
		commands.emplace_back(NormalizeCommandButtonValue(std::string(item)));
	});
	return commands;
}

class CommandButtonDataViewModel final : public wxDataViewVirtualListModel {
	Preferences *parent;
	std::vector<std::string> commands;
	bool has_pending_changes = false;

	void MarkDirty() {
		if (!has_pending_changes) {
			has_pending_changes = true;
			parent->AddPendingChange([this] { Apply(); });
		}
	}

	void Apply() {
		OPT_SET(kCommandButtonCommandsOption)->SetListString(commands);
		has_pending_changes = false;
	}

	public:
	explicit CommandButtonDataViewModel(Preferences *parent)
		: wxDataViewVirtualListModel(static_cast<unsigned int>(LoadCommandButtonCommands().size())), parent(parent), commands(LoadCommandButtonCommands()) {
	}

	unsigned int GetColumnCount() const override { return 3; }
	wxString GetColumnType(unsigned int col) const override {
		return col == 1 ? wxS("wxDataViewIconText") : wxS("string");
	}

	void GetValueByRow(wxVariant& variant, unsigned row, unsigned col) const override {
		if (row >= commands.size())
			return;

		auto const& command = commands[row];
		auto [cmd_name, display_name] = toolbar::ParseCommandEntry(command);
		if (col == 0) {
			// Display Name
			variant = to_wx(display_name);
			return;
		}

		if (col == 1) {
			// Command name with icon
			wxBitmapBundle icon;
			if (!cmd_name.empty()) {
				if (auto *cmd = cmd::get_if(cmd_name))
					icon = cmd->IconBundle();
			}
			wxString label = cmd_name.empty() ? wxString(wxS("-")) : to_wx(cmd_name);
			variant << wxDataViewIconText(label, icon);
			return;
		}

		if (col == 2) {
			// Description
			if (cmd_name.empty())
				variant = _("Separator");
			else if (auto *cmd = cmd::get_if(cmd_name))
				variant = cmd->StrHelp();
			else
				variant = _("Unknown command");
		}
	}

	bool SetValueByRow(wxVariant const& variant, unsigned row, unsigned col) override {
		if (row >= commands.size())
			return false;

		auto [cmd_name, display_name] = toolbar::ParseCommandEntry(commands[row]);
		if (col == 0) {
			std::string new_display = from_wx(variant.GetString());
			agi::util::strings::trim_inplace(new_display);
			display_name = std::move(new_display);
		}
		else if (col == 1) {
			wxDataViewIconText text;
			text << variant;
			cmd_name = NormalizeCommandButtonValue(from_wx(text.GetText()));
		}
		else {
			return false;
		}

		commands[row] = toolbar::MakeCommandEntry(cmd_name, display_name);
		MarkDirty();
		RowChanged(row);
		return true;
	}

	unsigned AppendCommand(std::string command) {
		commands.emplace_back(NormalizeCommandButtonValue(std::move(command)));
		RowAppended();
		MarkDirty();
		return static_cast<unsigned>(commands.size() - 1);
	}

	void DeleteCommand(unsigned row) {
		if (row >= commands.size())
			return;

		commands.erase(commands.begin() + row);
		RowDeleted(row);
		MarkDirty();
	}

	unsigned MoveCommand(unsigned row, int direction) {
		if (row >= commands.size())
			return row;

		int new_row = static_cast<int>(row) + direction;
		if (new_row < 0 || new_row >= static_cast<int>(commands.size()))
			return row;

		std::swap(commands[row], commands[new_row]);
		Reset(static_cast<unsigned>(commands.size()));
		MarkDirty();
		return static_cast<unsigned>(new_row);
	}
};

void AddCommandButtonEditor(OptionPage *p) {
	p->parent->AddChangeableOption(kCommandButtonCommandsOption);

	auto box = new wxStaticBoxSizer(wxVERTICAL, p, _("Commands Bar"));
	auto *model = new CommandButtonDataViewModel(p->parent);
	auto *dvc = new wxDataViewCtrl(p, -1, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES | wxDV_VERT_RULES | wxDV_SINGLE);
	dvc->AssociateModel(model);
	model->DecRef();

	dvc->AppendTextColumn(_("Display Name"), 0, wxDATAVIEW_CELL_EDITABLE, 120, wxALIGN_LEFT, wxCOL_RESIZABLE);
	dvc->AppendColumn(new wxDataViewColumn(_("Command"), new CommandRenderer, 1, 250, wxALIGN_LEFT, wxCOL_RESIZABLE));
	dvc->AppendTextColumn(_("Description"), 2, wxDATAVIEW_CELL_INERT, 300, wxALIGN_LEFT, wxCOL_RESIZABLE);
	dvc->SetMinSize(p->FromDIP(wxSize(520, 240)));
	box->Add(dvc, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxTOP, 5));

	auto selected_row = [dvc, model]() -> int {
		auto item = dvc->GetSelection();
		if (!item.IsOk())
			return -1;
		return static_cast<int>(model->GetRow(item));
	};

	auto buttons = new wxBoxSizer(wxHORIZONTAL);
	auto add_button = new wxButton(p, -1, _("&New"));
	auto separator_button = new wxButton(p, -1, _("&Separator"));
	auto edit_button = new wxButton(p, -1, _("&Edit"));
	auto delete_button = new wxButton(p, -1, _("&Delete"));
	auto up_button = new wxButton(p, -1, _("&Up"));
	auto down_button = new wxButton(p, -1, _("&Down"));

	add_button->Bind(wxEVT_BUTTON, [dvc, model](wxCommandEvent&) {
		unsigned row = model->AppendCommand(std::string());
		auto item = model->GetItem(row);
		dvc->Select(item);
		dvc->EnsureVisible(item);
		dvc->EditItem(item, dvc->GetColumn(0));
	});
	separator_button->Bind(wxEVT_BUTTON, [dvc, model](wxCommandEvent&) {
		unsigned row = model->AppendCommand(std::string());
		auto item = model->GetItem(row);
		dvc->Select(item);
		dvc->EnsureVisible(item);
	});
	edit_button->Bind(wxEVT_BUTTON, [dvc](wxCommandEvent&) {
		auto item = dvc->GetSelection();
		if (item.IsOk())
			dvc->EditItem(item, dvc->GetColumn(0));
	});
	delete_button->Bind(wxEVT_BUTTON, [dvc, model, selected_row](wxCommandEvent&) {
		int row = selected_row();
		if (row < 0)
			return;
		model->DeleteCommand(static_cast<unsigned>(row));
		if (model->GetCount()) {
			unsigned select = static_cast<unsigned>(std::min<int>(row, static_cast<int>(model->GetCount()) - 1));
			dvc->Select(model->GetItem(select));
		}
	});
	up_button->Bind(wxEVT_BUTTON, [dvc, model, selected_row](wxCommandEvent&) {
		int row = selected_row();
		if (row < 0)
			return;
		unsigned new_row = model->MoveCommand(static_cast<unsigned>(row), -1);
		dvc->Select(model->GetItem(new_row));
	});
	down_button->Bind(wxEVT_BUTTON, [dvc, model, selected_row](wxCommandEvent&) {
		int row = selected_row();
		if (row < 0)
			return;
		unsigned new_row = model->MoveCommand(static_cast<unsigned>(row), 1);
		dvc->Select(model->GetItem(new_row));
	});

	buttons->Add(add_button, wxSizerFlags().Border(wxALL, 5));
	buttons->Add(separator_button, wxSizerFlags().Border(wxTOP | wxBOTTOM | wxRIGHT, 5));
	buttons->Add(edit_button, wxSizerFlags().Border(wxTOP | wxBOTTOM | wxRIGHT, 5));
	buttons->Add(delete_button, wxSizerFlags().Border(wxTOP | wxBOTTOM | wxRIGHT, 5));
	buttons->AddStretchSpacer(1);
	buttons->Add(up_button, wxSizerFlags().Border(wxTOP | wxBOTTOM | wxRIGHT, 5));
	buttons->Add(down_button, wxSizerFlags().Border(wxTOP | wxBOTTOM | wxRIGHT, 5));
	box->Add(buttons, wxSizerFlags().Expand());

	p->sizer->Add(box, 0, wxEXPAND | wxALL, p->FromDIP(5));
}

class HotkeyRenderer final : public wxDataViewCustomRenderer {
	wxString value;
	wxTextCtrl *ctrl = nullptr;

	wxSize GetDefaultSize() const {
		auto view = GetView();
		return view ? view->FromDIP(wxSize(80, 20)) : wxSize(80, 20);
	}

	public:
	HotkeyRenderer()
		: wxDataViewCustomRenderer(wxS("string"), wxDATAVIEW_CELL_EDITABLE) {}

	wxWindow *CreateEditorCtrl(wxWindow *parent, wxRect label_rect, wxVariant const& var) override {
		ctrl = new wxTextCtrl(parent, -1, var.GetString(), label_rect.GetPosition(), label_rect.GetSize(), wxTE_PROCESS_ENTER);
		ctrl->SetInsertionPointEnd();
		ctrl->SelectAll();
		ctrl->Bind(wxEVT_CHAR_HOOK, &HotkeyRenderer::OnKeyDown, this);
		ctrl->Bind(wxEVT_AUX1_DOWN, &HotkeyRenderer::OnMouse, this);
		ctrl->Bind(wxEVT_AUX2_DOWN, &HotkeyRenderer::OnMouse, this);
		return ctrl;
	}

	void OnKeyDown(wxKeyEvent& evt) {
		ctrl->ChangeValue(to_wx(hotkey::keypress_to_str(evt.GetKeyCode(), evt.GetModifiers())));
	}

	void OnMouse(wxMouseEvent& evt) {
		auto combo = hotkey::mousepress_to_str(evt);
		if (combo.empty()) {
			evt.Skip();
			return;
		}

		ctrl->ChangeValue(to_wx(combo));
	}

	bool SetValue(wxVariant const& var) override {
		value = var.GetString();
		return true;
	}

	bool Render(wxRect rect, wxDC *dc, int state) override {
		RenderText(value, 0, rect, dc, state);
		return true;
	}

	bool GetValueFromEditorCtrl(wxWindow *, wxVariant& var) override {
		var = ctrl->GetValue();
		return true;
	}

	bool GetValue(wxVariant&) const override { return false; }
	wxSize GetSize() const override { return !value ? GetDefaultSize() : GetTextExtent(value); }
	bool HasEditorCtrl() const override { return true; }
};

static void edit_item(wxDataViewCtrl *dvc, wxDataViewItem item) {
	dvc->EditItem(item, dvc->GetColumn(0));
}

class Interface_Hotkeys final : public OptionPage {
	wxDataViewCtrl *dvc;
	wxObjectDataPtr<HotkeyDataViewModel> model;
	wxSearchCtrl *quick_search;

	void OnNewButton(wxCommandEvent&);
	void OnUpdateFilter(wxCommandEvent&);

	public:
	Interface_Hotkeys(wxTreebook *book, Preferences *parent);
};

/// Interface Hotkeys preferences subpage
Interface_Hotkeys::Interface_Hotkeys(wxTreebook *book, Preferences *parent)
	: OptionPage(book, parent, _("Hotkeys"), OptionPage::PAGE_SUB)
	  // Seed tool contexts that may have no defaults yet so users can bind commands.
	  ,
	  model(new HotkeyDataViewModel(parent, {
												"Visual Vector Clip",
											})) {
	quick_search = new wxSearchCtrl(this, -1);
	auto new_button = new wxButton(this, -1, _("&New"));
	auto edit_button = new wxButton(this, -1, _("&Edit"));
	auto delete_button = new wxButton(this, -1, _("&Delete"));

	new_button->Bind(wxEVT_BUTTON, &Interface_Hotkeys::OnNewButton, this);
	edit_button->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) { edit_item(dvc, dvc->GetSelection()); });
	delete_button->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) { model->Delete(dvc->GetSelection()); });

	quick_search->Bind(wxEVT_TEXT, &Interface_Hotkeys::OnUpdateFilter, this);
	quick_search->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [=](wxCommandEvent&) { quick_search->SetValue(wxEmptyString); });

	dvc = new wxDataViewCtrl(this, -1);
	dvc->AssociateModel(model.get());
#ifndef __APPLE__
	dvc->AppendColumn(new wxDataViewColumn(wxS("Hotkey"), new HotkeyRenderer, 0, 125, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE));
	dvc->AppendColumn(new wxDataViewColumn(wxS("Command"), new CommandRenderer, 1, 250, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE));
#else
	auto col = new wxDataViewColumn(wxS("Hotkey"), new wxDataViewTextRenderer(wxS("string"), wxDATAVIEW_CELL_EDITABLE), 0, 150, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE);
	col->SetMinWidth(150);
	dvc->AppendColumn(col);
	dvc->AppendColumn(new wxDataViewColumn(wxS("Command"), new wxDataViewIconTextRenderer(wxS("wxDataViewIconText"), wxDATAVIEW_CELL_EDITABLE), 1, 250, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE));
#endif
	dvc->AppendTextColumn(wxS("Description"), 2, wxDATAVIEW_CELL_INERT, 300, wxALIGN_LEFT, wxCOL_SORTABLE | wxCOL_RESIZABLE);

	wxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->Add(quick_search, wxSizerFlags().Border());
	buttons->AddStretchSpacer(1);
	buttons->Add(new_button, wxSizerFlags().Border());
	buttons->Add(edit_button, wxSizerFlags().Border());
	buttons->Add(delete_button, wxSizerFlags().Border());

	sizer->Add(buttons, wxSizerFlags().Expand());
	sizer->Add(dvc, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT));

	SetSizerAndFit(sizer);
}

void Interface_Hotkeys::OnNewButton(wxCommandEvent&) {
	wxDataViewItem sel = dvc->GetSelection();
	dvc->ExpandAncestors(sel);
	dvc->Expand(sel);

	wxDataViewItem new_item = model->New(sel);
	if (new_item.IsOk()) {
		dvc->Select(new_item);
		dvc->EnsureVisible(new_item);
		edit_item(dvc, new_item);
	}
}

void Interface_Hotkeys::OnUpdateFilter(wxCommandEvent&) {
	model->SetFilter(quick_search->GetValue());

	if (!quick_search->GetValue().empty()) {
		wxDataViewItemArray contexts;
		model->GetChildren(wxDataViewItem(nullptr), contexts);
		for (auto const& context : contexts)
			dvc->Expand(context);
	}
}

}

void Preferences::RegisterDeferredPageBuilder(Thunk builder, bool built) {
	deferred_page_builders.push_back(std::move(builder));
	deferred_page_built.push_back(built);
}

void Preferences::EnsureDeferredPageBuilt(int page) {
	if (page < 0 || page >= static_cast<int>(deferred_page_builders.size()))
		return;
	if (deferred_page_built[page])
		return;

	bool const should_freeze = IsShownOnScreen();
	bool const is_current_page = book && book->GetSelection() == page;
	wxSize const old_size = GetSize();
	if (should_freeze)
		Freeze();

	deferred_page_builders[page]();
	deferred_page_built[page] = true;

	book->InvalidateBestSize();
	book->Layout();
	if (auto *sizer = GetSizer())
		sizer->Layout();
	Layout();
	if (is_current_page) {
		if (auto *sizer = GetSizer()) {
			sizer->Fit(this);
			auto const fitted_size = GetSize();
			SetSize(std::max(old_size.x, fitted_size.x), std::max(old_size.y, fitted_size.y));
		}
	}

	if (should_freeze)
		Thaw();
}

void Preferences::EnsureAllDeferredPagesBuilt() {
	for (int page = 0; page < static_cast<int>(deferred_page_builders.size()); ++page)
		EnsureDeferredPageBuilt(page);
}

void Preferences::SetOption(std::unique_ptr<agi::OptionValue> new_value) {
	pending_changes[new_value->GetName()] = std::move(new_value);
	if (applyButton)
		applyButton->Enable(true);
}

void Preferences::AddPendingChange(Thunk const& callback) {
	pending_callbacks.push_back(callback);
	if (applyButton)
		applyButton->Enable(true);
}

void Preferences::AddChangeableOption(std::string const& name) {
	option_names.push_back(name);
}

agi::fs::path Preferences::RequestOpenFile(agi::OpenFileDialogRequest const& request) const {
	if (file_dialog_service)
		return file_dialog_service->RequestOpenFile(request);
	return {};
}

agi::fs::path Preferences::RequestSaveFile(agi::SaveFileDialogRequest const& request) const {
	if (file_dialog_service)
		return file_dialog_service->RequestSaveFile(request);
	return {};
}

agi::fs::path Preferences::RequestSelectDirectory(agi::SelectDirectoryDialogRequest const& request) const {
	if (file_dialog_service)
		return file_dialog_service->RequestSelectDirectory(request);
	return {};
}

agi::InteractionResult Preferences::RequestInteraction(agi::InteractionRequest const& request) const {
	if (interaction_sink)
		return interaction_sink->Request(request);
	return agi::InteractionResult::Cancel;
}

void Preferences::OnOK(wxCommandEvent& event) {
	OnApply(event);
	EndModal(0);
}

void Preferences::OnApply(wxCommandEvent&) {
	for (auto const& change : pending_changes)
		OPT_SET(change.first)->Set(change.second.get());
	pending_changes.clear();

	for (auto const& thunk : pending_callbacks)
		thunk();
	pending_callbacks.clear();

	applyButton->Enable(false);
	config::opt->Flush();
}

void Preferences::OnResetDefault(wxCommandEvent&) {
	if (RequestInteraction({from_wx(_("Restore defaults?")),
							from_wx(_("Are you sure that you want to restore the defaults? All your settings will be overridden.")),
							agi::InteractionButtons::YesNo,
							agi::InteractionIcon::Question}) != agi::InteractionResult::Yes)
		return;

	EnsureAllDeferredPagesBuilt();

	for (auto const& opt_name : option_names) {
		agi::OptionValue *opt = OPT_SET(opt_name);
		if (!opt->IsDefault())
			opt->Reset();
	}
	config::opt->Flush();

	agi::hotkey::Hotkey def_hotkeys("", libresrc_getconfig(default_hotkey, default_hotkey_size));
	hotkey::inst->SetHotkeyMap(def_hotkeys.GetHotkeyMap());

	// Close and reopen the dialog to update all the controls with the new values
	OPT_SET("Tool/Preferences/Page")->SetInt(book->GetSelection());
	EndModal(-1);
}

Preferences::Preferences(wxWindow *parent) : wxDialog(parent, -1, _("Preferences"), wxDefaultPosition, wxSize(-1, -1), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
	SetIcon(GETICON(options_button_16));
	file_dialog_service = agi::MakePreferencesFileDialogService(this);
	interaction_sink = agi::MakePreferencesInteractionSink(this);

	auto duration_ms = [](auto const& started) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	};
	auto observe_phase = [&](char const *phase, auto&& callback) {
		auto const started = std::chrono::steady_clock::now();
		callback();
		perf_trace::ObserveWindowOpenPhase("preferences", phase, duration_ms(started));
	};

	observe_phase("treebook_create", [&] {
		book = new wxTreebook(this, -1, wxDefaultPosition, wxDefaultSize);
		book->SetDoubleBuffered(true);
		if (auto *tree = book->GetTreeCtrl())
			tree->SetDoubleBuffered(true);
	});

	auto register_deferred_page = [&](char const *phase, wxString const& name, int style, auto builder) {
		auto *page = new OptionPage(book, this, name, style);
		RegisterDeferredPageBuilder([&, page, phase, builder] {
			auto const started = std::chrono::steady_clock::now();
			builder(page);
			perf_trace::ObserveWindowOpenPhase("preferences", phase, duration_ms(started));
		});
	};

	register_deferred_page("page_general", _("General"), OptionPage::PAGE_DEFAULT, BuildGeneralPage);
	register_deferred_page("page_default_styles", _("Default styles"), OptionPage::PAGE_SUB, BuildGeneralDefaultStylesPage);
#ifdef WITH_DISCORD_PRESENCE
	register_deferred_page("page_discord", wxS("Discord"), OptionPage::PAGE_SUB, BuildDiscordPage);
#endif
	register_deferred_page("page_audio", _("Audio"), OptionPage::PAGE_DEFAULT, BuildAudioPage);
	register_deferred_page("page_video", _("Video"), OptionPage::PAGE_DEFAULT, BuildVideoPage);
	// Inserting pages here shifts Tool/Preferences/Page indices used by
	// tests/gui-automation/skia-audio-uia.cs (Colors is currently page 7).
	register_deferred_page("page_visual_tools", _("Visual Tools"), OptionPage::PAGE_DEFAULT, BuildVisualToolsPage);
	register_deferred_page("page_interface", _("Interface"), OptionPage::PAGE_DEFAULT, BuildInterfacePage);
	register_deferred_page("page_interface_colours", _("Colors"), OptionPage::PAGE_SCROLL | OptionPage::PAGE_SUB, BuildInterfaceColoursPage);
	register_deferred_page("page_interface_command_buttons", _("Commands Bar"), OptionPage::PAGE_SUB, BuildCommandButtonsPage);
	observe_phase("page_hotkeys", [&] { new Interface_Hotkeys(book, this); });
	RegisterDeferredPageBuilder({}, true);
	register_deferred_page("page_backup", _("Backup"), OptionPage::PAGE_DEFAULT, BuildBackupPage);
	register_deferred_page("page_automation", _("Automation"), OptionPage::PAGE_DEFAULT, BuildAutomationPage);
	register_deferred_page("page_advanced", _("Advanced"), OptionPage::PAGE_DEFAULT, BuildAdvancedPage);
	register_deferred_page("page_advanced_audio", _("Audio"), OptionPage::PAGE_SUB, BuildAdvancedAudioPage);
	register_deferred_page("page_advanced_video", _("Video"), OptionPage::PAGE_SUB, BuildAdvancedVideoPage);

	int initial_page = OPT_GET("Tool/Preferences/Page")->GetInt();
	if (initial_page < 0 || initial_page >= static_cast<int>(deferred_page_builders.size()))
		initial_page = 0;
	book->ChangeSelection(initial_page);
	try {
		EnsureDeferredPageBuilt(initial_page);
	}
	catch (...) {
		if (initial_page == 0)
			throw;
		initial_page = 0;
		OPT_SET("Tool/Preferences/Page")->SetInt(initial_page);
		book->ChangeSelection(initial_page);
		EnsureDeferredPageBuilt(initial_page);
	}

	observe_phase("book_fit", [&] {
		book->Fit();
	});

	wxSizer *mainSizer = nullptr;
	wxButton *defaultButton = nullptr;
	observe_phase("dialog_chrome", [&] {
		book->Bind(wxEVT_TREEBOOK_PAGE_CHANGING, [this](wxBookCtrlEvent& evt) {
			EnsureDeferredPageBuilt(evt.GetSelection());
		});
		book->Bind(wxEVT_TREEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& evt) {
			OPT_SET("Tool/Preferences/Page")->SetInt(evt.GetSelection());
		});

		// Bottom Buttons
		auto stdButtonSizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxAPPLY | wxHELP);
		applyButton = stdButtonSizer->GetApplyButton();
		wxSizer *buttonSizer = new wxBoxSizer(wxHORIZONTAL);
		defaultButton = new wxButton(this, -1, _("&Restore Defaults"));
		buttonSizer->Add(defaultButton, wxSizerFlags(0).Expand());
		buttonSizer->AddStretchSpacer(1);
		buttonSizer->Add(stdButtonSizer, wxSizerFlags(0).Expand());

		// Main Sizer
		mainSizer = new wxBoxSizer(wxVERTICAL);
		mainSizer->Add(book, wxSizerFlags(1).Expand().Border());
		mainSizer->Add(buttonSizer, wxSizerFlags(0).Expand().Border(wxALL & ~wxTOP));
	});

	observe_phase("dialog_fit", [&] {
		SetSizerAndFit(mainSizer);
		wxSize const fitted = GetSize();
		SetMinSize(fitted);
		SetSize(std::max(fitted.x, FromDIP(520)), std::max(fitted.y, FromDIP(720)));
	});
	observe_phase("dialog_center", [&] {
		persist = agi::make_unique<PersistLocation>(this, "Tool/Preferences", true);
	});

	applyButton->Enable(false);

	Bind(wxEVT_BUTTON, &Preferences::OnOK, this, wxID_OK);
	Bind(wxEVT_BUTTON, &Preferences::OnApply, this, wxID_APPLY);
	Bind(wxEVT_BUTTON, std::bind(&HelpButton::OpenPage, "Options"), wxID_HELP);
	defaultButton->Bind(wxEVT_BUTTON, &Preferences::OnResetDefault, this);
}

void ShowPreferences(wxWindow *parent) {
	while (true) {
		auto const open_started = std::chrono::steady_clock::now();
		perf_trace::TraceWindowOpenBegin("preferences");
		try {
			Preferences dialog(parent);
			auto const duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_started).count();
			perf_trace::TraceWindowOpenEnd("preferences", duration_ms, true);
			if (dialog.ShowModal() >= 0)
				break;
		}
		catch (...) {
			auto const duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_started).count();
			perf_trace::TraceWindowOpenEnd("preferences", duration_ms, false);
			throw;
		}
	}
}
