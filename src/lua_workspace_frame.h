#pragma once

#include "automation/lua_workspace_document.h"
#include "automation/lua_workspace_run.h"
#include "automation/automation_debug_session.h"

#include <libaegisub/signal.h>

#include <wx/frame.h>

#include <cstdint>
#include <memory>
#include <string>

class wxButton;
class wxCheckBox;
class wxStaticText;
class wxStyledTextCtrl;
class wxTextCtrl;
class wxListBox;
class wxNotebook;
class wxSplitterWindow;
class wxTimer;
class wxTreeCtrl;
namespace agi {
struct Context;
}
namespace Automation4 {
struct AutomationInvocation;
class AutomationInvocationObserver;
}

struct LuaWorkspaceRuntimeObservation;
class LuaWorkspaceLanguage;

class LuaWorkspaceFrame : public wxFrame {
	agi::Context *context;
	std::unique_ptr<Automation4::LuaWorkspaceDocument> document;
	Automation4::LuaWorkspaceDocument const *pending_discard_document = nullptr;
	std::uint64_t pending_discard_revision = 0;
	wxStyledTextCtrl *editor = nullptr;
	wxSplitterWindow *source_splitter = nullptr;
	wxSplitterWindow *output_splitter = nullptr;
	bool restoring_splitters = false;
	bool source_drag_pending = false;
	bool output_drag_pending = false;
	std::unique_ptr<LuaWorkspaceLanguage> language;
	wxButton *open_button = nullptr;
	wxButton *apply = nullptr;
	wxButton *format = nullptr;
	wxButton *reload = nullptr;
	wxStaticText *diagnostics = nullptr;
	wxTextCtrl *runtime_context = nullptr;
	wxTextCtrl *generated_output = nullptr;
	wxNotebook *runtime_tabs = nullptr;
	wxStyledTextCtrl *execution_source = nullptr;
	wxStaticText *execution_identity = nullptr;
	wxListBox *stack_frames = nullptr;
	wxTreeCtrl *debug_variables = nullptr;
	wxTextCtrl *variable_details = nullptr;
	wxStaticText *debug_location = nullptr;
	wxCheckBox *pause_on_entry = nullptr;
	int execution_tab_index = -1;
	int stack_tab_index = -1;
	bool rebuilding_debug_tree = false;
	wxTextCtrl *run_log = nullptr;
	wxStaticText *run_status = nullptr;
	wxButton *run_button = nullptr;
	wxButton *debug_button = nullptr;
	wxButton *continue_button = nullptr;
	wxButton *pause_button = nullptr;
	wxButton *step_in_button = nullptr;
	wxButton *step_over_button = nullptr;
	wxButton *step_out_button = nullptr;
	wxButton *stop_button = nullptr;
	wxButton *detach_button = nullptr;
	wxTimer *debug_timer = nullptr;
	std::shared_ptr<Automation4::LuaWorkspaceRunRequest> active_run_request;
	std::shared_ptr<Automation4::AutomationDebugSession> active_session;
	std::shared_ptr<Automation4::LuaWorkspaceSourceRegistry> last_sources;
	std::optional<Automation4::AutomationDebugStateSnapshot> last_debug_snapshot;
	std::uint64_t next_run_id = 0;
	std::size_t last_debug_version = 0;
	bool close_after_run = false;
	std::shared_ptr<void> observation_lifetime = std::make_shared<char>();
	std::shared_ptr<LuaWorkspaceRuntimeObservation> active_observation;
	std::uint64_t invocation_sequence = 0;
	bool loading = false;
	std::optional<Automation4::LuaSourceDiagnostic> source_diagnostic;
	std::optional<std::uint64_t> no_macro_revision;
	std::string action_message;
	Automation4::LuaWorkspaceDocumentResult target_state;
	agi::signal::Connection commit_connection;
	agi::signal::Connection file_connection;
	agi::signal::Connection editor_font_face_connection;
	agi::signal::Connection editor_font_size_connection;
	agi::signal::Connection editor_wrap_connection;

	void SetEditorSource();
	void ApplyEditorPreferences();
	void ClearInvocationPresentation();
	void RefreshDocument(bool check_target = true);
	void ShowResult(Automation4::LuaWorkspaceDocumentResult const& result, bool remember = true);
	bool SaveDocument();
	bool ReloadDocument();
	void FormatDocument();
	void CopySource();
	bool FinishOpen(std::unique_ptr<Automation4::LuaWorkspaceDocument> candidate, Automation4::LuaWorkspaceDocumentResult const& result);
	void ClearRuntimeObservation();
	void RenderRuntimeObservation(std::uint64_t sequence, std::weak_ptr<LuaWorkspaceRuntimeObservation> const& observation);
	void StartRun(bool debug);
	void UpdateRunControls();
	void PollDebugState();
	void ShowSelectedFrame();
	void RefreshExecutionIdentity();
	bool UpdatePausedEditorMarker(Automation4::AutomationDebugStateSnapshot const& state);
	void RefreshVariableDetails(bool live);
	void ToggleBreakpoint(int line);
	std::vector<Automation4::AutomationDebugBreakpoint> CaptureBreakpoints(std::string const& source_uri) const;

	public:
	explicit LuaWorkspaceFrame(agi::Context *context);
	~LuaWorkspaceFrame() override;
	bool OpenCurrentLine();
	bool OpenFile(agi::fs::path const& filename);
	bool PrepareToClose(bool defer_discard = false);
	bool HasPendingDiscard() const;
	void FinishPendingDiscard(bool commit);
	bool IsDirty() const;
	void DetachContext();
	std::shared_ptr<Automation4::AutomationInvocationObserver> BeginInvocationObservation(Automation4::AutomationInvocation const& invocation);
	std::shared_ptr<Automation4::LuaWorkspaceRunRequest const> GetWorkspaceRunRequest(Automation4::AutomationInvocation const& invocation) const;
	std::shared_ptr<Automation4::LuaWorkspaceRunRequest const> GetActiveRunRequest() const;
	bool IsInvocationRunning() const;
	void ReportRunProgress(std::uint64_t invocation_id, std::string const& text);
};
