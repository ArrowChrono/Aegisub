// Copyright (c) 2006, Niels Martin Hansen
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

#include "auto4_base.h"
#include "automation/automation_invocation.h"

#include <deque>
#include <memory>
#include <string_view>
#include <vector>
#include <wx/string.h>

class AssEntry;
class wxControl;
class wxWindow;
struct lua_State;

namespace Automation4 {
	struct LuaWorkspaceRunRequest;

	std::string_view GetLuaMacroExecutionId(cmd::Command const *command);
	bool ValidateLuaMacroForWorkspace(
		cmd::Command *command,
		agi::Context const *context,
		std::shared_ptr<LuaWorkspaceRunRequest const> const& request,
		BackgroundScriptRunner& runner);

	class AutomationMutationJournal;

	struct LuaDialogControlCreateTrace {
		double native_construct_ms = 0.0;
		double validator_bind_ms = 0.0;
		double initial_value_set_ms = 0.0;
		double floatedit_spin_construct_with_value_ms = 0.0;
		double floatedit_spin_validator_bind_ms = 0.0;
		double floatedit_text_construct_with_validator_ms = 0.0;
	};

	/// @class LuaAssFile
	/// @brief Object wrapping an AssFile object for modification through Lua
	class LuaAssFile {
		struct PendingCommit {
			wxString mesage;
			int modification_type;
			std::vector<AssEntry*> lines;
		};

		/// Pointer to file being modified
		AssFile *ass;

		/// Lua state the object exists in
		lua_State *L;

		/// Is the feature this object is created for read-only?
		bool can_modify;
		/// Is the feature allowed to set undo points?
		bool can_set_undo;
		/// throws an error if modification is disallowed
		void CheckAllowModify();
		/// throws an error if the line index is out of bounds
		void CheckBounds(int idx);

		/// How ass file been modified by the script since the last commit
		int modification_type = 0;

		/// Script-owned operations are no longer allowed once processing
		/// completes, even though Lua userdata may still keep the object alive.
		bool script_reference_active = true;
		bool workspace_override_applied = false;

		/// Set of subtitle lines being modified; initially a shallow copy of ass->Line
		std::vector<AssEntry*> lines;
		bool script_info_copied = false;

		/// Commits to apply once processing completes successfully
		std::deque<PendingCommit> pending_commits;
		/// Lines to delete once processing complete successfully
		std::vector<std::unique_ptr<AssEntry>> lines_to_delete;
		/// Host references are kept alive for the full LuaAssFile lifetime.
		std::shared_ptr<AutomationHost> automation_host;
		std::shared_ptr<AutomationMutationJournal> mutation_journal;

		/// Create copies of all of the lines in the script info section if it
		/// hasn't already happened. This is done lazily, since it only needs
		/// to happen when the user modifies the headers in some way, which
		/// most runs of a script will not do.
		void InitScriptInfoIfNeeded();
		/// Add the line at the given index to the list of lines to be deleted
		/// when the script completes, unless it's an AssInfo, since those are
		/// owned by the container.
		void QueueLineForDeletion(size_t idx);
		/// Set the line at the index to the given value
		void AssignLine(size_t idx, std::unique_ptr<AssEntry> e);
		void InsertLine(std::vector<AssEntry *> &vec, size_t idx, std::unique_ptr<AssEntry> e);
		/// Mark the script-owned reference as completed without relying on self-delete.
		void ReleaseScriptReference();
		void RegisterMiscFunctions();

		int ObjectIndexRead(lua_State *L);
		void ObjectIndexWrite(lua_State *L);
		int ObjectGetLen(lua_State *L);
		void ObjectDelete(lua_State *L);
		void ObjectDeleteRange(lua_State *L);
		void ObjectAppend(lua_State *L);
		void ObjectInsert(lua_State *L);
		int ObjectIPairs(lua_State *L);
		int IterNext(lua_State *L);

		int LuaParseKaraokeData(lua_State *L);
		int LuaGetScriptResolution(lua_State *L);
		int LuaWorkspaceSourceIdentity(lua_State *L);
		int LuaWorkspaceCodeSource(lua_State *L);

		void LuaSetUndoPoint(lua_State *L);
	public:
		~LuaAssFile();
		static std::shared_ptr<LuaAssFile> Create(lua_State *L, AssFile *ass, bool can_modify = false, bool can_set_undo = false);
		static LuaAssFile *GetObjPointer(lua_State *L, int idx, bool allow_expired);
		size_t DebugLineCount() const { return lines.size(); }
		size_t DebugInfoCount() const;
		size_t DebugStyleCount() const;
		size_t DebugDialogueCount() const;
		size_t DebugPendingCommitCount() const { return pending_commits.size(); }
		bool DebugCanModify() const { return can_modify; }
		bool DebugCanSetUndo() const { return can_set_undo; }
		bool DebugHasPendingModifications() const { return modification_type != 0; }
		bool DebugTryPushLineAsLua(lua_State *L, size_t automation_row);
		[[nodiscard]] bool WorkspaceOverrideApplied() const { return workspace_override_applied; }

		/// makes a Lua representation of AssEntry and places on the top of the stack
		void AssEntryToLua(lua_State *L, size_t idx);
		/// assumes a Lua representation of AssEntry on the top of the stack, and creates an AssEntry object of it
		static std::unique_ptr<AssEntry> LuaToAssEntry(lua_State *L, AssFile *ass=nullptr);

		/// @brief Signal that the script using this file is now done running
		/// @param set_undo If there's any uncommitted changes to the file,
		///                 they will be automatically committed with this
		///                 description
		std::vector<AssEntry *> ProcessingComplete(wxString const& undo_description = wxString());

		/// End processing without applying any changes made
		void Cancel();

		LuaAssFile(lua_State *L, AssFile *ass, bool can_modify = false, bool can_set_undo = false);
	};

	class LuaProgressSink {
		lua_State *L;

		static int LuaSetProgress(lua_State *L);
		static int LuaSetTask(lua_State *L);
		static int LuaSetTitle(lua_State *L);
		static int LuaGetCancelled(lua_State *L);
		static int LuaDebugOut(lua_State *L);
		static int LuaDisplayDialog(lua_State *L);
		static int LuaDisplayOpenDialog(lua_State *L);
		static int LuaDisplaySaveDialog(lua_State *L);

	public:
		LuaProgressSink(lua_State *L, ProgressSink *ps, AutomationInvocation const& invocation);
		~LuaProgressSink();

		static ProgressSink* GetObjPointer(lua_State *L, int idx);
	};

	/// Base class for controls in dialogs
	class LuaDialogControl {
		char const* trace_type;
	protected:
		LuaDialogControlCreateTrace create_trace;
	public:
		/// Name of this control in the output table
		std::string name;

		/// Tooltip of this control
		std::string hint;
		wxString hint_wx;

		int x, y, width, height;

		/// Create the associated wxControl
		virtual wxControl *Create(wxWindow *parent) = 0;

		/// Get the default flags to use when inserting this control into a sizer
		virtual int GetSizerFlags() const { return wxEXPAND; }

		char const* GetTraceType() const { return trace_type; }
		virtual int GetTraceItemCount() const { return 0; }
		LuaDialogControlCreateTrace const& GetCreateTrace() const { return create_trace; }

		/// Push the current value of the control onto the lua stack. Must not
		/// touch the GUI as this may be called on a background thread.
		virtual void LuaReadBack(lua_State *L) = 0;

		/// Does this control have any user-changeable data that can be serialized?
		virtual bool CanSerialiseValue() const { return false; }

		/// Serialize the control's current value so that it can be stored
		/// in the script
		virtual std::string SerialiseValue() const { return ""; }

		/// Restore the control's value from a saved value in the script
		virtual void UnserialiseValue(const std::string &serialised) { }

		LuaDialogControl(lua_State *L, char const* trace_type);

		/// Virtual destructor so this can safely be inherited from
		virtual ~LuaDialogControl() = default;
	};

	/// A lua-generated dialog or panel in the export options dialog
	class LuaDialog final : public ScriptDialog {
		/// Controls in this dialog
		std::vector<std::unique_ptr<LuaDialogControl>> controls;
		/// The names and IDs of buttons in this dialog if non-default ones were used
		std::vector<std::pair<int, std::string>> buttons;

		/// Does the dialog contain any buttons
		bool use_buttons;

		/// Id of the button pushed (once a button has been pushed)
		int button_pushed = -1;

		wxWindow *window = nullptr;

	public:
		LuaDialog(lua_State *L, bool include_buttons);

		/// Push the values of the controls in this dialog onto the lua stack
		/// in a single table
		int LuaReadBack(lua_State *L);

		// ScriptDialog implementation
		wxWindow* CreateWindow(wxWindow *parent) override;
		std::string Serialise() override;
		void Unserialise(const std::string &serialised) override;
	};
}
