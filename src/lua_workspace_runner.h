#pragma once

#include "automation/automation_debug_session.h"
#include "automation/lua_workspace_run.h"
#include "ui_dispatch.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

class LuaWorkspaceBackgroundRunner final : public agi::BackgroundRunner {
	class Sink final : public agi::ProgressSink {
		std::shared_ptr<std::atomic<bool>> stop;
		std::mutex mutex;
		std::string title;
		std::string message;
		std::string progress;
		std::string log;
		bool changed = true;
		bool truncated = false;

		static std::string Prefix(std::string const& value, size_t limit) {
			if (value.size() <= limit)
				return value;
			while (limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80)
				--limit;
			return value.substr(0, limit);
		}

		public:
		explicit Sink(std::shared_ptr<std::atomic<bool>> stop) : stop(std::move(stop)) {}
		void SetIndeterminate() override {
			std::scoped_lock lock(mutex);
			progress.clear();
			changed = true;
		}
		void SetTitle(std::string const& value) override {
			std::scoped_lock lock(mutex);
			title = Prefix(value, 2048) + (value.size() > 2048 ? " [truncated]" : "");
			changed = true;
		}
		void SetMessage(std::string const& value) override {
			std::scoped_lock lock(mutex);
			message = Prefix(value, 2048) + (value.size() > 2048 ? " [truncated]" : "");
			changed = true;
		}
		void SetProgress(int64_t current, int64_t maximum) override {
			std::scoped_lock lock(mutex);
			progress = std::to_string(current) + " / " + std::to_string(maximum);
			changed = true;
		}
		void Log(std::string const& value) override {
			std::scoped_lock lock(mutex);
			constexpr size_t limit = 32768;
			if (value.size() >= limit) {
				auto start = value.size() - limit;
				while (start < value.size() && (static_cast<unsigned char>(value[start]) & 0xc0) == 0x80)
					++start;
				log = value.substr(start);
				truncated = true;
			}
			else {
				if (log.size() + value.size() > limit) {
					auto start = log.size() + value.size() - limit;
					while (start < log.size() && (static_cast<unsigned char>(log[start]) & 0xc0) == 0x80)
						++start;
					log.erase(0, start);
					truncated = true;
				}
				log += value;
			}
			changed = true;
		}
		bool IsCancelled() override { return stop->load(); }
		std::optional<std::string> TakeUpdate() {
			std::scoped_lock lock(mutex);
			if (!changed)
				return std::nullopt;
			changed = false;
			return title + "\n" + message + "\n" + progress + "\n\n" + (truncated ? "[earlier log truncated]\n" : "") + log;
		}
	};

	std::shared_ptr<Automation4::LuaWorkspaceRunRequest const> request;
	std::function<void(std::string const&)> report;

	void RequestStop() const {
		request->stop_requested->store(true);
		if (request->debug_session)
			request->debug_session->Detach();
	}

	public:
	LuaWorkspaceBackgroundRunner(std::shared_ptr<Automation4::LuaWorkspaceRunRequest const> request,
								 std::function<void(std::string const&)> report)
		: request(std::move(request)), report(std::move(report)) {}

	void Run(std::function<void(agi::ProgressSink *)> task) override {
		agi::ui::VerifyAccess();
		Sink sink(request->stop_requested);
		wxEventLoop loop;
		wxEvtHandler events;
		wxTimer timer(&events);
		std::atomic<bool> done = false;
		std::exception_ptr worker_error;
		std::exception_ptr ui_error;
		events.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
			try {
				if (auto update = sink.TakeUpdate())
					report(*update);
			}
			catch (...) {
				if (!ui_error)
					ui_error = std::current_exception();
				RequestStop();
			}
			if (done.load())
				loop.Exit();
		});
		if (!timer.Start(30))
			throw std::runtime_error("Could not start the Lua Workspace event-loop timer");
		std::jthread worker([&] {
			try {
				task(&sink);
			}
			catch (...) {
				worker_error = std::current_exception();
			}
			done.store(true);
		});
		try {
			loop.Run();
		}
		catch (...) {
			ui_error = std::current_exception();
			RequestStop();
		}
		timer.Stop();
		if (!done.load()) {
			RequestStop();
			wxEventLoopActivator activate(&loop);
			while (!done.load()) {
				try {
					wxTheApp->ProcessPendingEvents();
					if (loop.DispatchTimeout(30) < 0)
						std::this_thread::sleep_for(std::chrono::milliseconds(30));
				}
				catch (...) {
					if (!ui_error)
						ui_error = std::current_exception();
				}
			}
		}
		worker.join();
		try {
			if (auto update = sink.TakeUpdate())
				report(*update);
		}
		catch (...) {
			if (!ui_error)
				ui_error = std::current_exception();
		}
		if (worker_error)
			std::rethrow_exception(worker_error);
		if (ui_error)
			std::rethrow_exception(ui_error);
	}
};
