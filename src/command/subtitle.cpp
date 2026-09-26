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
#include "../ass_file.h"
#include "../charset_detect.h"
#include "../compat.h"
#include "../dialog_search_replace.h"
#include "../dialog_search_results.h"
#include "../dialogs.h"
#include "../frame_main.h"
#include "../include/aegisub/context.h"
#include "../include/aegisub/context_ui.h"
#include "../libresrc/libresrc.h"
#include "../main.h"
#include "../options.h"
#include "../project.h"
#include "../project_session_ops.h"
#include "../search_replace_engine.h"
#include "../selection_controller.h"
#include "../subs_controller.h"
#include "../subtitle_editor_ops.h"
#include "../subtitle_format.h"
#include "../ui_services.h"
#include "../utils.h"
#include "../video_controller.h"

#include <libaegisub/address_of_adaptor.h>
#include <libaegisub/charset_conv.h>
#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <wx/choicdlg.h>
namespace {
	using cmd::Command;

agi::OpenFileDialogRequest make_open_subtitles_file_request() {
	return {
		from_wx(_("Open subtitles file")),
		"Path/Last/Subtitles",
		"",
		"",
		SubtitleFormat::GetWildcards(0)
	};
}

agi::SaveFileDialogRequest make_save_subtitles_file_request(std::string const& default_filename) {
	return {
		from_wx(_("Save subtitles file")),
		"Path/Last/Subtitles",
		default_filename,
		"ass",
		"Advanced Substation Alpha (*.ass)|*.ass"
	};
}

struct validate_nonempty_selection : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return !c->GetCore().selectionController->GetSelectedSet().empty();
	}
};

struct validate_nonempty_selection_video_loaded : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		return core.project->VideoProvider() && !core.selectionController->GetSelectedSet().empty();
	}
};

struct subtitle_attachment final : public Command {
	CMD_NAME("subtitle/attachment")
	CMD_ICON(attach_button)
	STR_MENU("A&ttachments...")
	STR_DISP("Attachments")
	STR_HELP("Open the attachment manager dialog")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.videoController->Stop();
		ShowAttachmentsDialog(c);
	}
};

struct subtitle_find final : public Command {
	CMD_NAME("subtitle/find")
	CMD_ICON(find_button)
	STR_MENU("&Find...")
	STR_DISP("Find")
	STR_HELP("Search for text in the subtitles")

	void operator()(agi::Context *c) override {
		c->GetCore().videoController->Stop();
		DialogSearchReplace::Show(c, false);
	}
};

struct subtitle_find_next final : public Command {
	CMD_NAME("subtitle/find/next")
	CMD_ICON(find_next_menu)
	STR_MENU("Find &Next")
	STR_DISP("Find Next")
	STR_HELP("Find next match of last search")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.videoController->Stop();
		if (!core.search->FindNext())
			DialogSearchReplace::Show(c, false);
	}
};

struct subtitle_find_all final : public Command {
	CMD_NAME("subtitle/find/all")
	STR_MENU("Find &All")
	STR_DISP("Find All")
	STR_HELP("List every match of the last search")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.videoController->Stop();
		try {
			if (!core.search->FindAll()) {
				DialogSearchReplace::Show(c, false);
				return;
			}
		}
		catch (agi::Exception const& e) {
			c->ShowError(e.GetMessage());
			return;
		}
		catch (std::exception const& e) {
			c->ShowError(e.what());
			return;
		}

		if (!core.search->GetLastMatches().empty())
			DialogSearchResults::Show(c, core.search->GetSettings(), core.search->GetLastMatches());
		else
			DialogSearchResults::Dismiss(c);
	}
};

static aegisub::project_session_ops::SubtitleSessionTarget resolve_subtitle_session_target(agi::Context *c) {
#ifdef __APPLE__
	(void)c;
	return aegisub::project_session_ops::ResolveSubtitleSessionTarget(true, false);
#else
	return aegisub::project_session_ops::ResolveSubtitleSessionTarget(
		false,
		c->GetCore().subsController->TryToClose() == wxCANCEL);
#endif
}

static bool execute_subtitle_load(agi::Context *c,
                                  aegisub::project_session_ops::SubtitleSessionTarget target,
                                  agi::fs::path const& path,
                                  std::string const& encoding = "",
                                  bool load_linked = true) {
	return aegisub::project_session_ops::ExecuteSubtitleLoad(
		target,
		path,
		[&](agi::fs::path const& filename, std::string const& file_encoding, bool linked) {
			c->GetCore().project->LoadSubtitles(filename, file_encoding, linked);
		},
#ifdef __APPLE__
		[&](agi::fs::path const& filename, std::string const& file_encoding, bool linked) {
			wxGetApp().NewProjectContext().GetCore().project->LoadSubtitles(filename, file_encoding, linked);
		},
#else
		aegisub::project_session_ops::SubtitleLoadAction{},
#endif
		encoding,
		load_linked);
}

static void insert_subtitle_at_video(agi::Context *c, bool after) {
	auto core = c->GetCore();
	int default_duration = OPT_GET("Timing/Default Duration")->GetInt();
	int video_ms = core.videoController->TimeAtFrame(core.videoController->GetFrameN(), agi::vfr::START);
	auto new_line = aegisub::subtitle_editor_ops::CreateLineAtVideoTime(
		*core.selectionController->GetActiveLine(),
		video_ms,
		default_duration);

	AssDialogue *inserted = new_line.get();
	auto pos = core.ass->iterator_to(*core.selectionController->GetActiveLine());
	if (after) ++pos;

	core.ass->Events.insert(pos, *new_line.release());
	core.ass->Commit(from_wx(_("line insertion")), AssFile::COMMIT_DIAG_ADDREM);

	core.selectionController->SetSelectionAndActive({ inserted }, inserted);
}

struct subtitle_insert_after final : public validate_nonempty_selection {
	CMD_NAME("subtitle/insert/after")
	STR_MENU("&After Current")
	STR_DISP("After Current")
	STR_HELP("Insert a new line after the current one")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		AssDialogue *active_line = core.selectionController->GetActiveLine();
		auto new_line = aegisub::subtitle_editor_ops::CreateLineAfterActive(
			*active_line,
			core.ass->Events,
			OPT_GET("Timing/Default Duration")->GetInt());
		AssDialogue *inserted = new_line.get();
		auto pos = core.ass->iterator_to(*active_line);
		core.ass->Events.insert(++pos, *new_line.release());

		core.ass->Commit(from_wx(_("line insertion")), AssFile::COMMIT_DIAG_ADDREM);
		core.selectionController->SetSelectionAndActive({ inserted }, inserted);
	}
};

struct subtitle_insert_after_videotime final : public validate_nonempty_selection_video_loaded {
	CMD_NAME("subtitle/insert/after/videotime")
	STR_MENU("After Current, at Video Time")
	STR_DISP("After Current, at Video Time")
	STR_HELP("Insert a new line after the current one, starting at video time")

	void operator()(agi::Context *c) override {
		insert_subtitle_at_video(c, true);
	}
};

struct subtitle_insert_before final : public validate_nonempty_selection {
	CMD_NAME("subtitle/insert/before")
	STR_MENU("&Before Current")
	STR_DISP("Before Current")
	STR_HELP("Insert a new line before the current one")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		AssDialogue *active_line = core.selectionController->GetActiveLine();
		auto new_line = aegisub::subtitle_editor_ops::CreateLineBeforeActive(
			*active_line,
			core.ass->Events,
			OPT_GET("Timing/Default Duration")->GetInt());
		AssDialogue *inserted = new_line.get();
		core.ass->Events.insert(core.ass->iterator_to(*active_line), *new_line.release());

		core.ass->Commit(from_wx(_("line insertion")), AssFile::COMMIT_DIAG_ADDREM);
		core.selectionController->SetSelectionAndActive({ inserted }, inserted);
	}
};

struct subtitle_insert_before_videotime final : public validate_nonempty_selection_video_loaded {
	CMD_NAME("subtitle/insert/before/videotime")
	STR_MENU("Before Current, at Video Time")
	STR_DISP("Before Current, at Video Time")
	STR_HELP("Insert a new line before the current one, starting at video time")

	void operator()(agi::Context *c) override {
		insert_subtitle_at_video(c, false);
	}
};

struct subtitle_new final : public Command {
	CMD_NAME("subtitle/new")
	CMD_ICON(new_toolbutton)
	STR_MENU("&New Subtitles")
	STR_DISP("New Subtitles")
	STR_HELP("New subtitles")

	void operator()(agi::Context *c) override {
		auto workspace_close = agi::make_scope_exit([frame = c->GetUI().frame] { if (frame) frame->FinishLuaWorkspaceClose(false); });
		aegisub::project_session_ops::ExecuteSubtitleSessionAction(
			resolve_subtitle_session_target(c),
			[&] { c->GetCore().project->CloseSubtitles(); },
			[&] { wxGetApp().NewProjectContext(); });
	}
};

struct subtitle_close final : public Command {
	CMD_NAME("subtitle/close")
	CMD_ICON(new_toolbutton)
	STR_MENU("Close")
	STR_DISP("Close")
	STR_HELP("Close")

	void operator()(agi::Context *c) override {
		c->GetUI().frame->Close();
	}
};

struct subtitle_open final : public Command {
	CMD_NAME("subtitle/open")
	CMD_ICON(open_toolbutton)
	STR_MENU("&Open Subtitles...")
	STR_DISP("Open Subtitles")
	STR_HELP("Open a subtitles file")

	void operator()(agi::Context *c) override {
		auto workspace_close = agi::make_scope_exit([frame = c->GetUI().frame] { if (frame) frame->FinishLuaWorkspaceClose(false); });
		auto target = resolve_subtitle_session_target(c);
		if (target == aegisub::project_session_ops::SubtitleSessionTarget::Cancel) return;

		auto filename = c->RequestOpenFile(make_open_subtitles_file_request());
		execute_subtitle_load(c, target, filename);
	}
};

struct subtitle_open_autosave final : public Command {
	CMD_NAME("subtitle/open/autosave")
	STR_MENU("Open A&utosaved Subtitles...")
	STR_DISP("Open Autosaved Subtitles")
	STR_HELP("Open a previous version of a file which was autosaved by Aegisub")

	void operator()(agi::Context *c) override {
		auto workspace_close = agi::make_scope_exit([frame = c->GetUI().frame] { if (frame) frame->FinishLuaWorkspaceClose(false); });
		auto target = resolve_subtitle_session_target(c);
		if (target == aegisub::project_session_ops::SubtitleSessionTarget::Cancel) return;

		auto filename = PickAutosaveFile(c->GetUI().parent);
		execute_subtitle_load(c, target, filename);
	}
};

struct subtitle_open_charset final : public Command {
	CMD_NAME("subtitle/open/charset")
	CMD_ICON(open_with_toolbutton)
	STR_MENU("Open Subtitles with &Charset...")
	STR_DISP("Open Subtitles with Charset")
	STR_HELP("Open a subtitles file with a specific file encoding")

	void operator()(agi::Context *c) override {
		auto workspace_close = agi::make_scope_exit([frame = c->GetUI().frame] { if (frame) frame->FinishLuaWorkspaceClose(false); });
		auto target = resolve_subtitle_session_target(c);
		if (target == aegisub::project_session_ops::SubtitleSessionTarget::Cancel) return;

		auto filename = c->RequestOpenFile(make_open_subtitles_file_request());
		if (filename.empty()) return;

		auto charset = CharSetDetect::PromptForEncodingChoice(
			agi::charset::GetEncodingsList<std::vector<std::string>>(),
			c->GetSingleChoiceInteractionSink());
		if (!charset) return;

		execute_subtitle_load(c, target, filename, *charset);
	}
};

struct subtitle_open_video final : public Command {
	CMD_NAME("subtitle/open/video")
	STR_MENU("Open Subtitles from &Video")
	STR_DISP("Open Subtitles from Video")
	STR_HELP("Open the subtitles from the current video file")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		auto workspace_close = agi::make_scope_exit([frame = c->GetUI().frame] { if (frame) frame->FinishLuaWorkspaceClose(false); });
		if (core.subsController->TryToClose() == wxCANCEL) return;
		core.project->LoadSubtitles(core.project->VideoName(), "binary", false);
	}

	bool Validate(const agi::Context *c) override {
		return c->GetCore().project->CanLoadSubtitlesFromVideo();
	}
};

struct subtitle_properties final : public Command {
	CMD_NAME("subtitle/properties")
	CMD_ICON(properties_toolbutton)
	STR_MENU("&Properties...")
	STR_DISP("Properties")
	STR_HELP("Open script properties window")

	void operator()(agi::Context *c) override {
		c->GetCore().videoController->Stop();
		ShowPropertiesDialog(c);
	}
};

static void save_subtitles(agi::Context *c, agi::fs::path filename) {
	auto core = c->GetCore();
	if (filename.empty()) {
		core.videoController->Stop();
		filename = c->RequestSaveFile(make_save_subtitles_file_request(
			agi::fs::PathToString(core.subsController->Filename().stem()) + ".ass"));
		if (filename.empty()) return;
	}

	try {
		core.subsController->Save(filename);
	}
	catch (const agi::Exception& err) {
		c->ShowError(err.GetMessage());
	}
	catch (...) {
		c->ShowError("Unknown error");
	}
}

struct subtitle_save final : public Command {
	CMD_NAME("subtitle/save")
	CMD_ICON(save_toolbutton)
	STR_MENU("&Save Subtitles")
	STR_DISP("Save Subtitles")
	STR_HELP("Save the current subtitles")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		save_subtitles(c, core.subsController->CanSave() ? core.subsController->Filename() : "");
	}

	bool Validate(const agi::Context *c) override {
		return c->GetCore().subsController->IsModified();
	}
};

struct subtitle_save_as final : public Command {
	CMD_NAME("subtitle/save/as")
	CMD_ICON(save_as_toolbutton)
	STR_MENU("Save Subtitles &as...")
	STR_DISP("Save Subtitles as")
	STR_HELP("Save subtitles with another name")

	void operator()(agi::Context *c) override {
		save_subtitles(c, "");
	}
};

struct subtitle_select_all final : public Command {
	CMD_NAME("subtitle/select/all")
	STR_MENU("Select &All")
	STR_DISP("Select All")
	STR_HELP("Select all dialogue lines")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		Selection sel;
		for (auto& line : core.ass->Events)
			sel.insert(&line);
		core.selectionController->SetSelectedSet(std::move(sel));
	}
};

struct subtitle_select_visible final : public Command {
	CMD_NAME("subtitle/select/visible")
	CMD_ICON(select_visible_button)
	STR_MENU("Select Visible")
	STR_DISP("Select Visible")
	STR_HELP("Select all dialogue lines that are visible on the current video frame")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.videoController->Stop();

		int frame = core.videoController->GetFrameN();
		auto visible = aegisub::subtitle_editor_ops::SelectMatchingLines(core.ass->Events, [&](AssDialogue const& diag) {
			return core.videoController->FrameAtTime(diag.Start, agi::vfr::START) <= frame
				&& core.videoController->FrameAtTime(diag.End, agi::vfr::END) >= frame;
		});

		Selection new_selection(visible.ordered_lines.begin(), visible.ordered_lines.end());
		if (visible.active_line)
			core.selectionController->SetActiveLine(visible.active_line);

		core.selectionController->SetSelectedSet(std::move(new_selection));
	}

	bool Validate(const agi::Context *c) override {
		return !!c->GetCore().project->VideoProvider();
	}
};

struct subtitle_spellcheck final : public Command {
	CMD_NAME("subtitle/spellcheck")
	CMD_ICON(spellcheck_toolbutton)
	STR_MENU("Spell &Checker...")
	STR_DISP("Spell Checker")
	STR_HELP("Open spell checker")

	void operator()(agi::Context *c) override {
		c->GetCore().videoController->Stop();
		ShowSpellcheckerDialog(c);
	}
};

}

namespace cmd {
	void init_subtitle() {
		reg(agi::make_unique<subtitle_attachment>());
		reg(agi::make_unique<subtitle_find>());
		reg(agi::make_unique<subtitle_find_next>());
		reg(agi::make_unique<subtitle_find_all>());
		reg(agi::make_unique<subtitle_insert_after>());
		reg(agi::make_unique<subtitle_insert_after_videotime>());
		reg(agi::make_unique<subtitle_insert_before>());
		reg(agi::make_unique<subtitle_insert_before_videotime>());
		reg(agi::make_unique<subtitle_new>());
		reg(agi::make_unique<subtitle_close>());
		reg(agi::make_unique<subtitle_open>());
		reg(agi::make_unique<subtitle_open_autosave>());
		reg(agi::make_unique<subtitle_open_charset>());
		reg(agi::make_unique<subtitle_open_video>());
		reg(agi::make_unique<subtitle_properties>());
		reg(agi::make_unique<subtitle_save>());
		reg(agi::make_unique<subtitle_save_as>());
		reg(agi::make_unique<subtitle_select_all>());
		reg(agi::make_unique<subtitle_select_visible>());
		reg(agi::make_unique<subtitle_spellcheck>());
	}
}
