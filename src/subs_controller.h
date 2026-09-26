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

#include <libaegisub/fs_fwd.h>
#include <libaegisub/signal.h>

#include "ass_dialogue.h"
#include "subs_controller_timer.h"

#include <boost/container/list.hpp>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

class SelectionController;
class SubtitleFormat;
class WatchedFile;
namespace agi {
	namespace dispatch {
		class Queue;
	}
	struct Context;
}
struct AssFileCommit;
struct ProjectProperties;

namespace subs_controller_detail {
	template<class Request>
	class LatestSaveState {
		std::optional<Request> pending;
		uint64_t generation = 0;
		bool stopping = false;

	public:
		uint64_t BeginRevision(std::optional<Request> *discarded = nullptr) {
			if (discarded)
				discarded->swap(pending);
			else
				pending.reset();
			return ++generation;
		}

		bool Submit(uint64_t request_generation, Request request,
				std::optional<Request> *discarded = nullptr) {
			if (stopping || request_generation != generation)
				return false;
			if (discarded)
				discarded->swap(pending);
			else
				pending.reset();
			pending.emplace(std::move(request));
			return true;
		}

		std::optional<Request> TakePending() {
			std::optional<Request> request;
			request.swap(pending);
			return request;
		}

		bool CanPublish(uint64_t request_generation) const {
			return !stopping && request_generation == generation;
		}

		bool IsStopping() const { return stopping; }
		uint64_t Generation() const { return generation; }

		void Stop() {
			stopping = true;
			pending.reset();
			++generation;
		}
	};

	inline bool TryAmendDialogueSnapshot(
		std::vector<AssDialogueBase>& snapshot,
		std::span<AssDialogue const *const> changed_lines) {
		if (changed_lines.empty())
			return false;

		for (auto const line : changed_lines) {
			if (!line || line->Row < 0 || static_cast<size_t>(line->Row) >= snapshot.size())
				return false;

			auto const& previous = snapshot[static_cast<size_t>(line->Row)];
			if (previous.Row != line->Row || previous.Id != line->Id)
				return false;
		}

		for (auto const line : changed_lines)
			snapshot[static_cast<size_t>(line->Row)] = *line;
		return true;
	}
}

class SubsController {
	agi::Context *context;
	agi::signal::Connection undo_connection;
	agi::signal::Connection active_line_connection;
	agi::signal::Connection selection_connection;
	agi::signal::Connection text_selection_connection;

	struct UndoInfo;
	boost::container::list<UndoInfo> undo_stack;
	boost::container::list<UndoInfo> redo_stack;

	/// Revision counter for undo coalescing and modified state tracking
	int commit_id = 0;
	/// Last saved version of this file
	int saved_commit_id = 0;
	/// Last autosaved version of this file
	int autosaved_commit_id = 0;
	/// Version to use for the next commit
	/// Needed to handle Save -> Undo -> Edit, which would result in the file
	/// being marked unmodified if we reused commit IDs
	int next_commit_id = 1;

	/// Timer for triggering autosaves on GUI shells only.
	std::unique_ptr<SubsControllerTimer> autosave_timer;
	int autosave_inhibit_depth = 0;

	/// Queue which autosaves are performed on
	std::unique_ptr<agi::dispatch::Queue> autosave_queue;

	struct SaveEveryChangeState;
	std::shared_ptr<SaveEveryChangeState> save_every_change_state;

	struct FileWatchSnapshot {
		bool exists = false;
		uintmax_t size = 0;
		time_t modified_time = 0;
		bool hash_valid = false;
		uint64_t content_hash = 0;
	};

	std::unique_ptr<WatchedFile> file_watch;
	std::optional<FileWatchSnapshot> last_known_file_snapshot;
	std::optional<FileWatchSnapshot> last_prompted_file_snapshot;
	bool external_file_change_pending = false;
	bool external_file_prompt_active = false;
	/// GUI shells only: permits external-change snapshots and live watching when
	/// the preference is enabled. Never inferred from file_watch==nullptr.
	bool tracks_external_file_state = false;

	// Declared after the state they touch so they disconnect first on destruction
	// (members are destroyed in reverse declaration order).
	agi::signal::Connection reload_external_changes_connection;
	agi::signal::Connection autosave_enable_connection;
	agi::signal::Connection autosave_interval_connection;

	/// A new file has been opened (filename, is_reload)
	/// is_reload is true when the file was reloaded from disk due to an
	/// external modification rather than freshly opened.
	agi::signal::Signal<agi::fs::path, bool> FileOpen;
	/// The file has been saved
	agi::signal::Signal<> FileSave;

	/// The filename of the currently open file, if any
	agi::fs::path filename;
	std::uint64_t document_generation = 0;

	/// Set the filename, updating things like the MRU and last used path
	void SetFileName(agi::fs::path const& file);

	/// Autosave the file if there have been any chances since the last autosave
	void AutoSave();
	uint64_t BeginSaveEveryChangeRevision();
	void ApplyCompletedSaveEveryChangeWrites();
	void WaitForSaveEveryChangeWrites(bool invalidate);
	void QueueSaveEveryChange(uint64_t generation, const SubtitleFormat *writer,
		std::optional<FileWatchSnapshot> expected_target);
	static void DrainSaveEveryChangeQueue(std::shared_ptr<SaveEveryChangeState> state);

	void UpdateFileWatch();
	void ClearFileWatch();
	/// Arm or disarm external-change detection to match the current option.
	/// Safe to call when the option is toggled at runtime (no restart required).
	void ApplyReloadExternalChangesOption();
	FileWatchSnapshot MakeFileWatchSnapshot(agi::fs::path const& path, bool include_hash = true) const;
	static bool FileWatchMetadataEqual(FileWatchSnapshot const& left, FileWatchSnapshot const& right);
	static bool FileWatchSnapshotsEqual(FileWatchSnapshot const& left, FileWatchSnapshot const& right);
	void RecordCurrentFileSnapshot();
	bool HasFileChangedOnDisk() const;
	void OnWatchedFileChanged(agi::fs::path const& path);
	void OnFileWatchError(std::string const& message);
	bool PromptReloadAfterExternalChange(FileWatchSnapshot const& current_snapshot);
	bool PromptOverwriteExternalChanges(agi::fs::path const& target) const;
	bool ConfirmOverwriteExternalChanges(agi::fs::path const& target) const;
	bool ConfirmOverwriteExternalChangesForAsync(
		agi::fs::path const& target,
		std::optional<FileWatchSnapshot>& expected_target) const;
	void UpdateTitleAfterExternalChange();
	void ReloadFileFromDisk(bool load_linked_files);

	void OnCommit(AssFileCommit c);
	void OnActiveLineChanged();
	void OnSelectionChanged();
	void OnTextSelectionChanged();

public:
	class AutosaveInhibitor {
		SubsController *controller = nullptr;
		explicit AutosaveInhibitor(SubsController *controller);
		friend class SubsController;

	public:
		AutosaveInhibitor(AutosaveInhibitor const&) = delete;
		AutosaveInhibitor& operator=(AutosaveInhibitor const&) = delete;
		AutosaveInhibitor(AutosaveInhibitor&& other) noexcept;
		AutosaveInhibitor& operator=(AutosaveInhibitor&&) = delete;
		~AutosaveInhibitor();
	};

	SubsController(agi::Context *context);
	~SubsController();

	/// Temporarily prevent the periodic autosave timer from snapshotting
	/// transient document state. Nested inhibitors are supported.
	AutosaveInhibitor InhibitAutosave();

	/// Set the selection controller to use
	///
	/// Required due to that the selection controller is the subtitles grid, and
	/// so is created long after the subtitles controller
	void SetSelectionController(SelectionController *selection_controller);

	/// The file's path and filename if any, or platform-appropriate "untitled"
	agi::fs::path Filename() const;
	bool HasFile() const { return !filename.empty(); }

	/// Does the file have unsaved changes?
	bool IsModified() const { return commit_id != saved_commit_id; };
	/// Current subtitle document revision used by snapshot/transaction clients.
	int64_t GetDocumentRevision() const { return commit_id; }
	[[nodiscard]] std::uint64_t GetDocumentGeneration() const { return document_generation; }

	/// @brief Load from a file
	/// @param file File name
	/// @param charset Character set of file
	/// @param is_reload True when reloading the current file from disk after an
	///                  external modification, rather than opening a new file.
	ProjectProperties Load(agi::fs::path const& file, std::string charset, bool is_reload = false);

	/// @brief Save to a file
	/// @param file Path to save to
	/// @param encoding Encoding to use, or empty to use App/Save Charset
	void Save(agi::fs::path const& file, std::string const& encoding="");

	/// Close the currently open file (i.e. open a new blank file)
	void Close();

	/// If there are unsaved changes, asl the user if they want to save them
	/// @param allow_cancel Let the user cancel the closing
	/// @return wxYES, wxNO or wxCANCEL (note: all three are true in a boolean context)
	int TryToClose(bool allow_cancel = true);

	/// Can the file be saved in its current format?
	bool CanSave() const;

	/// The file is about to be saved
	/// This signal is intended for adding metadata which is awkward or
	/// expensive to always keep up to date
	agi::signal::Signal<> UpdateProperties;

	DEFINE_SIGNAL_ADDERS(FileOpen, AddFileOpenListener)
	DEFINE_SIGNAL_ADDERS(FileSave, AddFileSaveListener)
	DEFINE_SIGNAL_ADDERS(UpdateProperties, AddUpdatePropertiesListener)

	/// @brief Undo the last set of changes to the file
	void Undo();
	/// @brief Redo the last undone changes
	void Redo();
	/// Check if undo stack is empty
	bool IsUndoStackEmpty() const { return undo_stack.size() <= 1; };
	/// Check if redo stack is empty
	bool IsRedoStackEmpty() const { return redo_stack.empty(); };
	/// Get the description of the first undoable change
	std::string GetUndoDescription() const;
	/// Get the description of the first redoable change
	std::string GetRedoDescription() const;
};
