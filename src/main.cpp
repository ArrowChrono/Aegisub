// Copyright (c) 2005, Rodrigo Braz Monteiro
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

/// @file main.cpp
/// @brief Main entry point, as well as crash handling
/// @ingroup main
///

#include "main.h"

#include "command/command.h"
#include "include/aegisub/hotkey.h"

#include "auto4_base.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "app_runtime.h"
#include "automation_command_executor.h"
#include "automation_scenario.h"
#include "automation_scenario_runner.h"
#include "automation_session_service.h"
#include "avisynth_provider_registration.h"
#include "compat.h"
#include "crash_writer.h"
#include "discord_presence.h"
#include "format.h"
#include "frame_main.h"
#include "gui_wx_runtime_entry_host.h"
#include "gui_test_wx_boundary.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "subs_controller.h"
#include "selection_controller.h"
#include "subtitle_grid_folding.h"
#include "video_controller.h"
#include "utils.h"
#include "video_display.h"
#include "visual_guide_overlay.h"
#include <libaegisub/dispatch.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/format_path.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
#include <wx/arrstr.h>
#include <wx/bitmap.h>
#include <wx/clipbrd.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/stackwalk.h>
#include <wx/thread.h>
#include <wx/utils.h>

wxIMPLEMENT_APP_NO_MAIN(AegisubApp);

static const char *LastStartupState = nullptr;

#ifdef WITH_STARTUPLOG
#define StartupLog(a) wxMessageBox(wxT(a), wxT("Aegisub startup log"))
#else
#define StartupLog(a) LastStartupState = a
#endif

void AegisubApp::OnAssertFailure(const wxChar *file, int line, const wxChar *func, const wxChar *cond, const wxChar *msg) {
	auto narrow = [](const wxChar *value) {
		return value ? from_wx(wxString(value)) : std::string();
	};
	LOG_A("wx/assert") << narrow(file) << ":" << line << ":" << narrow(func) << "() " << narrow(cond) << ": " << narrow(msg);
#ifdef __WXMSW__
	if (RecordWxBoundaryTestAssertion(file, line, func, cond, msg))
		return;
#endif
	wxApp::OnAssertFailure(file, line, func, cond, msg);
}

AegisubApp::AegisubApp() {
	auto const started = std::chrono::steady_clock::now();

	// http://trac.wxwidgets.org/ticket/14302
	wxSetEnv(wxS("UBUNTU_MENUPROXY"), wxS("0"));

	perf_trace::ObserveWindowOpenPhase(
		"main",
		"startup.wx_app.constructor",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

AegisubApp::~AegisubApp() = default;

namespace {

agi::fs::path PathFromUtf8(std::string const& value) {
	return agi::fs::PathFromString(value);
}

std::vector<std::string> ToUtf8Args(wxArrayString const& args) {
	std::vector<std::string> values;
	values.reserve(args.size());
	for (auto const& arg : args)
		values.emplace_back(arg.ToStdString(wxConvUTF8));
	return values;
}

aegisub::automation_session_service::AutomationSessionResult RunGuiAutomationStep(
	aegisub::automation_session_service::AutomationSessionRequest request) {
	// RunAsync currently invokes its completion callback before returning. Keep
	// this adapter synchronous so the scenario runner can preserve step order
	// and the GUI runtime remains the sole owner of automation-session cleanup.
	std::optional<aegisub::automation_session_service::AutomationSessionResult> result;
	aegisub::automation_session_service::RunAsync(
		std::move(request),
		[&result](aegisub::automation_session_service::AutomationSessionResult finished) {
			result.emplace(std::move(finished));
		});
	if (!result)
		throw std::runtime_error("GUI automation session did not complete");
	return std::move(*result);
}

}

/// Message displayed when an exception has occurred.
static wxString exception_message = wxS("Oops, Aegisub has crashed!\n\nAn attempt has been made to save a copy of your file to:\n\n%s\n\nAegisub will now close.");

/// @brief Gets called when application starts.
/// @return bool
bool AegisubApp::OnInit() {
	auto const startup_started = std::chrono::steady_clock::now();
	auto duration_ms = [](std::chrono::steady_clock::time_point started) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	};
	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase("main", phase, duration_ms(phase_started));
		phase_started = std::chrono::steady_clock::now();
	};
	auto finish_startup_trace = [&](bool succeeded) {
		perf_trace::TraceWindowOpenEnd("main", duration_ms(startup_started), succeeded);
	};
	perf_trace::TraceWindowOpenBegin("main");

	// App name (yeah, this is a little weird to get rid of an odd warning)
#if defined(__WXMSW__) || defined(__WXMAC__)
	SetAppName(wxS("Aegisub"));
#else
	SetAppName(wxS("aegisub"));
#endif
	observe_phase("startup.on_init.set_app_name");
	auto const process_args = ToUtf8Args(argv.GetArguments());
	launch_plan.emplace(ParseAppLaunchPlan(process_args));
	if (launch_plan->mode == AppLaunchMode::GuiTest) {
		if (!launch_plan->ParseSucceeded() || !launch_plan->gui_test_run) {
			finish_startup_trace(false);
			ShowGuiWxBootstrapUiError("Invalid GUI test command line", launch_plan->error);
			return false;
		}
		std::string profile_error;
		auto const& request = *launch_plan->gui_test_run;
		automation_profile = std::make_unique<AutomationRuntimeProfile>(
			AutomationRuntimeProfile::Create(
				AutomationRuntimeProfileOptions{
					request.profile_directory,
					request.artifacts_directory,
					request.keep_profile},
				profile_error));
		if (automation_profile->Root().empty()) {
			finish_startup_trace(false);
			ShowGuiWxBootstrapUiError("Could not create GUI test profile", profile_error);
			return false;
		}
		if (!launch_plan->gui_test_host) {
			auto scenario_result = aegisub::automation_scenario::Load(
				request.scenario_path, request.inputs);
			gui_test_scenario = std::move(scenario_result.scenario);
			gui_test_scenario_error = std::move(scenario_result.error);
		}
	}
	auto record_gui_test_phase = [&](char const* phase) {
		if (!automation_profile)
			return;
		try {
			std::ofstream out(automation_profile->ArtifactsDirectory() / agi::fs::PathFromString("startup.log"), std::ios::app);
			out << phase << "\n";
		}
		catch (...) {
		}
	};
	record_gui_test_phase("launch-plan.ready");

	BindGuiWxMainQueueDispatchHandler([this] { OnExceptionInMainLoop(); });
	observe_phase("startup.on_init.bind_main_queue_handler");

	runtime = std::make_unique<AppRuntime>();
	observe_phase("startup.on_init.create_runtime");
	std::string runtime_error;
	auto runtime_options = BuildGuiWxAppRuntimeInitOptions();
	if (automation_profile) {
		runtime_options.path_overrides = automation_profile->PathOverrides();
		runtime_options.locale_policy = RuntimeLocalePolicy::UseConfiguredOrEnglish;
	}
	record_gui_test_phase("runtime-options.ready");
	auto bootstrap_ui_host = runtime_options.bootstrap_ui_host;
	runtime_options.bootstrap_ui_host = bootstrap_ui_host;
	observe_phase("startup.on_init.build_runtime_options");
	auto const runtime_initialize_started = std::chrono::steady_clock::now();
	if (!runtime->Initialize(std::move(runtime_options), runtime_error)) {
		perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.total", duration_ms(runtime_initialize_started));
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", runtime_error);
		return false;
	}
	RegisterAvisynthProviderFactories();
	if (automation_profile) {
		OPT_SET("App/First Start")->SetBool(false);
		OPT_SET("App/Auto/Check For Updates")->SetBool(false);
		OPT_SET("App/Auto/Save")->SetBool(false);
	}
	record_gui_test_phase("runtime.initialized");
	perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.total", duration_ms(runtime_initialize_started));
	phase_started = std::chrono::steady_clock::now();

	StartupLog("Inside OnInit");
	try {
		// Crash handling
#if (!defined(_DEBUG) || defined(WITH_EXCEPTIONS)) && (wxUSE_ON_FATAL_EXCEPTION+0)
		StartupLog("Install exception handler");
		wxHandleFatalExceptions(true);
		observe_phase("startup.on_init.install_exception_handler");
#endif

#ifdef __APPLE__
		// When run from an app bundle, LC_CTYPE defaults to "C", which breaks on
		// anything involving unicode and in some cases number formatting.
		// The right thing to do here would be to query CoreFoundation for the user's
		// locale and add .UTF-8 to that, but :effort:
		setlocale(LC_CTYPE, "en_US.UTF-8");
		observe_phase("startup.on_init.platform_locale_adjustment");
#endif

		exception_message = _("Oops, Aegisub has crashed!\n\nAn attempt has been made to save a copy of your file to:\n\n%s\n\nAegisub will now close.");
		observe_phase("startup.on_init.exception_message_setup");

		StartupLog("Create main window");
#ifdef WITH_DISCORD_PRESENCE
		if (launch_plan->mode == AppLaunchMode::Gui)
			discord_presence = std::make_unique<DiscordPresence>(startup_started);
#endif
		StartupLog("Possibly perform automatic updates check");
		StartupLog("Parse command line");
		auto const startup_sequence_started = std::chrono::steady_clock::now();
		auto startup_args = process_args;
		if (launch_plan->mode == AppLaunchMode::GuiTest) {
			startup_args.resize(1);
			startup_args.insert(
				startup_args.end(),
				launch_plan->gui_test_open_files.begin(),
				launch_plan->gui_test_open_files.end());
		}
		RunGuiWxAppStartupSequence(startup_args,
			[this] { NewProjectContext(); },
			[this](std::vector<std::string> const& files) {
				std::vector<agi::fs::path> paths;
				paths.reserve(files.size());
				for (auto const& file : files)
					paths.push_back(PathFromUtf8(file));
				if (!paths.empty())
					frames[0]->context->GetCore().project->LoadList(paths);
			},
			launch_plan->mode != AppLaunchMode::GuiTest
				|| (gui_test_scenario && gui_test_scenario->load_global_scripts));
		record_gui_test_phase("frame.created");
		perf_trace::ObserveWindowOpenPhase("main", "startup.sequence.total", duration_ms(startup_sequence_started));
		if (launch_plan->mode == AppLaunchMode::GuiTest)
			CallAfter([this] { StartGuiTest(); });
	}
	catch (agi::Exception const& e) {
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", e.GetMessage());
		return false;
	}
	catch (std::exception const& e) {
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", e.what());
		return false;
	}
#ifndef _DEBUG
	catch (...) {
		finish_startup_trace(false);
		ShowGuiWxBootstrapUiError("Fatal error while initializing", "Unhandled exception");
		return false;
	}
#endif

	StartupLog("Clean old autosave files");
	auto const autosave_cleanup_started = std::chrono::steady_clock::now();
	CleanCache(config::path->Decode(OPT_GET("Path/Auto/Save")->GetString()), "*.AUTOSAVE.ass", 100, 1000);
	perf_trace::ObserveWindowOpenPhase("main", "startup.post.autosave_cleanup.schedule", duration_ms(autosave_cleanup_started));

	StartupLog("Initialization complete");
	finish_startup_trace(true);
	return true;
}

void AegisubApp::StartGuiTest() {
	if (!automation_profile || !launch_plan || frames.empty())
		return;

	auto const artifacts = automation_profile->ArtifactsDirectory();
	auto save_json = [artifacts](std::string const& name, const json::Object& object) {
		agi::fs::path temporary_path;
		try {
			auto final_path = artifacts / agi::fs::PathFromString(name);
			temporary_path = agi::fs::UniquePath(
				artifacts / agi::fs::PathFromString(name + ".tmp-%%%%%%%%"));
			{
				auto stream = agi::io::Save(temporary_path);
				agi::JsonWriter::Write(object, stream.Get());
				stream.Get().flush();
			}
			std::error_code error;
			std::filesystem::remove(final_path, error);
			std::filesystem::rename(temporary_path, final_path, error);
			if (error)
				throw std::system_error(error, "could not publish automation artifact");
			return true;
		}
		catch (std::exception const& e) {
			if (!temporary_path.empty()) {
				std::error_code ignored;
				std::filesystem::remove(temporary_path, ignored);
			}
			LOG_E("automation/gui_test") << "Could not save " << name << ": " << e.what();
			return false;
		}
	};

	json::Object ready;
	ready["version"] = static_cast<int64_t>(1);
	ready["host"] = "gui-test";
	ready["state"] = "ready";
	ready["process_id"] = static_cast<int64_t>(wxGetProcessId());
	ready["window_title"] = from_wx(frames.front()->GetTitle());
	ready["artifacts"] = agi::fs::PathToGenericString(artifacts);
	if (!save_json("ready.json", ready)) {
		gui_test_exit_code = 2;
		ScheduleGuiTestClose();
		return;
	}

	if (launch_plan->gui_test_host) {
		auto *frame = frames.front();
		auto document_state = [frame] {
			auto const core = frame->context->GetCore();
			auto const& selected = core.selectionController->GetSelectedSet();
			auto const *active = core.selectionController->GetActiveLine();
			auto& folding = core.ass->Folding();
			json::Object state;
			state["document_revision"] = core.subsController->GetDocumentRevision();
			state["document_generation"] = static_cast<int64_t>(core.subsController->GetDocumentGeneration());
			state["modified"] = core.subsController->IsModified();
			state["undo_empty"] = core.subsController->IsUndoStackEmpty();
			state["redo_empty"] = core.subsController->IsRedoStackEmpty();
			state["undo_description"] = core.subsController->GetUndoDescription();
			state["redo_description"] = core.subsController->GetRedoDescription();
			state["active_id"] = static_cast<int64_t>(active ? active->Id : -1);
			state["video_frame"] = static_cast<int64_t>(core.videoController->GetFrameN());
			json::Array selected_ids, rows, groups;
			for (auto const *line : selected)
				selected_ids.emplace_back(static_cast<int64_t>(line->Id));
			int source_row = 0;
			for (auto const& line : core.ass->Events) {
				json::Object row;
				row["source_id"] = static_cast<int64_t>(line.Id);
				row["source_row"] = static_cast<int64_t>(source_row);
				row["display_row"] = static_cast<int64_t>(folding.SourceToDisplay(source_row));
				row["selected"] = std::ranges::any_of(selected, [&line](auto const *item) { return item == &line; });
				row["text"] = line.Text.get();
				row["event"] = line.GetEntryData();
				rows.emplace_back(std::move(row));
				++source_row;
			}
			for (auto const& group : folding.Groups()) {
				json::Object row;
				row["id"] = static_cast<int64_t>(group.id);
				row["start"] = static_cast<int64_t>(group.start);
				row["end"] = static_cast<int64_t>(group.end);
				row["collapsed"] = folding.IsCollapsed(group);
				groups.emplace_back(std::move(row));
			}
			state["selected_ids"] = std::move(selected_ids);
			state["rows"] = std::move(rows);
			state["fold_groups"] = std::move(groups);
			state["display_rows"] = static_cast<int64_t>(folding.DisplayRows().size());
			if (auto *display = frame->context->GetUI().videoDisplay) {
				state["native_capture"] = display->HasCapture();
				state["visual_interacting"] = display->IsVisualToolInteracting();
				auto const viewport = display->GetVisualGuideViewport();
				json::Object allocation;
				allocation["x"] = viewport.canvas_x;
				allocation["y"] = viewport.canvas_y;
				allocation["width"] = viewport.canvas_width;
				allocation["height"] = viewport.canvas_height;
				allocation["script_width"] = viewport.script_width;
				allocation["script_height"] = viewport.script_height;
				state["viewport"] = std::move(allocation);
				state["presentation_snapshot_available"] = static_cast<bool>(display->GetToolPresentationSnapshot());
			}
			return state;
		};
#ifdef __WXMSW__
		InstallWxBoundaryTestHost(*frame, *frame->context, artifacts, [this, save_json](std::string const& name, const json::Object& object) {
			if (!save_json(name, object)) {
				gui_test_exit_code = 2;
				ScheduleGuiTestClose();
			}
		});
#else
		frame->Bind(wxEVT_MENU, [document_state, save_json, sequence = 0](wxCommandEvent&) mutable {
			auto state = document_state();
			state["sequence"] = static_cast<int64_t>(++sequence);
			save_json("visual-state-" + std::to_string(sequence) + ".json", state); }, 0x7FFD);
#endif
		frame->Bind(wxEVT_MENU, [frame, artifacts, document_state, save_json, sequence = 0](wxCommandEvent&) mutable {
			auto const name = "viewport-" + std::to_string(++sequence);
			json::Object result;
			result["sequence"] = static_cast<int64_t>(sequence);
			result["capture_scope"] = "normal viewport before swap";
			result["captured"] = false;
			result["document"] = document_state();
			auto *display = frame->context->GetUI().videoDisplay;
			if (!display) {
				result["error"] = "main window has no video canvas";
				save_json(name + ".json", result);
				return;
			}
			auto const origin = frame->ScreenToClient(display->ClientToScreen(wxPoint(0, 0)));
			auto const size = display->GetClientSize();
			auto const dip_size = display->ToDIP(size);
			result["canvas_client_units"] = "wx client coordinates; physical pixels on MSW";
			result["canvas_client_x"] = static_cast<int64_t>(origin.x);
			result["canvas_client_y"] = static_cast<int64_t>(origin.y);
			result["canvas_client_width"] = static_cast<int64_t>(size.x);
			result["canvas_client_height"] = static_cast<int64_t>(size.y);
			result["canvas_dip_width"] = static_cast<int64_t>(dip_size.x);
			result["canvas_dip_height"] = static_cast<int64_t>(dip_size.y);
			result["dpi_scale"] = display->GetDPIScaleFactor();
			auto capture = display->CaptureRenderedViewport();
			result["frame"] = static_cast<int64_t>(capture.frame);
			result["backing_scale"] = static_cast<int64_t>(capture.backing_scale);
			result["current"] = capture.current;
			result["swapped"] = capture.swapped;
			result["physical_width"] = static_cast<int64_t>(capture.image.IsOk() ? capture.image.GetWidth() : 0);
			result["physical_height"] = static_cast<int64_t>(capture.image.IsOk() ? capture.image.GetHeight() : 0);
			if (capture.error.empty() && !capture.current)
				capture.error = "captured frame is not the current provider delivery";
			if (capture.error.empty()) {
				auto const image_name = name + ".png";
				if (capture.image.SaveFile(to_wx(agi::fs::PathToString(artifacts / agi::fs::PathFromString(image_name))), wxBITMAP_TYPE_PNG)) {
					result["captured"] = true;
					result["image"] = image_name;
				}
				else
					capture.error = "could not save captured viewport PNG";
			}
			if (!capture.error.empty())
				result["error"] = capture.error;
			save_json(name + ".json", result); }, 0x7FFE);
		return;
	}

	if (!gui_test_scenario) {
		json::Object failure;
		failure["version"] = static_cast<int64_t>(1);
		failure["host"] = "gui-test";
		failure["passed"] = false;
		failure["exit_code"] = static_cast<int64_t>(64);
		failure["error"] = gui_test_scenario_error.empty()
			? "GUI test scenario was not loaded"
			: gui_test_scenario_error;
		failure["error_kind"] = "schema";
		failure["phase"] = "scenario";
		save_json("result.json", failure);
		gui_test_exit_code = 64;
		ScheduleGuiTestClose();
		return;
	}

	auto execution = aegisub::automation_scenario_runner::Run(
		*gui_test_scenario,
		"gui-test",
		artifacts,
		[](aegisub::automation_session_service::AutomationSessionRequest request) {
			return RunGuiAutomationStep(std::move(request));
		},
		[context = frames.front()->context.get()](std::string const& command_id) {
			return aegisub::automation_command_executor::Invoke(command_id, context, true);
		},
		{},
		[context = frames.front()->context.get()](
			std::string const& target,
			json::Object const& step) {
			if (target != "command")
				throw std::runtime_error("unsupported GUI query target: " + target);
			auto const command_id = static_cast<json::String const&>(step.at("id"));
			return aegisub::automation_command_executor::Inspect(command_id, context, true);
		},
		[](std::chrono::milliseconds duration) {
			auto const deadline = std::chrono::steady_clock::now() + duration;
			while (std::chrono::steady_clock::now() < deadline) {
				wxYieldIfNeeded();
				auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - std::chrono::steady_clock::now());
				if (remaining.count() > 0)
					wxMilliSleep(static_cast<unsigned long>(std::min<int64_t>(remaining.count(), 10)));
			}
		},
		[frame = frames.front()](std::string const&, agi::fs::path const& output) {
			json::Object result;
			auto const size = frame->GetClientSize();
			if (size.x <= 0 || size.y <= 0) {
				result["captured"] = false;
				result["error"] = "main window has no drawable client area";
				return result;
			}

			frame->Update();
			wxYieldIfNeeded();
			wxBitmap bitmap(size.x, size.y, 24);
			wxMemoryDC destination(bitmap);
			wxClientDC source(frame);
			auto const copied = destination.Blit(
				0, 0, size.x, size.y, &source, 0, 0, wxCOPY, false);
			destination.SelectObject(wxNullBitmap);
			bool has_visible_pixels = false;
			if (copied) {
				auto image = bitmap.ConvertToImage();
				auto const x_step = std::max(1, size.x / 32);
				auto const y_step = std::max(1, size.y / 32);
				for (int y = 0; y < size.y && !has_visible_pixels; y += y_step) {
					for (int x = 0; x < size.x; x += x_step) {
						if (image.GetRed(x, y) || image.GetGreen(x, y) || image.GetBlue(x, y)) {
							has_visible_pixels = true;
							break;
						}
					}
				}
			}
			auto const saved = copied && has_visible_pixels && bitmap.SaveFile(
				to_wx(agi::fs::PathToString(output)), wxBITMAP_TYPE_PNG);
			result["captured"] = saved;
			result["has_visible_pixels"] = has_visible_pixels;
			result["scope"] = "client";
			result["path"] = agi::fs::PathToGenericString(output);
			if (!saved)
				result["error"] = !copied
					? "could not copy the main window client area"
					: !has_visible_pixels
						? "GUI capture contained only black pixels"
						: "could not save GUI capture";
			return result;
		});
	auto const passed = execution.passed;
	auto const exit_code = execution.exit_code;
	auto result = aegisub::automation_scenario_runner::SerializeResult(
		gui_test_scenario->name,
		"gui-test",
		std::move(execution),
		automation_profile->Root(),
		artifacts);
	if (!save_json("result.json", result)) {
		gui_test_exit_code = 2;
		ScheduleGuiTestClose();
		return;
	}

	gui_test_exit_code = passed ? 0 : (exit_code == 0 ? 1 : exit_code);
	ScheduleGuiTestClose();
}

void AegisubApp::ScheduleGuiTestClose() {
	if (gui_test_close_scheduled)
		return;
	gui_test_close_scheduled = true;
	CallAfter([this] {
		CallAfter([this] {
			gui_test_close_scheduled = false;
			CloseAll();
		});
	});
}

int AegisubApp::OnExit() {
	auto record_exit_phase = [&](char const* phase) {
		if (!automation_profile)
			return;
		try {
			std::ofstream out(automation_profile->ArtifactsDirectory() / agi::fs::PathFromString("startup.log"), std::ios::app);
			out << phase << "\n";
		}
		catch (...) {
		}
	};
	record_exit_phase("exit.begin");
#ifdef WITH_DISCORD_PRESENCE
	discord_presence.reset();
#endif
	gui_test_close_scheduled = false;
	record_exit_phase("exit.close-barrier-reset");
	ui_activation.Deactivate();
	record_exit_phase("exit.ui-deactivated");

	for (auto frame : frames)
		delete frame;
	frames.clear();
	record_exit_phase("exit.frames-cleared");

	if (wxTheClipboard->Open()) {
		wxTheClipboard->Flush();
		wxTheClipboard->Close();
	}
	record_exit_phase("exit.clipboard-flushed");

	runtime.reset();
	record_exit_phase("exit.runtime-reset");
	if (automation_profile) {
		automation_profile->Complete(gui_test_exit_code.value_or(0) == 0);
		record_exit_phase("exit.profile-complete");
	}

	auto result = wxApp::OnExit();
	record_exit_phase("exit.wx-complete");
	automation_profile.reset();
	return result;
}

agi::Context& AegisubApp::NewProjectContext() {
	auto frame = new FrameMain;
#ifdef WITH_DISCORD_PRESENCE
	auto* subtitles = frame->context->GetCore().subsController.get();
	if (discord_presence) {
		discord_presence->Track(subtitles);
		frame->Bind(wxEVT_ACTIVATE, [this, subtitles](wxActivateEvent& event) {
			if (event.GetActive() && discord_presence)
				discord_presence->Activate(subtitles);
			event.Skip();
		});
	}
#endif
	frame->Bind(wxEVT_DESTROY, [=](wxWindowDestroyEvent& evt) {
		if (evt.GetWindow() != frame) {
			evt.Skip();
			return;
		}

#ifdef WITH_DISCORD_PRESENCE
		if (discord_presence)
			discord_presence->Remove(subtitles);
#endif
		frames.erase(remove(begin(frames), end(frames), frame), end(frames));
		if (frames.empty()) {
			if (launch_plan && launch_plan->mode == AppLaunchMode::GuiTest && !gui_test_exit_code)
				gui_test_exit_code = 0;
			ExitMainLoop();
		}
	});
	frames.push_back(frame);
	return *frame->context;
}

void AegisubApp::CloseAll() {
	for (auto frame : frames) {
		if (!frame->Close())
			break;
	}
}

void AegisubApp::UnhandledException(bool stackWalk) {
#if (!defined(_DEBUG) || defined(WITH_EXCEPTIONS)) && (wxUSE_ON_FATAL_EXCEPTION+0)
	bool any = false;
	agi::fs::path path;
	agi::fs::path last_recovered_path;
	for (auto& frame : frames) {
		auto c = frame->context.get();
		if (!c) continue;

		auto core = c->GetCore();
		if (!core.ass || !core.subsController) continue;

		try {
			path = config::path->Decode("?user/recovered");
			agi::fs::CreateDirectory(path);

			auto filename = core.subsController->Filename().stem();
			filename.replace_extension(agi::format("%s.ass", agi::util::strftime("%Y-%m-%d-%H-%M-%S")));
			path /= filename;
			core.subsController->Save(path);
			any = true;
			last_recovered_path = path;
		}
		catch (agi::Exception const& err) {
			crash_writer::Write("Crash recovery save failed: " + err.GetMessage());
		}
		catch (std::exception const& err) {
			crash_writer::Write(std::string("Crash recovery save failed: ") + err.what());
		}
		catch (...) {
			crash_writer::Write("Crash recovery save failed: unknown error");
		}
	}

	if (stackWalk)
		crash_writer::Write();

	if (any) {
		// Inform user of crash.
		ShowGuiWxBootstrapUiError(from_wx(_("Program error")), agi::format(exception_message, last_recovered_path));
	}
	else if (LastStartupState) {
		ShowGuiWxBootstrapUiError(
			from_wx(_("Program error")),
			agi::format("Aegisub has crashed while starting up!\n\nThe last startup step attempted was: %s.", LastStartupState));
	}
#endif
}

void AegisubApp::OnUnhandledException() {
	UnhandledException(false);
}

void AegisubApp::OnFatalException() {
	UnhandledException(true);
}

bool AegisubApp::OnExceptionInMainLoop() {
	try {
		throw;
	}
	catch (const agi::Exception &e) {
		ShowGuiWxBootstrapUiError(
			"Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), e.GetMessage()));
	}
	catch (const std::exception &e) {
		ShowGuiWxBootstrapUiError(
			"Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), e.what()));
	}
	catch (...) {
		ShowGuiWxBootstrapUiError(
			"Exception in event handler",
			agi::format(_("An unexpected error has occurred. Please save your work and restart Aegisub.\n\nError Message: %s"), "Unknown error"));
	}
	return true;
}

int AegisubApp::OnRun() {
	std::string error;

	try {
		auto const result = MainLoop();
		return gui_test_exit_code.value_or(result);
	}
	catch (const std::exception &e) { error = std::string("std::exception: ") + e.what(); }
	catch (const agi::Exception &e) { error = "agi::exception: " + e.GetMessage(); }
	catch (...) { error = "Program terminated in error."; }

	// Report errors
	if (!error.empty()) {
		crash_writer::Write(error);
		OnUnhandledException();
	}

	ExitMainLoop();
	return 1;
}

void AegisubApp::MacOpenFiles(wxArrayString const& filenames) {
	OpenFiles(filenames);
}

void AegisubApp::OpenFiles(wxArrayStringsAdapter filenames) {
	std::vector<agi::fs::path> files;
	for (size_t i = 0; i < filenames.GetCount(); ++i)
		files.push_back(PathFromUtf8(from_wx(filenames[i])));
	if (!files.empty())
		frames[0]->context->GetCore().project->LoadList(files);
}
