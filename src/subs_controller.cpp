// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "subs_controller.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_file_app.h"
#include "ass_info.h"
#include "ass_style.h"
#include "app_runtime.h"
#include "compat.h"
#include "command/command.h"
#include "format.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "project.h"
#include "reload_external_changes_policy.h"
#include "selection_controller.h"
#include "status_sink.h"
#include "subtitle_format.h"
#include "text_selection_controller.h"
#include "ui_services.h"
#include "watched_file.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/util.h>

// #include <wx/msgdlg.h>  ← unused, removed (using context->ShowWarning/RequestInteraction instead)

#include <array>
#include <atomic>
#include <functional>
#include <fstream>
#include <mutex>

namespace {
	constexpr uint64_t kFileWatchFnvOffset = 1469598103934665603ULL;
	constexpr uint64_t kFileWatchFnvPrime = 1099511628211ULL;

	void autosave_timer_changed(SubsControllerTimer *timer) {
		if (!timer)
			return;
		int freq = OPT_GET("App/Auto/Save Every Seconds")->GetInt();
		if (freq > 0 && OPT_GET("App/Auto/Save")->GetBool())
			timer->Start(freq * 1000);
		else
			timer->Stop();
	}

	int interaction_result_to_wx(agi::InteractionResult result) {
		switch (result) {
		case agi::InteractionResult::Ok:
			return wxOK;
		case agi::InteractionResult::Cancel:
			return wxCANCEL;
		case agi::InteractionResult::Yes:
			return wxYES;
		case agi::InteractionResult::No:
			return wxNO;
		}
		return wxCANCEL;
	}

	bool try_hash_file(agi::fs::path const& path, uint64_t& hash) {
		try {
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return false;

			hash = kFileWatchFnvOffset;
			std::array<char, 32768> buffer;
			while (stream) {
				stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				auto const bytes_read = stream.gcount();
				for (std::streamsize i = 0; i < bytes_read; ++i) {
					hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(i)]);
					hash *= kFileWatchFnvPrime;
				}
			}

			return stream.eof();
		}
		catch (...) {
			return false;
		}
	}
}

SubsController::AutosaveInhibitor::AutosaveInhibitor(SubsController *controller)
: controller(controller) {
	++controller->autosave_inhibit_depth;
}

SubsController::AutosaveInhibitor::AutosaveInhibitor(AutosaveInhibitor&& other) noexcept
: controller(other.controller) {
	other.controller = nullptr;
}

SubsController::AutosaveInhibitor::~AutosaveInhibitor() {
	if (controller)
		--controller->autosave_inhibit_depth;
}

SubsController::AutosaveInhibitor SubsController::InhibitAutosave() {
	return AutosaveInhibitor(this);
}

struct SubsController::UndoInfo {
	std::string undo_description;
	int commit_id;

	std::vector<std::pair<std::string, std::string>> script_info;
	std::vector<AssStyle> styles;
	std::vector<AssDialogueBase> events;
	std::vector<AssAttachment> attachments;
	std::vector<ExtradataEntry> extradata;

	mutable std::vector<int> selection;
	int active_line_id = 0;
	int pos = 0, sel_start = 0, sel_end = 0;

	UndoInfo(const agi::Context *c, std::string const& d, int commit_id)
	: undo_description(d)
	, commit_id(commit_id)
	, attachments(c->GetCore().ass->Attachments)
	, extradata(c->GetCore().ass->Extradata)
	{
		auto core = c->GetCore();
		script_info.reserve(core.ass->Info.size());
		for (auto const& info : core.ass->Info)
			script_info.emplace_back(info.Key(), info.Value());

		styles.reserve(core.ass->Styles.size());
		styles.assign(core.ass->Styles.begin(), core.ass->Styles.end());

		events.reserve(core.ass->Events.size());
		events.assign(core.ass->Events.begin(), core.ass->Events.end());

		UpdateActiveLine(c);
		UpdateSelection(c);
		UpdateTextSelection(c);
	}

	void Apply(agi::Context *c) const {
		auto core = c->GetCore();
		// Keep old dialogue lines alive until after the commit is complete
		// since a bunch of stuff holds references to them
		AssFile old;
		old.Events.swap(core.ass->Events);
		core.ass->Info.clear();
		core.ass->Attachments.clear();
		core.ass->Styles.clear();
		core.ass->Extradata.clear();

		sort(begin(selection), end(selection));

		AssDialogue *active_line = nullptr;
		Selection new_sel;

		for (auto const& info : script_info)
			core.ass->Info.push_back(*new AssInfo(info.first, info.second));
		for (auto const& style : styles)
			core.ass->Styles.push_back(*new AssStyle(style));
		core.ass->Attachments = attachments;
		for (auto const& event : events) {
			auto copy = new AssDialogue(event);
			core.ass->Events.push_back(*copy);
			if (copy->Id == active_line_id)
				active_line = copy;
			if (binary_search(begin(selection), end(selection), copy->Id))
				new_sel.insert(copy);
		}
		core.ass->Extradata = extradata;

		core.ass->Commit("", AssFile::COMMIT_NEW);
		core.selectionController->SetSelectionAndActive(std::move(new_sel), active_line);

		core.textSelectionController->SetInsertionPoint(pos);
		core.textSelectionController->SetSelection(sel_start, sel_end);
	}

	void UpdateActiveLine(const agi::Context *c) {
		auto core = c->GetCore();
		auto line = core.selectionController->GetActiveLine();
		if (line)
			active_line_id = line->Id;
	}

	void UpdateSelection(const agi::Context *c) {
		auto core = c->GetCore();
		auto const& sel = core.selectionController->GetSelectedSet();
		selection.clear();
		selection.reserve(sel.size());
		for (const auto diag : sel)
			selection.push_back(diag->Id);
	}

	void UpdateTextSelection(const agi::Context *c) {
		auto core = c->GetCore();
		pos = core.textSelectionController->GetInsertionPoint();
		sel_start = core.textSelectionController->GetSelectionStart();
		sel_end = core.textSelectionController->GetSelectionEnd();
	}
};

struct SubsController::SaveEveryChangeState {
	struct Request {
		uint64_t generation = 0;
		std::function<bool()> prepare;
		std::function<std::function<void()>()> publish;
		std::function<void(std::string const&)> report_error;
	};

	std::mutex mutex;
	subs_controller_detail::LatestSaveState<Request> latest;
	std::vector<std::function<void()>> completed;
	std::function<void()> notify_main;
	bool drain_scheduled = false;
	std::atomic<bool> alive{true};
};

namespace {
	struct SaveEveryChangeStagingFile {
		agi::fs::path path;
		bool published = false;

		~SaveEveryChangeStagingFile() {
			if (published || path.empty())
				return;
			try {
				if (agi::fs::FileExists(path))
					agi::fs::Remove(path);
			}
			catch (...) {
			}
		}
	};

	agi::fs::path make_save_every_change_staging_path(agi::fs::path const& target) {
		auto model = agi::fs::PathToString(target.stem())
			+ ".aegisub-save-%%%%%%%%"
			+ agi::fs::PathToString(target.extension());
		return agi::fs::UniquePath(target.parent_path() / agi::fs::PathFromString(model));
	}
}

SubsController::SubsController(agi::Context *context)
: context(context)
, undo_connection(context->GetCore().ass->AddUndoManager(&SubsController::OnCommit, this))
, text_selection_connection(context->GetCore().textSelectionController->AddSelectionListener(&SubsController::OnTextSelectionChanged, this))
, autosave_queue(agi::dispatch::Create())
, save_every_change_state(std::make_shared<SaveEveryChangeState>())
{
	auto state = save_every_change_state;
	state->notify_main = [this, state] {
		agi::dispatch::Main().Async([this, state] {
			if (state->alive.load(std::memory_order_acquire))
				ApplyCompletedSaveEveryChangeWrites();
		});
	};

	if (!IsGuiRuntimeShell())
		return;

	// Explicit GUI-only flag: headless/automation must not hash open files or
	// block Save() on overwrite interaction. Do not use file_watch==nullptr for
	// this (nullptr can also mean detection is off on a GUI shell).
	tracks_external_file_state = reload_external_changes::ShouldTrackExternalFileSnapshots(true);

	// Live directory watching is optional (App/Auto/Reload External Changes).
	// ApplyReloadExternalChangesOption arms or disarms the watcher at runtime
	// without restart. Subscriptions are scoped Connections so multi-frame
	// project close cannot leave dangling this on process-global options.
	ApplyReloadExternalChangesOption();
	reload_external_changes_connection = OPT_SUB(
		"App/Auto/Reload External Changes",
		[this] { ApplyReloadExternalChangesOption(); });

	autosave_timer = CreateSubsControllerTimer([this] { AutoSave(); });
	autosave_timer_changed(autosave_timer.get());
	autosave_enable_connection = OPT_SUB(
		"App/Auto/Save",
		[this] { autosave_timer_changed(autosave_timer.get()); });
	autosave_interval_connection = OPT_SUB(
		"App/Auto/Save Every Seconds",
		[this] { autosave_timer_changed(autosave_timer.get()); });
}

SubsController::~SubsController() {
	// Disconnect option slots before tearing down timer/watcher state.
	reload_external_changes_connection.Disconnect();
	autosave_enable_connection.Disconnect();
	autosave_interval_connection.Disconnect();

	save_every_change_state->alive.store(false, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(save_every_change_state->mutex);
		save_every_change_state->latest.Stop();
		save_every_change_state->notify_main = {};
	}
	ClearFileWatch();
	// Background writers admitted by SupportsBackgroundWriting never request
	// the UI, so draining this queue from the UI thread cannot deadlock.
	autosave_queue->Sync([]{ });
}

void SubsController::SetSelectionController(SelectionController *selection_controller) {
	auto core = context->GetCore();
	active_line_connection = core.selectionController->AddActiveLineListener(&SubsController::OnActiveLineChanged, this);
	selection_connection = core.selectionController->AddSelectionListener(&SubsController::OnSelectionChanged, this);
}

ProjectProperties SubsController::Load(agi::fs::path const& filename, std::string charset, bool is_reload) {
	if (context->lua_workspace_invocation_active)
		throw agi::UserCancelException("Wait for the Lua Workspace invocation to finish before changing subtitles");
	auto *frame = context->GetUI().frame;
	auto workspace_close = agi::make_scope_exit([frame] {
		if (frame)
			frame->FinishLuaWorkspaceClose(false);
	});
	if (frame && frame->NeedsLuaWorkspaceCloseDecision() && TryToClose() == wxCANCEL)
		throw agi::UserCancelException("Subtitle change cancelled in Lua Workspace");
	WaitForSaveEveryChangeWrites(true);
	AssFile temp;
	auto core = context->GetCore();

	SubtitleFormat::GetReader(filename, charset)->ReadFile(&temp, filename, core.project->Timecodes(), charset, context->GetSingleChoiceInteractionSink(), core.backgroundRunnerFactory);

	if (frame)
		frame->FinishLuaWorkspaceClose(true);
	++document_generation;
	core.ass->swap(temp);
	auto props = core.ass->Properties;

	SetFileName(filename);

	// Push the initial state of the file onto the undo stack
	undo_stack.clear();
	redo_stack.clear();
	autosaved_commit_id = saved_commit_id = commit_id + 1;
	core.ass->Commit("", AssFile::COMMIT_NEW);

	// Save backup of file
	if (CanSave() && OPT_GET("App/Auto/Backup")->GetBool()) {
		auto path_str = OPT_GET("Path/Auto/Backup")->GetString();
		agi::fs::path path;
		if (path_str.empty())
			path = filename.parent_path();
		else
			path = core.path->Decode(path_str);
		agi::fs::CreateDirectory(path);
		agi::fs::Copy(filename, path / agi::fs::PathFromString(agi::fs::PathToString(filename.stem()) + ".ORIGINAL" + agi::fs::PathToString(filename.extension())));
	}

	UpdateFileWatch();
	FileOpen(filename, is_reload);
	return props;
}

void SubsController::Save(agi::fs::path const& filename, std::string const& encoding) {
	// Establish a barrier before any foreground save. An older automatic write
	// can finish staging, but cannot replace the target after this invalidation.
	WaitForSaveEveryChangeWrites(true);
	const SubtitleFormat *writer = SubtitleFormat::GetWriter(filename);
	if (!writer)
		throw agi::InvalidInputException("Unknown file type.");
	if (!ConfirmOverwriteExternalChanges(filename))
		return;

	auto old_filename = this->filename;
	auto old_properties = context->GetCore().ass->Properties;
	int old_autosaved_commit_id = autosaved_commit_id, old_saved_commit_id = saved_commit_id;
	auto core = context->GetCore();
	try {
		autosaved_commit_id = saved_commit_id = commit_id;

		// Have to set this now for the sake of things that want to save paths
		// relative to the script in the header
		this->filename = filename;
		core.path->SetToken("?script", filename.parent_path());
		UpdateProperties();

		const AssFile *save_source = core.ass.get();
		std::unique_ptr<AssFile> save_copy;
		if (!core.ass->Extradata.empty()) {
			save_copy.reset(new AssFile(*core.ass));
			save_copy->CleanExtradata();
			save_source = save_copy.get();
		}

		writer->WriteFile(save_source, filename, core.project->Timecodes(), encoding, context->GetSingleChoiceInteractionSink());
		FileSave();

		SetFileName(filename);
		UpdateFileWatch();
	}
	catch (...) {
		this->filename = old_filename;
		core.path->SetToken("?script", old_filename.parent_path());
		core.ass->Properties = std::move(old_properties);
		autosaved_commit_id = old_autosaved_commit_id;
		saved_commit_id = old_saved_commit_id;
		throw;
	}
}

void SubsController::Close() {
	if (context->lua_workspace_invocation_active)
		throw agi::UserCancelException("Wait for the Lua Workspace invocation to finish before changing subtitles");
	auto *frame = context->GetUI().frame;
	auto workspace_close = agi::make_scope_exit([frame] {
		if (frame)
			frame->FinishLuaWorkspaceClose(false);
	});
	if (frame && frame->NeedsLuaWorkspaceCloseDecision() && TryToClose() == wxCANCEL)
		throw agi::UserCancelException("Subtitle change cancelled in Lua Workspace");
	WaitForSaveEveryChangeWrites(true);
	undo_stack.clear();
	redo_stack.clear();
	autosaved_commit_id = saved_commit_id = commit_id + 1;
	filename.clear();
	AssFile blank;
	auto core = context->GetCore();
	if (frame)
		frame->FinishLuaWorkspaceClose(true);
	++document_generation;
	blank.swap(*core.ass);
	LoadDefaultAssFileWithAppOptions(*core.ass, true, OPT_GET("Subtitle Format/ASS/Default Style Catalog")->GetString());
	core.ass->Commit("", AssFile::COMMIT_NEW);
	ClearFileWatch();
	FileOpen(filename, false);
}

int SubsController::TryToClose(bool allow_cancel) {
	if (context->lua_workspace_invocation_active) {
		context->ShowStatus("Wait for the Lua Workspace invocation to finish before closing subtitles.");
		return wxCANCEL;
	}
	auto *frame = context->GetUI().frame;
	if (frame && !frame->PrepareLuaWorkspaceForClose())
		return wxCANCEL;
	// Preserve the old synchronous option semantics: if the latest automatic
	// write succeeds, closing should not briefly prompt for already-saved work.
	WaitForSaveEveryChangeWrites(false);
	if (!IsModified())
		return wxYES;

	auto buttons = allow_cancel ? agi::InteractionButtons::YesNoCancel : agi::InteractionButtons::YesNo;
	int result = interaction_result_to_wx(context->RequestInteraction({
		from_wx(_("Unsaved changes")),
		from_wx(fmt_tl("Do you want to save changes to %s?", Filename())),
		buttons,
		agi::InteractionIcon::Question
	}));
	if (result == wxYES) {
		cmd::call("subtitle/save", context);
		// If it fails saving, return cancel anyway
		return IsModified() ? wxCANCEL : wxYES;
	}
	return result;
}

void SubsController::AutoSave() {
	if (autosave_inhibit_depth > 0 || commit_id == autosaved_commit_id)
		return;

	auto core = context->GetCore();
	auto directory = core.path->Decode(OPT_GET("Path/Auto/Save")->GetString());
	if (directory.empty())
		directory = filename.parent_path();

	auto name = filename.filename();
	if (name.empty())
		name = agi::fs::PathFromString("Untitled");

	autosaved_commit_id = commit_id;
	auto status_sink = context->GetStatusSink();
	auto choice_sink = context->GetSingleChoiceInteractionSink();
	auto subs_copy = new AssFile(*core.ass);
	auto fps = core.project->Timecodes();
	autosave_queue->Async([subs_copy, name, directory, status_sink, choice_sink, fps] {
		wxString msg;
		std::unique_ptr<AssFile> subs(subs_copy);

		try {
			agi::fs::CreateDirectory(directory);
			auto path = directory / agi::fs::PathFromString(agi::format("%s.%s.AUTOSAVE.ass",
				agi::fs::PathToString(name),
				agi::util::strftime("%Y-%m-%d-%H-%M-%S")));
			SubtitleFormat::GetWriter(path)->WriteFile(subs.get(), path, fps, "", choice_sink);
			msg = fmt_tl("File backup saved as \"%s\".", path);
		}
		catch (const agi::Exception& err) {
			msg = to_wx("Exception when attempting to autosave file: " + err.GetMessage());
		}
		catch (...) {
			msg = wxS("Unhandled exception when attempting to autosave file.");
		}

		if (status_sink)
			status_sink->ShowStatus(from_wx(msg));
	});
}

uint64_t SubsController::BeginSaveEveryChangeRevision() {
	std::vector<std::function<void()>> completed;
	std::optional<SaveEveryChangeState::Request> discarded;
	uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(save_every_change_state->mutex);
		completed.swap(save_every_change_state->completed);
		generation = save_every_change_state->latest.BeginRevision(&discarded);
	}

	for (auto& apply : completed)
		apply();
	return generation;
}

void SubsController::ApplyCompletedSaveEveryChangeWrites() {
	std::vector<std::function<void()>> completed;
	{
		std::lock_guard<std::mutex> lock(save_every_change_state->mutex);
		completed.swap(save_every_change_state->completed);
	}

	for (auto& apply : completed)
		apply();
}

void SubsController::WaitForSaveEveryChangeWrites(bool invalidate) {
	if (invalidate)
		BeginSaveEveryChangeRevision();
	else
		ApplyCompletedSaveEveryChangeWrites();

	autosave_queue->Sync([] { });
	ApplyCompletedSaveEveryChangeWrites();
}

void SubsController::QueueSaveEveryChange(
		uint64_t generation,
		const SubtitleFormat *writer,
		std::optional<FileWatchSnapshot> expected_target) {
	auto core = context->GetCore();

	// Properties are part of the persisted snapshot. Keep this on the UI
	// thread, where subscribers and core.ass are owned.
	UpdateProperties();

	auto snapshot = std::make_shared<AssFile>(*core.ass);
	if (!snapshot->Extradata.empty())
		snapshot->CleanExtradata();

	auto const snapshot_id = commit_id;
	auto const target = filename;
	auto const fps = core.project->Timecodes();
	auto const encoding = SubtitleFormat::ResolveWriteEncoding("");
	auto const status_sink = context->GetStatusSink();
	auto const state = save_every_change_state;
	auto staging = std::make_shared<SaveEveryChangeStagingFile>();
	auto staging_snapshot = std::make_shared<std::optional<FileWatchSnapshot>>();

	SaveEveryChangeState::Request request;
	request.generation = generation;
	request.report_error = [status_sink](std::string const& message) {
		if (status_sink)
			status_sink->ShowStatus(message);
	};
	request.prepare = [this, snapshot, target, fps, encoding, writer, status_sink,
	                   staging, staging_snapshot, expected_target] {
		try {
			staging->path = make_save_every_change_staging_path(target);
			writer->WriteFile(snapshot.get(), staging->path, fps, encoding, {});
			if (!agi::fs::FileExists(staging->path)) {
				if (status_sink)
					status_sink->ShowStatus("Save on every change did not produce an output file.");
				return false;
			}

			*staging_snapshot = MakeFileWatchSnapshot(staging->path);
			if (expected_target) {
				auto const current_target = MakeFileWatchSnapshot(target);
				auto const target_unchanged = expected_target->hash_valid
					? FileWatchSnapshotsEqual(current_target, *expected_target)
					: FileWatchMetadataEqual(current_target, *expected_target);
				if (!target_unchanged) {
					if (status_sink)
						status_sink->ShowStatus(
							"Automatic save skipped because the subtitle file changed while the save was pending.");
					return false;
				}
			}
			return true;
		}
		catch (agi::Exception const& e) {
			if (status_sink)
				status_sink->ShowStatus("Save failed: " + e.GetMessage());
			return false;
		}
		catch (std::exception const& e) {
			if (status_sink)
				status_sink->ShowStatus("Save failed: " + std::string(e.what()));
			return false;
		}
		catch (...) {
			if (status_sink)
				status_sink->ShowStatus("Save on every change: unhandled write error.");
			return false;
		}
	};
	request.publish = [this, state, staging, staging_snapshot, target, snapshot_id] {
		auto completion = std::function<void()>([this, state, staging_snapshot, target, snapshot_id] {
			if (!state->alive.load(std::memory_order_acquire) || filename != target)
				return;
			saved_commit_id = snapshot_id;
			autosaved_commit_id = snapshot_id;
			FileSave();
			if (tracks_external_file_state
					&& OPT_GET("App/Auto/Reload External Changes")->GetBool()
					&& *staging_snapshot) {
				last_known_file_snapshot = **staging_snapshot;
				last_prompted_file_snapshot.reset();
				external_file_change_pending = false;
			}
		});
		agi::fs::Rename(staging->path, target);
		staging->published = true;
		return completion;
	};

	bool schedule_drain = false;
	std::optional<SaveEveryChangeState::Request> discarded;
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (!state->latest.Submit(generation, std::move(request), &discarded))
			return;
		if (!state->drain_scheduled) {
			state->drain_scheduled = true;
			schedule_drain = true;
		}
	}

	if (schedule_drain)
		autosave_queue->Async([state] { DrainSaveEveryChangeQueue(std::move(state)); });
}

void SubsController::DrainSaveEveryChangeQueue(std::shared_ptr<SaveEveryChangeState> state) {
	for (;;) {
		std::optional<SaveEveryChangeState::Request> request;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->latest.IsStopping()) {
				state->drain_scheduled = false;
				return;
			}
			request = state->latest.TakePending();
			if (!request) {
				state->drain_scheduled = false;
				return;
			}
		}

		if (!request->prepare())
			continue;

		std::function<void()> notify_main;
		try {
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->latest.CanPublish(request->generation)) {
				auto completion = request->publish();
				if (completion)
					state->completed.emplace_back(std::move(completion));
				notify_main = state->notify_main;
			}
		}
		catch (agi::Exception const& e) {
			request->report_error("Save failed: " + e.GetMessage());
		}
		catch (std::exception const& e) {
			request->report_error("Save failed: " + std::string(e.what()));
		}
		catch (...) {
			request->report_error("Save on every change: unhandled publish error.");
		}

		if (notify_main)
			notify_main();
	}
}

void SubsController::UpdateFileWatch() {
	// Headless/automation: never snapshot or watch. Save must stay non-interactive.
	if (!tracks_external_file_state)
		return;

	if (filename.empty()) {
		ClearFileWatch();
		return;
	}

	// The preference controls both live reload prompts and save-time overwrite
	// warnings. Do not retain a baseline while detection is disabled.
	if (!OPT_GET("App/Auto/Reload External Changes")->GetBool()) {
		if (file_watch)
			file_watch->ClearTargetPath();
		last_known_file_snapshot.reset();
		last_prompted_file_snapshot.reset();
		external_file_change_pending = false;
		return;
	}

	if (file_watch)
		file_watch->SetTargetPath(filename);
	RecordCurrentFileSnapshot();
}

void SubsController::ClearFileWatch() {
	if (file_watch)
		file_watch->ClearTargetPath();

	last_known_file_snapshot.reset();
	last_prompted_file_snapshot.reset();
	external_file_change_pending = false;
	external_file_prompt_active = false;
}

void SubsController::ApplyReloadExternalChangesOption() {
	using namespace reload_external_changes;
	if (!tracks_external_file_state)
		return;

	auto const plan = PlanWatchArm(
		OPT_GET("App/Auto/Reload External Changes")->GetBool(),
		static_cast<bool>(file_watch),
		!filename.empty(),
		last_known_file_snapshot.has_value());

	if (plan.disarm_watcher && file_watch) {
		// Clear the target only. Never file_watch.reset() here: option changes
		// can nest inside OnWatchedFileChanged via a modal event loop while the
		// call stack still owns frames on WatchedFile's debounce timer.
		// The idle watcher is released in ClearFileWatch / ~SubsController.
		file_watch->ClearTargetPath();
	}
	if (plan.clear_pending)
		external_file_change_pending = false;
	if (plan.clear_snapshots) {
		last_known_file_snapshot.reset();
		last_prompted_file_snapshot.reset();
	}

	if (plan.create_watcher) {
		file_watch = agi::make_unique<WatchedFile>(CreateWxFileSystemWatcherBackend());
		file_watch->SetChangedCallback([this](agi::fs::path const& path) {
			OnWatchedFileChanged(path);
		});
		file_watch->SetErrorCallback([this](std::string const& message) {
			OnFileWatchError(message);
		});
	}

	// Do not call UpdateFileWatch() here: it always rebaselines, and option
	// ValueChanged fires even when Preferences Apply/Reset SetValue keeps the
	// same bool. Rebaselining would hide external edits while detection remains
	// enabled.
	if (plan.bind_target && file_watch)
		file_watch->SetTargetPath(filename);
	if (plan.record_baseline_if_missing)
		RecordCurrentFileSnapshot();
}

SubsController::FileWatchSnapshot SubsController::MakeFileWatchSnapshot(
		agi::fs::path const& path, bool include_hash) const {
	FileWatchSnapshot snapshot;
	if (path.empty())
		return snapshot;

	try {
		snapshot.exists = agi::fs::FileExists(path);
		if (snapshot.exists) {
			snapshot.size = agi::fs::Size(path);
			snapshot.modified_time = agi::fs::ModifiedTime(path);
			if (include_hash)
				snapshot.hash_valid = try_hash_file(path, snapshot.content_hash);
		}
	}
	catch (agi::fs::FileSystemError const&) {
		snapshot.exists = false;
	}

	return snapshot;
}

bool SubsController::FileWatchMetadataEqual(
		FileWatchSnapshot const& left, FileWatchSnapshot const& right) {
	return left.exists == right.exists
		&& left.size == right.size
		&& left.modified_time == right.modified_time;
}

bool SubsController::FileWatchSnapshotsEqual(FileWatchSnapshot const& left, FileWatchSnapshot const& right) {
	if (!FileWatchMetadataEqual(left, right))
		return false;
	if (!left.exists)
		return true;
	if (!left.hash_valid || !right.hash_valid)
		return false;
	return left.content_hash == right.content_hash;
}

void SubsController::RecordCurrentFileSnapshot() {
	last_known_file_snapshot = filename.empty() ? std::nullopt : std::make_optional(MakeFileWatchSnapshot(filename));
	external_file_change_pending = false;
}

bool SubsController::HasFileChangedOnDisk() const {
	if (!reload_external_changes::ShouldCheckExternalFileSnapshot(
		tracks_external_file_state,
		OPT_GET("App/Auto/Reload External Changes")->GetBool(),
		!filename.empty(),
		last_known_file_snapshot.has_value()))
		return false;

	auto current_snapshot = MakeFileWatchSnapshot(filename);
	return !FileWatchSnapshotsEqual(current_snapshot, *last_known_file_snapshot);
}

void SubsController::OnWatchedFileChanged(agi::fs::path const&) {
	if (context->lua_workspace_invocation_active) {
		external_file_change_pending = true;
		return;
	}
	ApplyCompletedSaveEveryChangeWrites();
	if (filename.empty())
		return;
	// Option may have been disabled while a debounce/callback was already queued,
	// or while a modal prompt was open (multi-frame Preferences Apply).
	if (!OPT_GET("App/Auto/Reload External Changes")->GetBool())
		return;

	if (external_file_prompt_active) {
		external_file_change_pending = true;
		return;
	}

	int prompt_count = 0;
	constexpr int kMaxPromptLoopCount = 5;

	for (;;) {
		if (!OPT_GET("App/Auto/Reload External Changes")->GetBool())
			return;

		external_file_change_pending = false;
		auto current_snapshot = MakeFileWatchSnapshot(filename);

		if (last_known_file_snapshot && FileWatchSnapshotsEqual(current_snapshot, *last_known_file_snapshot))
			return;

		if (last_prompted_file_snapshot && FileWatchSnapshotsEqual(current_snapshot, *last_prompted_file_snapshot))
			return;

		if (++prompt_count > kMaxPromptLoopCount) {
			LOG_W("subs_controller") << "File change detection loop limit reached for " << agi::fs::PathToString(filename);
			last_prompted_file_snapshot = current_snapshot;
			return;
		}

		external_file_prompt_active = true;
		auto clear_prompt_flag = agi::make_scope_exit([this] {
			external_file_prompt_active = false;
		});

		bool const reload = PromptReloadAfterExternalChange(current_snapshot);
		bool const has_pending = external_file_change_pending;
		bool const option_still_on = OPT_GET("App/Auto/Reload External Changes")->GetBool();

		using reload_external_changes::AfterPromptAction;
		using reload_external_changes::PlanAfterPrompt;
		switch (PlanAfterPrompt(reload, option_still_on, has_pending)) {
		case AfterPromptAction::Reload:
			// Honor an explicit Yes even if detection was turned off while the
			// modal was open. The option gates future watch/prompt only.
			ReloadFileFromDisk(false);
			return;
		case AfterPromptAction::StampPromptedAndContinue:
			last_prompted_file_snapshot = current_snapshot;
			break;
		case AfterPromptAction::StampPromptedAndStop:
			last_prompted_file_snapshot = current_snapshot;
			return;
		}
	}
}

void SubsController::ProcessPendingExternalChange() {
	if (external_file_change_pending && !context->lua_workspace_invocation_active)
		OnWatchedFileChanged(filename);
}

void SubsController::OnFileWatchError(std::string const& message) {
	LOG_W("subs_controller") << "Subtitle file watcher error: " << message;
}

bool SubsController::PromptReloadAfterExternalChange(FileWatchSnapshot const& current_snapshot) {
	if (!current_snapshot.exists) {
		context->ShowWarning(
			from_wx(_("The subtitle file was deleted or moved by another program.\n\nThe current subtitles will be kept in memory. If you save, the file will be recreated.")),
			from_wx(_("Subtitle file changed")));
		saved_commit_id = -1;
		UpdateTitleAfterExternalChange();
		return false;
	}

	auto message = from_wx(_("The subtitle file has been modified by another program.\n\nDo you want to reload it?"));
	if (IsModified()) {
		message += "\n\n";
		message += from_wx(_("You have unsaved edits in Aegisub. Reloading will discard the current in-memory changes."));
	}
	message += "\n\n";
	message += from_wx(_("Reloading only updates the subtitle script. The currently loaded audio, video, timecodes, and keyframes will be kept."));

	auto const result = context->RequestInteraction({
		from_wx(_("Subtitle file changed")),
		message,
		agi::InteractionButtons::YesNo,
		IsModified() ? agi::InteractionIcon::Warning : agi::InteractionIcon::Question
	});
	return result == agi::InteractionResult::Yes;
}

bool SubsController::PromptOverwriteExternalChanges(agi::fs::path const& target) const {
	std::string message;
	if (agi::fs::FileExists(target)) {
		message = from_wx(_("The subtitle file has been modified by another program since it was last loaded or saved.\n\nSaving now will overwrite those external changes. Continue?"));
	} else {
		message = from_wx(_("The subtitle file was deleted or moved by another program since it was last loaded or saved.\n\nSaving now will recreate it. Continue?"));
	}

	auto const result = context->RequestInteraction({
		from_wx(_("Subtitle file changed")),
		message,
		agi::InteractionButtons::YesNo,
		agi::InteractionIcon::Warning
	});
	return result == agi::InteractionResult::Yes;
}

bool SubsController::ConfirmOverwriteExternalChanges(agi::fs::path const& target) const {
	if (!HasFile() || target != filename || !HasFileChangedOnDisk())
		return true;
	return PromptOverwriteExternalChanges(target);
}

bool SubsController::ConfirmOverwriteExternalChangesForAsync(
		agi::fs::path const& target,
		std::optional<FileWatchSnapshot>& expected_target) const {
	expected_target.reset();
	if (!HasFile() || target != filename)
		return true;

	if (!reload_external_changes::ShouldCheckExternalFileSnapshot(
		tracks_external_file_state,
		OPT_GET("App/Auto/Reload External Changes")->GetBool(),
		!filename.empty(),
		last_known_file_snapshot.has_value()))
		return true;

	// The common path only reads metadata on the UI thread. The worker hashes
	// the target immediately before publishing its staging file and compares it
	// with this trusted baseline, retaining same-size/same-time protection.
	auto const current_metadata = MakeFileWatchSnapshot(target, false);
	if (FileWatchMetadataEqual(current_metadata, *last_known_file_snapshot)
			&& last_known_file_snapshot->hash_valid) {
		expected_target = last_known_file_snapshot;
		return true;
	}

	if (!PromptOverwriteExternalChanges(target))
		return false;

	// Metadata changes are rare and already require user interaction. Hash once
	// after confirmation so the worker can still detect another intervening edit.
	expected_target = MakeFileWatchSnapshot(target);
	return true;
}

void SubsController::UpdateTitleAfterExternalChange() {
	auto const ui = context->GetUI();
	if (ui.frame)
		ui.frame->UpdateTitle();
}

void SubsController::ReloadFileFromDisk(bool load_linked_files) {
	auto core = context->GetCore();
	if (!core.project->ReloadSubtitles(filename, "", load_linked_files))
		return;

	context->ShowStatus(from_wx(_("Subtitles reloaded from disk.")));
}

bool SubsController::CanSave() const {
	try {
		auto core = context->GetCore();
		return SubtitleFormat::GetWriter(filename)->CanSave(core.ass.get());
	}
	catch (...) {
		return false;
	}
}

void SubsController::SetFileName(agi::fs::path const& path) {
	filename = path;
	context->GetCore().path->SetToken("?script", path.parent_path());
	config::mru->Add("Subtitle", path);
	OPT_SET("Path/Last/Subtitles")->SetString(agi::fs::PathToString(filename.parent_path()));
}

void SubsController::OnCommit(AssFileCommit c) {
	if (c.message.empty() && !undo_stack.empty()) return;
	auto const save_generation = BeginSaveEveryChangeRevision();

	auto core = context->GetCore();
	auto save_on_change = [&] {
		if (undo_stack.size() <= 1
				|| !OPT_GET("App/Auto/Save on Every Change")->GetBool()
				|| filename.empty())
			return;

		const SubtitleFormat *writer = nullptr;
		try {
			writer = SubtitleFormat::GetWriter(filename);
			if (!writer || !writer->CanSave(core.ass.get()))
				return;
		}
		catch (...) {
			return;
		}

		// Formats which may request a frame-rate or other UI choice retain the
		// proven synchronous path. Background-capable formats never call back to
		// the UI, which makes queue draining during close safe.
		if (!writer->SupportsBackgroundWriting()) {
			Save(filename);
			return;
		}

		std::optional<FileWatchSnapshot> expected_target;
		if (!ConfirmOverwriteExternalChangesForAsync(filename, expected_target))
			return;
		QueueSaveEveryChange(save_generation, writer, std::move(expected_target));
	};

	if (c.single_line && c.single_line->Group() == AssEntryGroup::DIALOGUE)
		core.selectionController->RecordEditedLine(c.single_line);

	commit_id = next_commit_id++;
	// Allow coalescing only if it's the last change and the file has not been
	// saved since the last change
	if (commit_id == *c.commit_id+1 && redo_stack.empty() && saved_commit_id+1 != commit_id) {
		auto const dialogue_only = c.type != AssFile::COMMIT_NEW
			&& (c.type & AssFile::COMMIT_DIAG_FULL) != 0
			&& (c.type & ~AssFile::COMMIT_DIAG_FULL) == 0;
		if (dialogue_only
			&& subs_controller_detail::TryAmendDialogueSnapshot(undo_stack.back().events, c.changed_lines)) {
			*c.commit_id = commit_id;
			save_on_change();
			return;
		}

		undo_stack.pop_back();
	}

	// Make sure the file has at least one style and one dialogue line
	if (core.ass->Styles.empty())
		core.ass->Styles.push_back(*new AssStyle);
	if (core.ass->Events.empty()) {
		core.ass->Events.push_back(*new AssDialogue);
		core.ass->Events.back().Row = 0;
	}

	redo_stack.clear();

	undo_stack.emplace_back(context, c.message, commit_id);

	int depth = std::max<int>(OPT_GET("Limits/Undo Levels")->GetInt(), 2);
	while ((int)undo_stack.size() > depth)
		undo_stack.pop_front();

	save_on_change();

	*c.commit_id = commit_id;
}

void SubsController::OnActiveLineChanged() {
	if (!undo_stack.empty())
		undo_stack.back().UpdateActiveLine(context);
}

void SubsController::OnSelectionChanged() {
	if (!undo_stack.empty())
		undo_stack.back().UpdateSelection(context);
}

void SubsController::OnTextSelectionChanged() {
	if (!undo_stack.empty())
		undo_stack.back().UpdateTextSelection(context);
}

void SubsController::Undo() {
	if (undo_stack.size() <= 1) return;
	BeginSaveEveryChangeRevision();
	redo_stack.splice(redo_stack.end(), undo_stack, std::prev(undo_stack.end()));

	commit_id = undo_stack.back().commit_id;

	text_selection_connection.Block();
	undo_stack.back().Apply(context);
	text_selection_connection.Unblock();
}

void SubsController::Redo() {
	if (redo_stack.empty()) return;
	BeginSaveEveryChangeRevision();
	undo_stack.splice(undo_stack.end(), redo_stack, std::prev(redo_stack.end()));

	commit_id = undo_stack.back().commit_id;

	text_selection_connection.Block();
	undo_stack.back().Apply(context);
	text_selection_connection.Unblock();
}

std::string SubsController::GetUndoDescription() const {
	return IsUndoStackEmpty() ? std::string() : undo_stack.back().undo_description;
}

std::string SubsController::GetRedoDescription() const {
	return IsRedoStackEmpty() ? std::string() : redo_stack.back().undo_description;
}

agi::fs::path SubsController::Filename() const {
	if (!filename.empty()) return filename;

	// Apple HIG says "untitled" should not be capitalised
#ifndef __WXMAC__
	return _("Untitled").wx_str();
#else
	return _("untitled").wx_str();
#endif
}
