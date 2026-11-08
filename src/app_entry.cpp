// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "headless_process_entry.h"
#include "app_launch_plan.h"
#include "perf_trace.h"

#include <chrono>
#include <vector>
#include <string>
#include <cstdio>
#include <iostream>

#include <wx/app.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "gui_test_wx_boundary.h"

namespace {

std::string WideToUtf8(wchar_t const* value) {
	if (!value)
		return {};
	int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1)
		return {};
	std::string out(static_cast<size_t>(size - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
	return out;
}

std::vector<std::string> CurrentProcessArgs() {
	int argc = 0;
	auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
		return {};

	struct ScopedArgv final {
		wchar_t** argv = nullptr;
		~ScopedArgv() {
			if (argv)
				LocalFree(argv);
		}
	} scoped{argv};

	std::vector<std::string> args;
	args.reserve(argc);
	for (int i = 0; i < argc; ++i)
		args.push_back(WideToUtf8(argv[i]));
	return args;
}

bool HasBoundStdHandle(DWORD handle_id) {
	auto handle = GetStdHandle(handle_id);
	if (!handle || handle == INVALID_HANDLE_VALUE)
		return false;

	SetLastError(ERROR_SUCCESS);
	auto const file_type = GetFileType(handle);
	if (file_type != FILE_TYPE_UNKNOWN)
		return true;
	return GetLastError() == ERROR_SUCCESS;
}

// GUI-subsystem binaries do not own a console. AttachConsole lets CLI output
// reach the parent shell. The attached console must be released explicitly so
// stream cleanup does not depend on process teardown.
bool EnsurePlainProcessConsoleStreams() {
	auto const needs_stdout = !HasBoundStdHandle(STD_OUTPUT_HANDLE);
	auto const needs_stderr = !HasBoundStdHandle(STD_ERROR_HANDLE);
	if (!needs_stdout && !needs_stderr)
		return false;

	if (!AttachConsole(ATTACH_PARENT_PROCESS))
		return false;

	// Do not reopen CONIN$: sharing console input with the parent shell makes
	// the stuck-prompt problem worse and is unnecessary for current CLI paths.
	FILE* stream = nullptr;
	if (needs_stdout) {
		freopen_s(&stream, "CONOUT$", "w", stdout);
		setvbuf(stdout, nullptr, _IONBF, 0);
	}
	if (needs_stderr) {
		freopen_s(&stream, "CONOUT$", "w", stderr);
		setvbuf(stderr, nullptr, _IONBF, 0);
	}

	std::ios::sync_with_stdio(true);
	std::cout.clear();
	std::cerr.clear();
	std::clog.clear();
	return true;
}

void FlushPlainProcessConsoleStreams() {
	std::cout.flush();
	std::cerr.flush();
	std::clog.flush();
	fflush(stdout);
	fflush(stderr);
}

class ScopedParentConsoleAttachment final {
	bool attached;

public:
	explicit ScopedParentConsoleAttachment(bool attached) noexcept
	: attached(attached) {
	}

	~ScopedParentConsoleAttachment() {
		if (!attached)
			return;
		FlushPlainProcessConsoleStreams();
		FreeConsole();
	}

	ScopedParentConsoleAttachment(ScopedParentConsoleAttachment const&) = delete;
	ScopedParentConsoleAttachment& operator=(ScopedParentConsoleAttachment const&) = delete;
};

}

extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, wxCmdLineArgType lpCmdLine, int nCmdShow) {
	wxDISABLE_DEBUG_SUPPORT();

	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase(
			"main",
			phase,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_started).count());
		phase_started = std::chrono::steady_clock::now();
	};

	auto const args = CurrentProcessArgs();
	auto const launch_plan = ParseAppLaunchPlan(args);
	EnableWxBoundaryTestAssertions(launch_plan);
	observe_phase("startup.entry.capture_process_args");
	if (launch_plan.RequestedPlainProcess()) {
		observe_phase("startup.entry.headless_command_check");
		ScopedParentConsoleAttachment console(EnsurePlainProcessConsoleStreams());
		return RunAppLaunchPlanInPlainProcessHost(launch_plan);
	}
	observe_phase("startup.entry.headless_command_check");
	observe_phase("startup.entry.before_wx_entry");

	return wxEntry(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}

#else

int main(int argc, char** argv) {
	wxDISABLE_DEBUG_SUPPORT();

	auto phase_started = std::chrono::steady_clock::now();
	auto observe_phase = [&](char const* phase) {
		perf_trace::ObserveWindowOpenPhase(
			"main",
			phase,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_started).count());
		phase_started = std::chrono::steady_clock::now();
	};

	std::vector<std::string> args;
	args.reserve(argc);
	for (int i = 0; i < argc; ++i)
		args.emplace_back(argv[i]);
	auto const launch_plan = ParseAppLaunchPlan(args);
	observe_phase("startup.entry.capture_process_args");

	if (launch_plan.RequestedPlainProcess())
		return RunAppLaunchPlanInPlainProcessHost(launch_plan);
	observe_phase("startup.entry.headless_command_check");
	observe_phase("startup.entry.before_wx_entry");

	return wxEntry(argc, argv);
}

#endif
