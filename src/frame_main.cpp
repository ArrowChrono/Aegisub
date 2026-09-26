// Copyright (c) 2005, Rodrigo Braz Monteiro, Niels Martin Hansen
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

/// @file frame_main.cpp
/// @brief Main window creation and control management
/// @ingroup main_ui

#include "frame_main.h"

#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/menu.h"
#include "include/aegisub/toolbar.h"
#include "include/aegisub/hotkey.h"

#include "ass_file.h"
#include "async_video_provider.h"
#include "audio_controller.h"
#include "audio_box.h"
#include "base_grid.h"
#include "compat.h"
#include "command/command.h"
#include "selection_controller.h"
#include "scoped_no_composited.h"
#include "dialog_detached_video.h"
#include "dialog_manager.h"
#include "font_family_catalog_cache.h"
#include "libresrc/libresrc.h"
#include "main.h"
#ifdef WITH_WXSTC
#include "lua_workspace_frame.h"
#endif
#include "options.h"
#include "pgs_sup_packet_stream.h"
#include "project.h"
#include "perf_trace.h"
#include "status_sink.h"
#include "subs_controller.h"
#include "subs_edit_box.h"
#include "ui_services.h"
#include "utils.h"
#include "version.h"
#include "vobsub_packet_stream.h"
#include "video_box.h"
#include "video_controller.h"
#include "video_display.h"
#include "wx_frame_main_dialog_ui_host.h"
#include "wx_frame_main_request_host.h"
#include "wx_frame_main_runtime_host.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <chrono>
#include <wx/dnd.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/sysopt.h>

#ifdef _WIN32
#include <dbt.h>
#include <windows.h>
#include <wtsapi32.h>
#endif

enum {
	ID_APP_TIMER_STATUSCLEAR = 12002
#ifdef _WIN32
	,ID_APP_TIMER_FONTCHANGE_DEBOUNCE
	,ID_APP_TIMER_AUDIO_OUTPUT_RECOVERY
#endif
};

#ifdef _WIN32
constexpr int kFontChangeDebounceDelayMs = 500;
constexpr int kAudioOutputRecoveryDebounceDelayMs = 750;

bool IsRelevantAudioDeviceChange(WXWPARAM wParam) {
	switch (static_cast<UINT>(wParam)) {
		case DBT_DEVNODES_CHANGED:
		case DBT_DEVICEARRIVAL:
		case DBT_DEVICEREMOVECOMPLETE:
			return true;
		default:
			return false;
	}
}

char const* DescribeAudioDeviceChange(WXWPARAM wParam) {
	switch (static_cast<UINT>(wParam)) {
		case DBT_DEVNODES_CHANGED: return "WM_DEVICECHANGE/DBT_DEVNODES_CHANGED";
		case DBT_DEVICEARRIVAL: return "WM_DEVICECHANGE/DBT_DEVICEARRIVAL";
		case DBT_DEVICEREMOVECOMPLETE: return "WM_DEVICECHANGE/DBT_DEVICEREMOVECOMPLETE";
		default: return "WM_DEVICECHANGE";
	}
}

bool IsRelevantSessionChange(WXWPARAM wParam) {
	switch (static_cast<DWORD>(wParam)) {
		case WTS_CONSOLE_CONNECT:
		case WTS_CONSOLE_DISCONNECT:
		case WTS_REMOTE_CONNECT:
		case WTS_REMOTE_DISCONNECT:
			return true;
		default:
			return false;
	}
}

bool IsRelevantGridSessionChange(WXWPARAM wParam) {
	if (IsRelevantSessionChange(wParam))
		return true;
	switch (static_cast<DWORD>(wParam)) {
		case WTS_SESSION_LOGON:
		case WTS_SESSION_LOGOFF:
		case WTS_SESSION_LOCK:
		case WTS_SESSION_UNLOCK:
		case WTS_SESSION_REMOTE_CONTROL:
			return true;
		default:
			return false;
	}
}

bool IsRelevantTextRasterSettingChange(WXWPARAM wParam) {
	switch (static_cast<UINT>(wParam)) {
		case SPI_SETFONTSMOOTHING:
		case SPI_SETFONTSMOOTHINGTYPE:
		case SPI_SETFONTSMOOTHINGCONTRAST:
		case SPI_SETFONTSMOOTHINGORIENTATION:
			return true;
		default:
			return false;
	}
}

char const* DescribeSessionChange(WXWPARAM wParam) {
	switch (static_cast<DWORD>(wParam)) {
		case WTS_CONSOLE_CONNECT: return "WM_WTSSESSION_CHANGE/WTS_CONSOLE_CONNECT";
		case WTS_CONSOLE_DISCONNECT: return "WM_WTSSESSION_CHANGE/WTS_CONSOLE_DISCONNECT";
		case WTS_REMOTE_CONNECT: return "WM_WTSSESSION_CHANGE/WTS_REMOTE_CONNECT";
		case WTS_REMOTE_DISCONNECT: return "WM_WTSSESSION_CHANGE/WTS_REMOTE_DISCONNECT";
		case WTS_SESSION_LOGON: return "WM_WTSSESSION_CHANGE/WTS_SESSION_LOGON";
		case WTS_SESSION_LOGOFF: return "WM_WTSSESSION_CHANGE/WTS_SESSION_LOGOFF";
		case WTS_SESSION_LOCK: return "WM_WTSSESSION_CHANGE/WTS_SESSION_LOCK";
		case WTS_SESSION_UNLOCK: return "WM_WTSSESSION_CHANGE/WTS_SESSION_UNLOCK";
		case WTS_SESSION_REMOTE_CONTROL: return "WM_WTSSESSION_CHANGE/WTS_SESSION_REMOTE_CONTROL";
		default: return "WM_WTSSESSION_CHANGE";
	}
}
#endif

#ifdef WITH_STARTUPLOG
#define StartupLog(a) agi::ShowFrameMainStartupLogDialog(wxS(a))
#else
#define StartupLog(a) LOG_I("frame_main/init") << a
#endif

FrameMain::FrameMain()
: wxFrame(nullptr, -1, wxEmptyString, wxDefaultPosition, wxSize(920,700), wxDEFAULT_FRAME_STYLE | wxCLIP_CHILDREN)
, context(agi::make_unique<agi::Context>())
{
	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase(
			"main",
			phase,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_started).count());
		phase_started = std::chrono::steady_clock::now();
	};

	SetSize(FromDIP(wxSize(1000, 700)));
	StartupLog("Entering FrameMain constructor");

#ifdef __WXGTK__
	// XXX HACK XXX
	// We need to set LC_ALL to "" here for input methods to work reliably.
	setlocale(LC_ALL, "");

	// However LC_NUMERIC must be "C", otherwise some parsing fails.
	setlocale(LC_NUMERIC, "C");
#endif

	StartupLog("Initializing context controls");
	auto core = context->GetCore();
	auto ui = context->GetUI();
	ui_activation.AddConnections(
		core.ass->AddCommitListener(&FrameMain::UpdateTitle, this),
		core.subsController->AddFileOpenListener(&FrameMain::OnSubtitlesOpen, this),
		core.subsController->AddFileSaveListener(&FrameMain::UpdateTitle, this),
		core.project->AddAudioProviderListener(&FrameMain::OnAudioOpen, this),
		core.project->AddVideoProviderListener(&FrameMain::OnVideoOpen, this));
	observe_phase("startup.frame.context.bind_core_listeners");

	StartupLog("Initializing context frames");
	ui.parent = this;
	ui.frame = this;
	core.statusSink = agi::MakeFrameMainStatusSink(
		[this](std::string const& message, int timeout_ms) {
			StatusTimeout(to_wx(message), timeout_ms);
		},
		[this](std::string const& command_name) {
			SetLastCommand(to_wx(command_name));
		},
		GetAsyncUiLifetime());
	selection_changed_connection = core.selectionController->AddSelectionListener(&FrameMain::OnSelectedSetChanged, this);
	core.notificationSink = agi::MakeFrameMainNotificationSink(this, GetAsyncUiLifetime());
	core.interactionSink = agi::MakeFrameMainInteractionSink(this, GetAsyncUiLifetime());
	core.singleChoiceInteractionSink = agi::MakeFrameMainSingleChoiceInteractionSink(this, GetAsyncUiLifetime());
	core.fileDialogService = agi::MakeFrameMainFileDialogService(this, GetAsyncUiLifetime());
	core.videoSourceRequestService = agi::MakeFrameMainVideoSourceRequestService(this, GetAsyncUiLifetime());
	core.backgroundRunnerFactory = agi::MakeFrameMainBackgroundRunnerFactory(this, GetAsyncUiLifetime());
	core.projectUiStateSink = agi::MakeFrameMainProjectUiStateSink(
		[context = context.get()](agi::ProjectUiStateSnapshot const& state) {
			auto ui = context->GetUI();
			if (state.subtitle_scroll_position && ui.subsGrid)
				ui.subsGrid->RestoreScrollPosition(*state.subtitle_scroll_position);
			if (state.video_zoom && ui.videoDisplay)
				ui.videoDisplay->SetZoom(*state.video_zoom);
		},
		GetAsyncUiLifetime());
	core.audioPlayerFactoryService = agi::MakeFrameMainAudioPlayerFactoryService(this, GetAsyncUiLifetime());
	core.automationBackgroundScriptRunnerFactory = agi::MakeFrameMainAutomationBackgroundScriptRunnerFactory(this, GetAsyncUiLifetime());
	observe_phase("startup.frame.context.install_ui_services");

	StartupLog("Set frame background for resize painting");
	SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE));

	StartupLog("Apply saved Maximized state");
	if (OPT_GET("App/Maximized")->GetBool()) Maximize(true);

	StartupLog("Initialize toolbar");
	wxSystemOptions::SetOption(wxS("msw.remap"), 0);
	OPT_SUB("App/Show Toolbar", &FrameMain::EnableToolBar, this);
	EnableToolBar(*OPT_GET("App/Show Toolbar"));
	observe_phase("startup.frame.toolbar.attach_or_hide");

	StartupLog("Initialize menu bar");
	menu::GetMenuBar("main", this, (wxID_HIGHEST + 1) + 10000, context.get());
	observe_phase("startup.frame.menu.attach");

	StartupLog("Create status bar");
	CreateStatusBar(4);
	if (auto *status_bar = GetStatusBar()) {
		// Reserve a stable narrow pane for the selected-lines count.
		int widths[] = { -4, -3, -3, FromDIP(140) };
		status_bar->SetStatusWidths(4, widths);
	}
	ui_activation.AddConnections(
		core.ass->AddCommitListener(&FrameMain::UpdateSelectionAnchorStatus, this),
		core.selectionController->AddSelectionAnchorListener(&FrameMain::UpdateSelectionAnchorStatus, this));
	UpdateSelectionAnchorStatus();

	StartupLog("Set icon");
#ifdef _WIN32
	SetIcon(wxICON(wxicon));
#else
	wxIcon icon;
	icon.CopyFromBitmap(GETIMAGE(wxicon));
	SetIcon(icon);
#endif

	StartupLog("Create views and inner main window controls");
	InitContents();
	OPT_SUB("Video/Detached/Enabled", &FrameMain::OnVideoDetach, this);
	observe_phase("startup.frame.contents.total");

	StartupLog("Set up drag/drop target");
	SetDropTarget(agi::MakeFrameMainFileDropTarget(
		[this](std::vector<agi::fs::path> const& files) {
			auto *ctx = context.get();
			if (!ctx)
				return;

			auto core = ctx->GetCore();
			if (!OPT_GET("Video/Secondary Subtitles/Enabled")->GetBool()) {
				core.project->LoadList(files);
				return;
			}

			auto is_subtitle_drop_file = [](agi::fs::path const& path) {
				// Match Project::LoadList text formats plus external bitmap formats.
				// Avoid containers such as MKV which can also be videos.
				return agi::fs::HasExtension(path, "ass")
					|| agi::fs::HasExtension(path, "ssa")
					|| agi::fs::HasExtension(path, "srt")
					|| agi::fs::HasExtension(path, "sub")
					|| IsVobSubIndexPath(path)
					|| IsPgsSupSubtitlePath(path)
					|| agi::fs::HasExtension(path, "ttxt");
			};

			std::vector<agi::fs::path> subtitle_files;
			std::vector<agi::fs::path> other_files;
			subtitle_files.reserve(files.size());
			other_files.reserve(files.size());
			for (auto const& file : files) {
				if (is_subtitle_drop_file(file))
					subtitle_files.push_back(file);
				else
					other_files.push_back(file);
			}

			if (subtitle_files.empty()) {
				core.project->LoadList(files);
				return;
			}

			auto same_stem = [](agi::fs::path left, agi::fs::path right) {
				left.replace_extension();
				right.replace_extension();
#ifdef _WIN32
				return agi::util::strings::utf8_iequals(
					agi::fs::PathToGenericString(left.lexically_normal()),
					agi::fs::PathToGenericString(right.lexically_normal()));
#else
				return left.lexically_normal() == right.lexically_normal();
#endif
			};

			auto selected_subtitle = subtitle_files.begin();
			if (agi::fs::HasExtension(*selected_subtitle, "sub")) {
				auto paired_index = std::find_if(
					subtitle_files.begin(), subtitle_files.end(), [&](agi::fs::path const& candidate) {
						return IsVobSubIndexPath(candidate)
							&& same_stem(candidate, *selected_subtitle);
					});
				if (paired_index != subtitle_files.end())
					selected_subtitle = paired_index;
			}

			auto logical_subtitle_count = subtitle_files.size();
			if (IsVobSubIndexPath(*selected_subtitle)) {
				logical_subtitle_count -= static_cast<size_t>(std::count_if(
					subtitle_files.begin(), subtitle_files.end(), [&](agi::fs::path const& candidate) {
						return agi::fs::HasExtension(candidate, "sub")
							&& same_stem(candidate, *selected_subtitle);
					}));
			}

			bool const selected_is_bitmap = IsVobSubIndexPath(*selected_subtitle)
				|| IsPgsSupSubtitlePath(*selected_subtitle);
			if (selected_is_bitmap) {
				if (!other_files.empty())
					core.project->LoadList(other_files);
				if (videoBox)
					videoBox->OpenSecondarySubtitlesFromPath(*selected_subtitle);
				if (logical_subtitle_count > 1) {
					ctx->ShowInfo(
						from_wx(_("Multiple subtitle files were dropped. Only the first one was loaded as secondary.")),
						from_wx(_("Secondary subtitles")));
				}
				return;
			}

			agi::SingleChoiceInteractionRequest request;
			request.title = from_wx(_("Dropped subtitles"));
			request.message = from_wx(_("Where do you want to load the dropped subtitle file?"));
			request.choices = {
				from_wx(_("Main subtitles")),
				from_wx(_("Secondary subtitles"))
			};
			request.default_choice = 0;
			request.request_id = "frame_main.drop_target.subtitle_destination";

			auto choice = ctx->RequestSingleChoice(request);
			if (!choice) {
				// Cancel: still load non-subtitle files (e.g. video), but skip the subtitles.
				if (!other_files.empty())
					core.project->LoadList(other_files);
				return;
			}

			if (*choice == 0) {
				core.project->LoadList(files);
				return;
			}

			if (!other_files.empty())
				core.project->LoadList(other_files);

			if (videoBox)
				videoBox->OpenSecondarySubtitlesFromPath(*selected_subtitle);

			if (logical_subtitle_count > 1) {
				ctx->ShowInfo(
					from_wx(_("Multiple subtitle files were dropped. Only the first one was loaded as secondary.")),
					from_wx(_("Secondary subtitles")));
			}
		},
		GetAsyncUiLifetime()));
	observe_phase("startup.frame.drag_drop.install");

	StartupLog("Load default file");
	core.project->CloseSubtitles();
	observe_phase("startup.frame.project.close_initial_subtitles");

	StartupLog("Display main window");
	AddFullScreenButton(this);
	Show();
	SetDisplayMode(1, 1);
#ifdef _WIN32
	RegisterSessionNotifications();
#endif
	// GUI automation scenarios do not exercise the font catalog. Avoid making
	// their deterministic shutdown wait on a potentially long system scan.
	if (!wxGetApp().launch_plan || wxGetApp().launch_plan->mode != AppLaunchMode::GuiTest)
		font_family_catalog_cache::WarmAsync();
	observe_phase("startup.frame.show");
	auto startup_lifetime = GetAsyncUiLifetime();
	auto main_loop_turn_started = std::chrono::steady_clock::now();
	CallAfter([startup_lifetime, main_loop_turn_started] {
		if (startup_lifetime.expired())
			return;
		perf_trace::ObserveWindowOpenPhase(
			"main",
			"startup.frame.first_main_loop_turn_after_show",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - main_loop_turn_started).count());
	});

	StartupLog("Leaving FrameMain constructor");
}

FrameMain::~FrameMain () {
	ui_activation.Deactivate();
#ifdef WITH_WXSTC
	if (lua_workspace) {
		lua_workspace->DetachContext();
		delete lua_workspace;
		lua_workspace = nullptr;
	}
#endif
	auto core = context->GetCore();
#ifdef _WIN32
	FontChangeDebounce.Stop();
	AudioOutputRecoveryDebounce.Stop();
	UnregisterSessionNotifications();
#endif
	// Tear the modeless dialogs down synchronously, before anything else:
	// their destructors dereference the context and swap visual tools on the
	// video display, so they must run while both are still alive. This has to
	// precede CloseVideo() as well — a video-close notification closes the
	// detached-video dialog, whose deferred destruction would then run after
	// the context is gone.
	context->GetUI().dialog.reset();

	core.project->CloseAudio();
	core.project->CloseVideo();

	DestroyChildren();
}

LuaWorkspaceFrame *FrameMain::GetLuaWorkspace(bool create) {
#ifdef WITH_WXSTC
	if (create && !lua_workspace)
		lua_workspace = new LuaWorkspaceFrame(context.get());
#else
	(void)create;
#endif
	return lua_workspace;
}

bool FrameMain::PrepareLuaWorkspaceForClose() {
#ifdef WITH_WXSTC
	return !lua_workspace || lua_workspace->PrepareToClose(true);
#else
	return true;
#endif
}

void FrameMain::FinishLuaWorkspaceClose(bool discard) {
#ifdef WITH_WXSTC
	if (lua_workspace)
		lua_workspace->FinishPendingDiscard(discard);
#else
	(void)discard;
#endif
}

bool FrameMain::NeedsLuaWorkspaceCloseDecision() const {
#ifdef WITH_WXSTC
	return lua_workspace && lua_workspace->IsDirty() && !lua_workspace->HasPendingDiscard();
#else
	return false;
#endif
}

void FrameMain::EnableToolBar(agi::OptionValue const& opt) {
	if (opt.GetBool()) {
		if (!GetToolBar()) {
			// AttachToolbar already attaches via SetToolBar() and then
			// populates, and Populate() ends in Realize(), so the toolbar
			// comes back fully realized. Realizing again here rebuilt the
			// native image list and re-inserted every button for nothing.
			toolbar::AttachToolbar(this, "main", context.get(), "Default");
		}
	}
	else if (wxToolBar *old_tb = GetToolBar()) {
		SetToolBar(nullptr);
		delete old_tb;
		Layout();
	}
}

void FrameMain::InitContents() {
	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase(
			"main",
			phase,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_started).count());
		phase_started = std::chrono::steady_clock::now();
	};

	StartupLog("Create background panel");
	{
#ifdef _WIN32
		ScopedWxNoComposited no_composited;
#endif
		contentsPanel = new wxPanel(this, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
	}

	StartupLog("Create subtitles grid");
	auto ui = context->GetUI();
	{
#ifdef _WIN32
		ScopedWxNoComposited no_composited;
#endif
		ui.subsGrid = new BaseGrid(contentsPanel, context.get());
	}

	StartupLog("Create subtitle editing box");
	auto EditBox = new SubsEditBox(contentsPanel, context.get());
	ui.subsEditBox = EditBox;
	observe_phase("startup.frame.contents.create_base_controls");

	StartupLog("Arrange main sizers");
	ToolsSizer = new wxBoxSizer(wxVERTICAL);
	ToolsSizer->Add(EditBox, 1, wxEXPAND);
	TopSizer = new wxBoxSizer(wxHORIZONTAL);
	TopSizer->Add(ToolsSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);
	{
#ifdef _WIN32
		ScopedWxNoComposited no_composited;
#endif
		editGridSplitter = new wxSplitterWindow(contentsPanel, wxID_ANY,
												wxDefaultPosition, wxDefaultSize, wxSP_NOBORDER);
		editAreaPanel = new wxPanel(editGridSplitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
	}
#ifndef _WIN32
	editGridSplitter->SetDoubleBuffered(true);
	editAreaPanel->SetDoubleBuffered(true);
#endif

	subtitleCommandToolbar = toolbar::GetOptionToolbarWrapping(
		editAreaPanel,
		"Subtitle/Edit Box/Command Buttons/Commands",
		context.get(),
		"Subtitle Edit Box");

	auto editAreaSizer = new wxBoxSizer(wxVERTICAL);
	editAreaSizer->Add(TopSizer, 1, wxEXPAND);
	editAreaSizer->Add(subtitleCommandToolbar, 0, wxEXPAND | wxLEFT | wxRIGHT, 2);
	editAreaSizer->Show(subtitleCommandToolbar,
		OPT_GET("Subtitle/Edit Box/Command Buttons/Enabled")->GetBool(), true);
	editAreaPanel->SetSizer(editAreaSizer);
	ui_activation.AddConnection(OPT_SUB("Subtitle/Edit Box/Command Buttons/Enabled",
		&FrameMain::OnSubtitleCommandToolbarVisibleChanged, this));

	ui.subsGrid->Reparent(editGridSplitter);
	EditBox->Reparent(editAreaPanel);

	editAreaPanel->Layout();
	int minHeight = editAreaPanel->GetBestSize().GetHeight();
	int savedHeight = OPT_GET("Subtitle/Edit Box/Display Height")->GetInt();
	int sashPos = savedHeight > minHeight ? savedHeight : minHeight;

	editGridSplitter->SetMinimumPaneSize(ui.subsGrid->FromDIP(40));
	editGridSplitter->SplitHorizontally(editAreaPanel, ui.subsGrid, sashPos);
	editGridSplitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGING,
		&FrameMain::OnEditGridSplitterSashPosChanging, this);
	editGridSplitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED,
		&FrameMain::OnEditGridSplitterSashPosChanged, this);
	auto queue_edit_grid_splitter_minimum_update = [this](agi::OptionValue const&) {
		QueueEditGridSplitterMinimumUpdate();
	};
	ui_activation.AddConnection(OPT_SUB("Subtitle/Edit Box/Command Buttons/Commands", queue_edit_grid_splitter_minimum_update));
	ui_activation.AddConnection(OPT_SUB("Audio/Display Height", queue_edit_grid_splitter_minimum_update));
	ui_activation.AddConnection(OPT_SUB("Subtitle/Show Original", queue_edit_grid_splitter_minimum_update));
	ui_activation.AddConnection(OPT_SUB("Video/Secondary Subtitles/Enabled", queue_edit_grid_splitter_minimum_update));
	ui_activation.AddConnection(OPT_SUB("Video/Secondary Subtitles/Height", queue_edit_grid_splitter_minimum_update));
	ui_activation.AddConnection(OPT_SUB("Subtitle/Edit Box/Display Height", queue_edit_grid_splitter_minimum_update));

	MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(editGridSplitter, 1, wxEXPAND);
	contentsPanel->SetSizer(MainSizer);
	UpdateEditGridSplitterMinimum();
	observe_phase("startup.frame.contents.create_sizers");

	StartupLog("Perform layout");
	Layout();
	observe_phase("startup.frame.contents.initial_layout");
	StartupLog("Leaving InitContents");
}

void FrameMain::EnsureVideoBoxCreated() {
	if (videoBox)
		return;

	bool didFreeze = contentsPanel && !contentsPanel->IsFrozen();
	if (didFreeze)
		contentsPanel->Freeze();

	videoBox = new VideoBox(editAreaPanel, false, context.get());
	videoBox->Hide();
	TopSizer->Insert(0, videoBox, 0, wxEXPAND, 0);
	TopSizer->Show(videoBox, false, true);
	videoBox->SyncToContextState();

	if (didFreeze)
		contentsPanel->Thaw();
}

void FrameMain::EnsureAudioBoxCreated() {
	if (audioBox)
		return;

	bool didFreeze = contentsPanel && !contentsPanel->IsFrozen();
	if (didFreeze)
		contentsPanel->Freeze();

	auto ui = context->GetUI();
	ui.audioBox = audioBox = new AudioBox(editAreaPanel, context.get());
	ToolsSizer->Insert(0, audioBox, 0, wxEXPAND);
	ToolsSizer->Show(audioBox, false, true);
	audioBox->SyncToContextState();

	if (didFreeze)
		contentsPanel->Thaw();
}

void FrameMain::SyncAudioOpenUi() {
	pending_audio_open_ui_sync = false;
	if (IsBeingDeleted())
		return;

	auto core = context->GetCore();
	if (!core.project->AudioProvider()) {
		SetDisplayMode(-1, 0);
		return;
	}

	EnsureAudioBoxCreated();
	SetDisplayMode(-1, 1);
}

void FrameMain::SyncVideoOpenUi() {
	pending_video_open_ui_sync = false;
	if (IsBeingDeleted())
		return;

	auto core = context->GetCore();
	auto provider = core.project->VideoProvider();
	if (!provider) {
		SetDisplayMode(0, -1);
		return;
	}

	Freeze();
	EnsureVideoBoxCreated();
	int vidx = provider->GetWidth(), vidy = provider->GetHeight();
	auto ui = context->GetUI();

	double zoom = ui.videoDisplay->GetZoom();
	wxSize windowSize = GetSize();
	if (vidx*3*zoom > windowSize.GetX()*4 || vidy*4*zoom > windowSize.GetY()*6)
		ui.videoDisplay->SetZoom(zoom * .25);
	else if (vidx*3*zoom > windowSize.GetX()*2 || vidy*4*zoom > windowSize.GetY()*3)
		ui.videoDisplay->SetZoom(zoom * .5);

	SetDisplayMode(1,-1);

	if (OPT_GET("Video/Detached/Enabled")->GetBool() && !ui.dialog->Get<DialogDetachedVideo>())
		cmd::call("video/detach", context.get());
	Thaw();
}

void FrameMain::SetDisplayMode(int video, int audio) {
	if (!IsShownOnScreen()) return;

	bool sv = false, sa = false;
	auto core = context->GetCore();
	auto ui = context->GetUI();

	if (video == -1) sv = showVideo;
	else if (video)  sv = core.project->VideoProvider() && !ui.dialog->Get<DialogDetachedVideo>();

	if (audio == -1) sa = showAudio;
	else if (audio)  sa = !!core.project->AudioProvider();

	// See if anything changed
	if (sv == showVideo && sa == showAudio) return;

	bool didFreeze = !IsFrozen();
	if (didFreeze) Freeze();

	if (sv)
		EnsureVideoBoxCreated();
	if (sa)
		EnsureAudioBoxCreated();

	showVideo = sv;
	showAudio = sa;

	core.videoController->Stop();

	if (videoBox)
		TopSizer->Show(videoBox, showVideo, true);
	if (audioBox)
		ToolsSizer->Show(audioBox, showAudio, true);

	UpdateEditGridSplitterMinimum();
	MainSizer->Layout();
	Layout();

	if (didFreeze) Thaw();
}

void FrameMain::UpdateTitle() {
	wxString newTitle;
	auto core = context->GetCore();
	if (core.subsController->IsModified()) newTitle << wxS("* ");
	newTitle << core.subsController->Filename().filename().wstring();

#ifndef __WXMAC__
	newTitle << wxS(" - Aegisub ") << wxString::FromUTF8(GetAegisubLongVersionString());
#endif

#if defined(__WXMAC__)
	// On Mac, set the mark in the close button
	OSXSetModified(core.subsController->IsModified());
#endif

	if (GetTitle() != newTitle) SetTitle(newTitle);
}

void FrameMain::OnVideoOpen(AsyncVideoProvider *provider) {
	if (!provider) {
		SetDisplayMode(0, -1);
		return;
	}

	if (!videoBox) {
		if (!pending_video_open_ui_sync) {
			pending_video_open_ui_sync = true;
			CallAfter([this] { SyncVideoOpenUi(); });
		}
		return;
	}

	SyncVideoOpenUi();
}

void FrameMain::OnVideoDetach(agi::OptionValue const& opt) {
	auto core = context->GetCore();
	if (opt.GetBool())
		SetDisplayMode(0, -1);
	else if (core.project->VideoProvider())
		SetDisplayMode(1, -1);
}

void FrameMain::OnSubtitleCommandToolbarVisibleChanged(agi::OptionValue const& opt) {
	if (!subtitleCommandToolbar || !editAreaPanel)
		return;

	editAreaPanel->GetSizer()->Show(subtitleCommandToolbar, opt.GetBool(), true);
	if (opt.GetBool()) {
		subtitleCommandToolbar->InvalidateBestSize();
		for (wxWindow *w = subtitleCommandToolbar->GetParent(); w; w = w->GetParent())
			w->InvalidateBestSize();
	}
	QueueEditGridSplitterMinimumUpdate();
	editAreaPanel->Layout();
	editAreaPanel->GetParent()->Layout();
	Layout();
}

void FrameMain::OnEditGridSplitterSashPosChanged(wxSplitterEvent& event) {
	if (updating_edit_grid_splitter_sash)
		return;

	int sashPosition = event.GetSashPosition();
	int minPosition = GetEditGridSplitterMinimumPosition();

	if (sashPosition < minPosition) {
		sashPosition = minPosition;
		editGridSplitter->SetSashPosition(sashPosition);
		return;
	}
}

void FrameMain::OnEditGridSplitterSashPosChanging(wxSplitterEvent& event) {
	int minPosition = GetEditGridSplitterMinimumPosition();
	if (event.GetSashPosition() < minPosition)
		event.SetSashPosition(minPosition);
}

void FrameMain::QueueEditGridSplitterMinimumUpdate() {
	if (pending_edit_grid_splitter_minimum_update)
		return;

	pending_edit_grid_splitter_minimum_update = true;
	CallAfter([this] {
		pending_edit_grid_splitter_minimum_update = false;
		UpdateEditGridSplitterForContentChange();
	});
}

int FrameMain::GetEditGridSplitterMinimumPosition() {
	if (edit_grid_splitter_minimum_position > 0)
		return edit_grid_splitter_minimum_position;
	return UpdateEditGridSplitterMinimumPosition();
}

int FrameMain::UpdateEditGridSplitterMinimumPosition() {
	if (!editAreaPanel)
		return 0;

	editAreaPanel->SetMinSize(wxDefaultSize);
	for (wxWindow *window = editAreaPanel; window; window = window->GetParent())
		window->InvalidateBestSize();

	editAreaPanel->Layout();
	int minPosition = editAreaPanel->GetBestSize().GetHeight();
	editAreaPanel->SetMinSize(wxSize(-1, minPosition));
	edit_grid_splitter_minimum_position = minPosition;
	return edit_grid_splitter_minimum_position;
}

void FrameMain::UpdateEditGridSplitterMinimum() {
	if (!editGridSplitter || !editAreaPanel)
		return;

	int minPosition = UpdateEditGridSplitterMinimumPosition();
	int preferredPosition = OPT_GET("Subtitle/Edit Box/Display Height")->GetInt();
	int targetPosition = preferredPosition > minPosition ? preferredPosition : minPosition;
	int sashPosition = editGridSplitter->GetSashPosition();

	if (editGridSplitter->IsSplit() && sashPosition != targetPosition) {
		updating_edit_grid_splitter_sash = true;
		editGridSplitter->SetSashPosition(targetPosition);
		updating_edit_grid_splitter_sash = false;
	}
}

void FrameMain::UpdateEditGridSplitterForContentChange() {
	UpdateEditGridSplitterMinimum();
	if (MainSizer)
		MainSizer->Layout();
	Layout();
}

void FrameMain::StatusTimeout(wxString text,int ms) {
	SetStatusText(text,1);
	StatusClear.SetOwner(this, ID_APP_TIMER_STATUSCLEAR);
	StatusClear.Start(ms,true);
}

void FrameMain::SetLastCommand(wxString text) {
	SetStatusText(text, 2);
}

void FrameMain::UpdateSelectionAnchorStatus() {
	auto anchor = context->GetCore().selectionController->RefreshSelectionAnchor();
	if (!anchor) {
		SetStatusText(wxString(), 0);
		return;
	}

	auto text = anchor->available
		? wxString::Format(_("Anchor: line %d"), anchor->row + 1)
		: wxString::Format(_("Anchor: line %d (unavailable)"), anchor->row + 1);
	SetStatusText(text, 0);
}

BEGIN_EVENT_TABLE(FrameMain, wxFrame)
	EVT_TIMER(ID_APP_TIMER_STATUSCLEAR, FrameMain::OnStatusClear)
#ifdef _WIN32
	EVT_TIMER(ID_APP_TIMER_FONTCHANGE_DEBOUNCE, FrameMain::OnFontChangeDebounce)
	EVT_TIMER(ID_APP_TIMER_AUDIO_OUTPUT_RECOVERY, FrameMain::OnAudioOutputRecoveryDebounce)
#endif
	EVT_CLOSE(FrameMain::OnCloseWindow)
	EVT_CHAR_HOOK(FrameMain::OnKeyDown)
	EVT_MOUSEWHEEL(FrameMain::OnMouseWheel)
END_EVENT_TABLE()

void FrameMain::OnCloseWindow(wxCloseEvent &event) {
	auto workspace_close = agi::make_scope_exit([this] { FinishLuaWorkspaceClose(false); });
	wxEventBlocker blocker(this, wxEVT_CLOSE_WINDOW);
	auto core = context->GetCore();
	auto ui = context->GetUI();

	core.videoController->Stop();
	core.audioController->Stop();

	// Ask user if he wants to save first
	if (core.subsController->TryToClose(event.CanVeto()) == wxCANCEL) {
		event.Veto();
		return;
	}

	ui.dialog.reset();

	// Store maximization state
	OPT_SET("App/Maximized")->SetBool(IsMaximized());

#ifdef _WIN32
	FontChangeDebounce.Stop();
	AudioOutputRecoveryDebounce.Stop();
	UnregisterSessionNotifications();
#endif

	Destroy();
}

void FrameMain::OnStatusClear(wxTimerEvent &) {
	SetStatusText(wxString(),1);
}

void FrameMain::OnSelectedSetChanged() {
	auto const& sel = context->GetCore().selectionController->GetSelectedSet();
	int count = sel.size();
	SetStatusText(count <= 1 ? wxString() : wxString::Format(_("%d lines selected"), count), 3);
}

#ifdef _WIN32
void FrameMain::OnFontChangeDebounce(wxTimerEvent &) {
	// Drop cached family catalog so style editor / \\fn preference remapping
	// pick up newly installed or removed fonts on next open/use.
	font_family_catalog_cache::Invalidate();
	if (auto *grid = context->GetUI().subsGrid)
		grid->NotifySystemFontsChanged();
	// Kick a background rebuild so the next interactive use is warm.
	font_family_catalog_cache::WarmAsync();
	context->GetCore().project->ReloadSubtitlesProvider();
}

void FrameMain::OnAudioOutputRecoveryDebounce(wxTimerEvent &) {
	LOG_I("audio/player/xaudio2/recovery") << "Running queued XAudio2 recovery after " << pending_audio_output_recovery_reason;
	context->GetCore().audioController->RecoverAudioPlayerAfterDeviceChange();
	pending_audio_output_recovery_reason.clear();
}

void FrameMain::QueueAudioOutputRecovery(std::string reason) {
	if (OPT_GET("Audio/Player")->GetString() != "XAudio2")
		return;

	if (AudioOutputRecoveryDebounce.IsRunning()) {
		LOG_D("audio/player/xaudio2/recovery") << "Coalescing XAudio2 recovery request; latest trigger: " << reason;
	}
	else {
		LOG_I("audio/player/xaudio2/recovery") << "Queueing XAudio2 recovery after " << reason;
	}
	pending_audio_output_recovery_reason = std::move(reason);
	AudioOutputRecoveryDebounce.SetOwner(this, ID_APP_TIMER_AUDIO_OUTPUT_RECOVERY);
	AudioOutputRecoveryDebounce.Start(kAudioOutputRecoveryDebounceDelayMs, true);
}

void FrameMain::RegisterSessionNotifications() {
	if (session_notifications_registered)
		return;

	auto *hwnd = reinterpret_cast<HWND>(GetHandle());
	if (!hwnd)
		return;

	session_notifications_registered = !!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
	if (!session_notifications_registered)
		LOG_W("audio/player/xaudio2/recovery") << "Failed to register session-change notifications for XAudio2 recovery.";
}

void FrameMain::UnregisterSessionNotifications() {
	if (!session_notifications_registered)
		return;

	auto *hwnd = reinterpret_cast<HWND>(GetHandle());
	if (hwnd)
		WTSUnRegisterSessionNotification(hwnd);
	session_notifications_registered = false;
}

WXLRESULT FrameMain::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) {
	if (message == WM_FONTCHANGE) {
		FontChangeDebounce.SetOwner(this, ID_APP_TIMER_FONTCHANGE_DEBOUNCE);
		FontChangeDebounce.Start(kFontChangeDebounceDelayMs, true);
	}

	if (message == WM_DEVICECHANGE && IsRelevantAudioDeviceChange(wParam))
		QueueAudioOutputRecovery(DescribeAudioDeviceChange(wParam));

	if (message == WM_WTSSESSION_CHANGE) {
		if (IsRelevantSessionChange(wParam))
			QueueAudioOutputRecovery(DescribeSessionChange(wParam));
		if (IsRelevantGridSessionChange(wParam)) {
			if (auto *grid = context->GetUI().subsGrid)
				grid->NotifyTextRasterPolicyChanged();
		}
	}

	if ((message == WM_DISPLAYCHANGE
		|| (message == WM_SETTINGCHANGE && IsRelevantTextRasterSettingChange(wParam)))) {
		if (auto *grid = context->GetUI().subsGrid)
			grid->NotifyTextRasterPolicyChanged();
	}

	if (message == WM_SIZE) {
		perf_trace::VideoUiDurationScope trace("frame_main.resize", LOWORD(lParam), HIWORD(lParam));
		WXLRESULT res = wxFrame::MSWWindowProc(message, wParam, lParam);
		// Repaint exposed frame background without invalidating unchanged child
		// controls: wxWindow::Refresh also requests RDW_ALLCHILDREN on Windows.
		::RedrawWindow(reinterpret_cast<HWND>(GetHandle()), nullptr, nullptr,
					   RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN);
		return res;
	}

	return wxFrame::MSWWindowProc(message, wParam, lParam);
}
#endif

void FrameMain::OnAudioOpen(agi::AudioProvider *provider) {
	if (!provider) {
		SetDisplayMode(-1, 0);
		return;
	}

	if (!audioBox) {
		if (!pending_audio_open_ui_sync) {
			pending_audio_open_ui_sync = true;
			CallAfter([this] { SyncAudioOpenUi(); });
		}
		return;
	}

	SetDisplayMode(-1, 1);
}

void FrameMain::OnSubtitlesOpen() {
	UpdateTitle();
	SetDisplayMode(1, 1);
}

void FrameMain::OnKeyDown(wxKeyEvent &event) {
	hotkey::check("Main Frame", context.get(), event);
}

void FrameMain::OnMouseWheel(wxMouseEvent &evt) {
	ForwardMouseWheelEvent(this, evt);
}
