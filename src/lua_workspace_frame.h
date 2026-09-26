#pragma once

#include "automation/lua_workspace_document.h"

#include <libaegisub/signal.h>

#include <wx/frame.h>

#include <cstdint>
#include <memory>
#include <string>

class wxButton;
class wxStaticText;
class wxStyledTextCtrl;
class wxTextCtrl;
namespace agi {
struct Context;
}
namespace Automation4 {
struct AutomationInvocation;
class AutomationInvocationObserver;
}

struct LuaWorkspaceRuntimeObservation;

class LuaWorkspaceFrame : public wxFrame {
	agi::Context *context;
	std::unique_ptr<Automation4::LuaWorkspaceDocument> document;
	Automation4::LuaWorkspaceDocument const *pending_discard_document = nullptr;
	std::uint64_t pending_discard_revision = 0;
	wxStyledTextCtrl *editor = nullptr;
	wxButton *apply = nullptr;
	wxButton *format = nullptr;
	wxButton *reload = nullptr;
	wxStaticText *diagnostics = nullptr;
	wxTextCtrl *runtime_context = nullptr;
	wxTextCtrl *generated_output = nullptr;
	std::shared_ptr<void> observation_lifetime = std::make_shared<char>();
	std::shared_ptr<LuaWorkspaceRuntimeObservation> active_observation;
	std::uint64_t invocation_sequence = 0;
	bool loading = false;
	std::optional<Automation4::LuaSourceDiagnostic> source_diagnostic;
	std::string action_message;
	Automation4::LuaWorkspaceDocumentResult target_state;
	agi::signal::Connection commit_connection;
	agi::signal::Connection file_connection;

	void SetEditorSource();
	void RefreshDocument(bool check_target = true);
	void ShowResult(Automation4::LuaWorkspaceDocumentResult const& result, bool remember = true);
	bool SaveDocument();
	bool ReloadDocument();
	void FormatDocument();
	void CopySource();
	bool FinishOpen(std::unique_ptr<Automation4::LuaWorkspaceDocument> candidate, Automation4::LuaWorkspaceDocumentResult const& result);
	void ClearRuntimeObservation();
	void RenderRuntimeObservation(std::uint64_t sequence, std::weak_ptr<LuaWorkspaceRuntimeObservation> const& observation);

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
};
