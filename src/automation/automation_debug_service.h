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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "automation_debug_session.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace Automation4 {
	struct AutomationDebugLiveEndpoint {
		bool available = false;
		std::string host = "127.0.0.1";
		int port = 0;
		std::string token;
	};

	struct AutomationDebugServiceStateSnapshot {
		size_t version = 0;
		bool enabled = false;
		bool client_connected = false;
		bool client_configured = false;
		size_t session_generation = 0;
		AutomationDebugLiveEndpoint endpoint;
	};

	class AutomationDebugService final {
		class Impl;

		mutable std::mutex mutex;
		mutable std::condition_variable cv;
		bool enabled = false;
		bool client_connected = false;
		bool client_configured = false;
		size_t state_version = 0;
		size_t session_generation = 0;
		std::shared_ptr<AutomationDebugSession> current_session;
		std::shared_ptr<AutomationDebugSession> local_session;
		AutomationDebugLiveEndpoint endpoint;
		AutomationDebugLaunchRequest launch_configuration;
		std::unique_ptr<Impl> impl;

	public:
		AutomationDebugService();
		~AutomationDebugService();

		AutomationDebugService(AutomationDebugService const&) = delete;
		AutomationDebugService& operator=(AutomationDebugService const&) = delete;

		bool IsEnabled() const;
		bool SetEnabled(bool value);
		bool Toggle();

		AutomationDebugLiveEndpoint GetEndpoint() const;
		AutomationDebugServiceStateSnapshot GetStateSnapshot() const;
		AutomationDebugServiceStateSnapshot WaitForStateChange(size_t after_version) const;
		AutomationDebugLaunchRequest GetLaunchConfiguration() const;
		void SetLaunchConfiguration(AutomationDebugLaunchRequest request);
		bool ReportClientState(bool connected, bool configured);
		void NotifyStateChange();
		std::shared_ptr<AutomationDebugSession> GetCurrentSession() const;
		bool HasLocalSession() const;
		[[nodiscard]] bool OwnsLocalSession(std::shared_ptr<AutomationDebugSession> const& session) const;
		std::shared_ptr<AutomationDebugSession> PrepareLocalSession(
			AutomationDebugTarget target,
			AutomationDebugLaunchRequest request);
		std::shared_ptr<AutomationDebugSession> PrepareSession(
			AutomationDebugTarget target,
			AutomationDebugLaunchRequest request = {});
		void ClearSession(std::shared_ptr<AutomationDebugSession> const& session);
	};
}
