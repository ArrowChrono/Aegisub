#pragma once

#include "automation/lua_language_environment.h"
#include "automation/lua_language_server.h"

#include <wx/event.h>
#include <wx/timer.h>

#include <memory>
#include <optional>

class wxNotebook;
class wxStaticText;
class wxStyledTextCtrl;
class wxStyledTextEvent;
class wxTextCtrl;
namespace Automation4 {
class LuaWorkspaceDocument;
}

class LuaWorkspaceLanguage final : public wxEvtHandler {
	wxStyledTextCtrl *editor;
	wxStaticText *status;
	wxTextCtrl *diagnostics;
	wxTimer timer;
	Automation4::LuaLanguageServer server;
	Automation4::LuaLanguageConfiguration configuration;
	Automation4::LuaHostHints host_hints;
	std::optional<Automation4::LuaLanguageDocument> document;
	std::optional<unsigned> scopes;
	std::uint64_t generation = 0;
	std::uint64_t completion_request = 0;
	std::uint64_t tip_request = 0;
	Automation4::LuaLanguageRequest tip_kind = Automation4::LuaLanguageRequest::Hover;
	int tip_position = 0;
	std::optional<Automation4::LuaLanguageEvent> completion;
	int selection_start = 0;
	int selection_end = 0;
	bool active = false;
	bool enabled = false;
	bool applying = false;
	bool completion_refresh_pending = false;
	bool local_available = false;
	bool configuration_error = false;
	std::uint64_t local_request_id = 0;
	std::string completion_identity;
	int completion_position = 0;
	std::string directory_option;
	std::string includes_option;

	void Synchronize();
	void Invalidate();
	void ClearDiagnostics();
	void ClearTip();
	void Request(Automation4::LuaLanguageRequest kind, int position);
	std::string HostReceiver(int end) const;
	int HostCallOpen(int position) const;
	void PresentCompletion(Automation4::LuaLanguageEvent event);
	void PresentTip(Automation4::LuaLanguageEvent const& event);
	void AfterCharacter(int character);
	void OnTimer(wxTimerEvent& event);
	void OnCharHook(wxKeyEvent& event);
	void OnChar(wxKeyEvent& event);
	void OnCharacter(wxStyledTextEvent& event);
	void OnSelection(wxStyledTextEvent& event);
	void OnUpdateUI(wxStyledTextEvent& event);
	void OnDwellStart(wxStyledTextEvent& event);
	void OnDwellEnd(wxStyledTextEvent& event);

	public:
	LuaWorkspaceLanguage(wxStyledTextCtrl *editor, wxNotebook *notebook);
	~LuaWorkspaceLanguage() override;
	void Update(Automation4::LuaWorkspaceDocument const *document, unsigned scopes);
	void SetActive(bool value);
};
