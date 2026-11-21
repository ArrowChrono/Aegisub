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

#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include "../compat.h"
#include "../dialog_detached_video.h"
#include "../dialog_manager.h"
#include "../dialogs.h"
#include "../frame_main.h"
#include "../include/aegisub/context.h"
#include "../include/aegisub/context_ui.h"
#include "../libresrc/libresrc.h"
#include "../main.h"
#include "../options.h"
#include "../project.h"
#include "../ui_services.h"
#include "../utils.h"

namespace {
	using cmd::Command;

struct app_about final : public Command {
	CMD_NAME("app/about")
	CMD_ICON(about_menu)
	STR_MENU("&About")
	STR_DISP("About")
	STR_HELP("About Aegisub")

	void operator()(agi::Context *c) override {
		ShowAboutDialog(c->GetUI().parent);
	}
};

struct app_display_audio_subs final : public Command {
	CMD_NAME("app/display/audio_subs")
	STR_MENU("&Audio+Subs View")
	STR_DISP("Audio+Subs View")
	STR_HELP("Display audio and the subtitles grid only")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	void operator()(agi::Context *c) override {
		c->GetUI().frame->SetDisplayMode(0,1);
	}

	bool Validate(const agi::Context *c) override {
		return !!c->GetCore().project->AudioProvider();
	}

	bool IsActive(const agi::Context *c) override {
		auto ui = c->GetUI();
		return ui.frame->IsAudioShown() && !ui.frame->IsVideoShown();
	}
};

struct app_display_full final : public Command {
	CMD_NAME("app/display/full")
	STR_MENU("&Full view")
	STR_DISP("Full view")
	STR_HELP("Display audio, video and then subtitles grid")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	void operator()(agi::Context *c) override {
		c->GetUI().frame->SetDisplayMode(1,1);
	}

	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		auto ui = c->GetUI();
		return core.project->AudioProvider() && core.project->VideoProvider() && !ui.dialog->Get<DialogDetachedVideo>();
	}

	bool IsActive(const agi::Context *c) override {
		auto ui = c->GetUI();
		return ui.frame->IsAudioShown() && ui.frame->IsVideoShown();
	}
};

struct app_display_subs final : public Command {
	CMD_NAME("app/display/subs")
	STR_MENU("S&ubs Only View")
	STR_DISP("Subs Only View")
	STR_HELP("Display the subtitles grid only")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	void operator()(agi::Context *c) override {
		c->GetUI().frame->SetDisplayMode(0, 0);
	}

	bool IsActive(const agi::Context *c) override {
		auto ui = c->GetUI();
		return !ui.frame->IsAudioShown() && !ui.frame->IsVideoShown();
	}
};

struct app_display_video_subs final : public Command {
	CMD_NAME("app/display/video_subs")
	STR_MENU("&Video+Subs View")
	STR_DISP("Video+Subs View")
	STR_HELP("Display video and the subtitles grid only")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	void operator()(agi::Context *c) override {
		c->GetUI().frame->SetDisplayMode(1, 0);
	}

	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		auto ui = c->GetUI();
		return core.project->VideoProvider() && !ui.dialog->Get<DialogDetachedVideo>();
	}

	bool IsActive(const agi::Context *c) override {
		auto ui = c->GetUI();
		return !ui.frame->IsAudioShown() && ui.frame->IsVideoShown();
	}
};

struct app_exit final : public Command {
	CMD_NAME("app/exit")
	STR_MENU("E&xit")
	STR_DISP("Exit")
	STR_HELP("Exit the application")

	void operator()(agi::Context *) override {
		wxGetApp().CloseAll();
	}
};

struct app_language final : public Command {
	CMD_NAME("app/language")
	CMD_ICON(languages_menu)
	STR_MENU("&Language...")
	STR_DISP("Language")
	STR_HELP("Select Aegisub interface language")

	void operator()(agi::Context *c) override {
		// Get language
		auto new_language = wxGetApp().GetLocale().PickLanguage(c ? c->GetSingleChoiceInteractionSink() : std::shared_ptr<agi::SingleChoiceInteractionSink>());
		if (new_language.empty()) return;

		OPT_SET("App/Language")->SetString(new_language);

		// Ask to restart program
		auto result = c->RequestInteraction({
			"Restart Aegisub?",
			"Aegisub needs to be restarted so that the new language can be applied. Restart now?",
			agi::InteractionButtons::YesNo,
			agi::InteractionIcon::Question
		});
		if (result == agi::InteractionResult::Yes) {
			// Restart Aegisub
			if (c->GetUI().frame->Close()) {
				RestartAegisub();
			}
		}
	}
};

struct app_log final : public Command {
	CMD_NAME("app/log")
	CMD_ICON(about_menu)
	STR_MENU("&Log window")
	STR_DISP("Log window")
	STR_HELP("View the event log")

	void operator()(agi::Context *c) override {
		ShowLogWindow(c);
	}
};

struct app_new_window final : public Command {
	CMD_NAME("app/new_window")
	CMD_ICON(new_window_menu)
	STR_MENU("New &Window")
	STR_DISP("New Window")
	STR_HELP("Open a new application window")

	void operator()(agi::Context *) override {
		wxGetApp().NewProjectContext();
	}
};

struct app_options final : public Command {
	CMD_NAME("app/options")
	CMD_ICON(options_button)
	STR_MENU("&Options...")
	STR_DISP("Options")
	STR_HELP("Configure Aegisub")

	void operator()(agi::Context *c) override {
		try {
			ShowPreferences(c->GetUI().parent);
		} catch (agi::Exception& e) {
			LOG_E("config/init") << "Caught exception: " << e.GetMessage();
		}
	}
};

struct app_toggle_global_hotkeys final : public Command {
	CMD_NAME("app/toggle/global_hotkeys")
	CMD_ICON(toggle_audio_medusa)
	STR_MENU("Toggle global hotkey overrides")
	STR_DISP("Toggle global hotkey overrides")
	STR_HELP("Toggle global hotkey overrides (Medusa Mode)")
	CMD_TYPE(COMMAND_TOGGLE)

	bool IsActive(const agi::Context *c) override {
		return OPT_GET("Audio/Medusa Timing Hotkeys")->GetBool();
	}

	void operator()(agi::Context *c) override {
		agi::OptionValue *opt = OPT_SET("Audio/Medusa Timing Hotkeys");
		opt->SetBool(!opt->GetBool());
	}
};

struct app_toggle_toolbar final : public Command {
	CMD_NAME("app/toggle/toolbar")
	STR_HELP("Toggle the main toolbar")
	CMD_TYPE(COMMAND_DYNAMIC_NAME)

	wxString StrMenu(const agi::Context *c) const override {
		return OPT_GET("App/Show Toolbar")->GetBool() ?
			_("Hide Toolbar") :
			_("Show Toolbar");
	}

	wxString StrDisplay(const agi::Context *c) const override {
		return StrMenu(nullptr);
	}

	void operator()(agi::Context *c) override {
		agi::OptionValue *opt = OPT_SET("App/Show Toolbar");
		opt->SetBool(!opt->GetBool());
	}
};

struct app_updates final : public Command {
	CMD_NAME("app/updates")
	STR_MENU("&Check for Updates...")
	STR_DISP("Check for Updates")
	STR_HELP("Check to see if there is a new version of Aegisub available")

	void operator()(agi::Context *c) override {
		PerformVersionCheck(true);
	}
};

/// Character-marker view toggles. STR_MENU/STR_HELP use string literals so
/// xgettext (-kSTR_MENU -kSTR_HELP) can extract them without macro expansion.
/// WITH_WXSTC=OFF: registered but Validate() is false (menu item disabled).
static bool CharacterMarkerCommandsAvailable() {
#ifdef WITH_WXSTC
	return true;
#else
	return false;
#endif
}

struct app_character_markers_space final : public Command {
	CMD_NAME("app/character_markers/space")
	STR_MENU("&Normal Spaces")
	STR_DISP("Normal Spaces")
	STR_HELP("Show markers for ordinary spaces (U+0020)")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	bool Validate(const agi::Context *) override { return CharacterMarkerCommandsAvailable(); }
	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Edit Box/Character Markers/Show/Space")->GetBool();
	}
	void operator()(agi::Context *) override {
		agi::OptionValue *opt = OPT_SET("Subtitle/Edit Box/Character Markers/Show/Space");
		opt->SetBool(!opt->GetBool());
	}
};

struct app_character_markers_ideographic_space final : public Command {
	CMD_NAME("app/character_markers/ideographic_space")
	STR_MENU("&Ideographic Spaces")
	STR_DISP("Ideographic Spaces")
	STR_HELP("Show markers for ideographic spaces (U+3000)")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	bool Validate(const agi::Context *) override { return CharacterMarkerCommandsAvailable(); }
	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Edit Box/Character Markers/Show/Ideographic Space")->GetBool();
	}
	void operator()(agi::Context *) override {
		agi::OptionValue *opt = OPT_SET("Subtitle/Edit Box/Character Markers/Show/Ideographic Space");
		opt->SetBool(!opt->GetBool());
	}
};

struct app_character_markers_unicode_whitespace final : public Command {
	CMD_NAME("app/character_markers/unicode_whitespace")
	STR_MENU("&No-Break and Other Unicode Spaces")
	STR_DISP("No-Break and Other Unicode Spaces")
	STR_HELP("Show markers for no-break spaces and other Unicode whitespace")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	bool Validate(const agi::Context *) override { return CharacterMarkerCommandsAvailable(); }
	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Edit Box/Character Markers/Show/Unicode Whitespace")->GetBool();
	}
	void operator()(agi::Context *) override {
		agi::OptionValue *opt = OPT_SET("Subtitle/Edit Box/Character Markers/Show/Unicode Whitespace");
		opt->SetBool(!opt->GetBool());
	}
};

struct app_character_markers_line_endings final : public Command {
	CMD_NAME("app/character_markers/line_endings")
	STR_MENU("&CR/LF")
	STR_DISP("CR/LF")
	STR_HELP("Show markers for carriage return and line feed characters")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	bool Validate(const agi::Context *) override { return CharacterMarkerCommandsAvailable(); }
	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Edit Box/Character Markers/Show/Line Endings")->GetBool();
	}
	void operator()(agi::Context *) override {
		agi::OptionValue *opt = OPT_SET("Subtitle/Edit Box/Character Markers/Show/Line Endings");
		opt->SetBool(!opt->GetBool());
	}
};

struct app_character_markers_control final : public Command {
	CMD_NAME("app/character_markers/control")
	STR_MENU("Con&trol Characters")
	STR_DISP("Control Characters")
	STR_HELP("Show markers for TAB and other control characters")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	bool Validate(const agi::Context *) override { return CharacterMarkerCommandsAvailable(); }
	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Edit Box/Character Markers/Show/Control Characters")->GetBool();
	}
	void operator()(agi::Context *) override {
		agi::OptionValue *opt = OPT_SET("Subtitle/Edit Box/Character Markers/Show/Control Characters");
		opt->SetBool(!opt->GetBool());
	}
};

struct app_character_markers_invisible final : public Command {
	CMD_NAME("app/character_markers/invisible")
	STR_MENU("&Unicode Invisible Characters")
	STR_DISP("Unicode Invisible Characters")
	STR_HELP("Show markers for bidi controls, join controls, and other invisible characters")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	bool Validate(const agi::Context *) override { return CharacterMarkerCommandsAvailable(); }
	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Edit Box/Character Markers/Show/Invisible Characters")->GetBool();
	}
	void operator()(agi::Context *) override {
		agi::OptionValue *opt = OPT_SET("Subtitle/Edit Box/Character Markers/Show/Invisible Characters");
		opt->SetBool(!opt->GetBool());
	}
};

static char const* const kCharacterMarkerShowOptions[] = {
	"Subtitle/Edit Box/Character Markers/Show/Space",
	"Subtitle/Edit Box/Character Markers/Show/Ideographic Space",
	"Subtitle/Edit Box/Character Markers/Show/Unicode Whitespace",
	"Subtitle/Edit Box/Character Markers/Show/Line Endings",
	"Subtitle/Edit Box/Character Markers/Show/Control Characters",
	"Subtitle/Edit Box/Character Markers/Show/Invisible Characters",
};

struct app_character_markers_show_all final : public Command {
	CMD_NAME("app/character_markers/show_all")
	STR_MENU("Show &All Markers")
	STR_DISP("Show All Markers")
	STR_HELP("Enable or disable every character marker display category")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)

	bool Validate(const agi::Context *) override { return CharacterMarkerCommandsAvailable(); }

	bool IsActive(const agi::Context *) override {
		for (auto const* path : kCharacterMarkerShowOptions) {
			if (!OPT_GET(path)->GetBool())
				return false;
		}
		return true;
	}

	void operator()(agi::Context *) override {
		bool const all_on = IsActive(nullptr);
		for (auto const* path : kCharacterMarkerShowOptions)
			OPT_SET(path)->SetBool(!all_on);
	}
};

bool CanControlProjectWindow(FrameMain const *frame) {
	return frame && frame->IsShown() && frame->IsEnabled() && !frame->IsIconized() && !frame->IsBeingDeleted();
}

struct app_minimize final : public Command {
	CMD_NAME("app/minimize")
	STR_MENU("Minimize")
	STR_DISP("Minimize")
	STR_HELP("Minimize the active window")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c && CanControlProjectWindow(c->GetUI().frame);
	}

	void operator()(agi::Context *c) override {
		c->GetUI().frame->Iconize();
	}
};

struct app_maximize final : public Command {
	CMD_NAME("app/maximize")
#ifdef __WXMAC__
	STR_MENU("Zoom")
	STR_DISP("Zoom")
#else
	STR_MENU("Maximize")
	STR_DISP("Maximize")
#endif
	STR_HELP("Maximize the active window")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c && CanControlProjectWindow(c->GetUI().frame);
	}

	void operator()(agi::Context *c) override {
		auto frame = c->GetUI().frame;
		frame->Maximize(!frame->IsMaximized());
	}
};

struct app_bring_to_front final : public Command {
	CMD_NAME("app/bring_to_front")
	STR_MENU("Bring All to Front")
	STR_DISP("Bring All to Front")
	STR_HELP("Bring forward all open documents to the front")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c && CanControlProjectWindow(c->GetUI().frame);
	}

	void operator()([[maybe_unused]] agi::Context *c) override {
#ifdef __WXMAC__
		osx::bring_to_front();
#else
		auto *active = c->GetUI().frame;
		for (auto *window : wxTopLevelWindows) {
			auto *frame = dynamic_cast<FrameMain *>(window);
			if (frame != active && CanControlProjectWindow(frame))
				frame->Raise();
		}
		// Raise the invoking document last so another project does not take its focus.
		active->Raise();
#endif
	}
};

}

namespace cmd {
	void init_app() {
		reg(agi::make_unique<app_about>());
		reg(agi::make_unique<app_display_audio_subs>());
		reg(agi::make_unique<app_display_full>());
		reg(agi::make_unique<app_display_subs>());
		reg(agi::make_unique<app_display_video_subs>());
		reg(agi::make_unique<app_exit>());
		reg(agi::make_unique<app_language>());
		reg(agi::make_unique<app_log>());
		reg(agi::make_unique<app_new_window>());
		reg(agi::make_unique<app_options>());
		reg(agi::make_unique<app_toggle_global_hotkeys>());
		reg(agi::make_unique<app_toggle_toolbar>());
		reg(agi::make_unique<app_character_markers_space>());
		reg(agi::make_unique<app_character_markers_ideographic_space>());
		reg(agi::make_unique<app_character_markers_unicode_whitespace>());
		reg(agi::make_unique<app_character_markers_line_endings>());
		reg(agi::make_unique<app_character_markers_control>());
		reg(agi::make_unique<app_character_markers_invisible>());
		reg(agi::make_unique<app_character_markers_show_all>());
		reg(agi::make_unique<app_minimize>());
		reg(agi::make_unique<app_maximize>());
		reg(agi::make_unique<app_bring_to_front>());
#ifdef WITH_UPDATE_CHECKER
		reg(agi::make_unique<app_updates>());
#endif
	}
}
