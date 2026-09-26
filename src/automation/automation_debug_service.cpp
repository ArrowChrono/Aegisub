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

#include "automation_debug_service.h"

#include "../options.h"
#include "automation_debug_adapter.h"

#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace Automation4 {
namespace {
constexpr size_t kMaxDapHeaderBytes = 16 * 1024;
constexpr size_t kMaxDapPayloadBytes = 16 * 1024 * 1024;
constexpr char kDapHeaderTerminator[] = "\r\n\r\n";

struct AutomationDebugListenerConfig {
	unsigned short port = 0;
	bool require_token = true;
	std::string token;
};

std::string Trim(std::string value)
{
	auto const not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

std::string GenerateToken()
{
	std::random_device rd;
	std::mt19937_64 rng(
		(static_cast<uint64_t>(rd()) << 32) ^
		static_cast<uint64_t>(rd()));
	std::uniform_int_distribution<uint64_t> dist;

	std::ostringstream out;
	out << std::hex << std::setfill('0')
		<< std::setw(16) << dist(rng)
		<< std::setw(16) << dist(rng);
	return out.str();
}

unsigned short NormalizeListenPort(int value)
{
	if (value <= 0 || value > 65535)
		return 0;
	return static_cast<unsigned short>(value);
}

AutomationDebugListenerConfig LoadListenerConfig()
{
	AutomationDebugListenerConfig config;
	config.port = NormalizeListenPort(config::GetIntOptionOrDefault("Automation/Debug/Listen Port", config.port));
	config.require_token = config::GetBoolOptionOrDefault("Automation/Debug/Require Token", config.require_token);
	config.token = Trim(config::GetStringOptionOrDefault("Automation/Debug/Token", config.token));
	return config;
}

std::string ResolveExpectedToken(AutomationDebugListenerConfig const& config)
{
	if (!config.require_token)
		return {};
	if (!config.token.empty())
		return config.token;
	return GenerateToken();
}

void WakeAutomationDebugAcceptor(unsigned short port)
{
	if (port == 0)
		return;

	boost::system::error_code ec;
	auto address = boost::asio::ip::make_address("127.0.0.1", ec);
	if (ec)
		return;

	boost::asio::io_context wake_context;
	boost::asio::ip::tcp::socket wake_socket(wake_context);
	wake_socket.connect({address, port}, ec);
	if (ec)
		return;

	wake_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
	wake_socket.close(ec);
}

template<typename Socket>
bool EnsureDapHeaderAvailable(Socket& socket, boost::asio::streambuf& input_buffer, std::string& error)
{
	for (;;) {
		auto data = input_buffer.data();
		auto begin = boost::asio::buffers_begin(data);
		auto end = boost::asio::buffers_end(data);
		if (std::search(begin, end, std::begin(kDapHeaderTerminator), std::end(kDapHeaderTerminator) - 1) != end)
			return true;

		if (input_buffer.size() >= kMaxDapHeaderBytes) {
			error = "DAP header exceeds 16 KB limit";
			return false;
		}

		boost::system::error_code ec;
		size_t const remaining = kMaxDapHeaderBytes - input_buffer.size();
		auto writable = input_buffer.prepare(std::min<size_t>(1024, remaining));
		size_t const bytes_read = socket.read_some(writable, ec);
		if (ec) {
			error = ec.message();
			return false;
		}
		if (bytes_read == 0) {
			error = "socket closed while reading DAP header";
			return false;
		}
		input_buffer.commit(bytes_read);
	}
}

class AutomationDebugSocketConnection final : public AutomationDebugAdapterConnection {
	using tcp = boost::asio::ip::tcp;

	mutable std::mutex io_mutex;
	tcp::socket socket;
	boost::asio::streambuf input_buffer;

public:
	explicit AutomationDebugSocketConnection(tcp::socket socket)
	: socket(std::move(socket))
	{
	}

	bool ReadProtocolMessage(std::string& payload, std::string& error) override
	{
		try {
			if (!EnsureDapHeaderAvailable(socket, input_buffer, error))
				return false;

			std::istream stream(&input_buffer);
			std::string line;
			size_t content_length = 0;
			bool saw_header = false;
			while (std::getline(stream, line)) {
				saw_header = true;
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty())
					break;

				auto separator = line.find(':');
				if (separator == std::string::npos)
					continue;
				auto header = Trim(line.substr(0, separator));
				auto value = Trim(line.substr(separator + 1));
				if (header == "Content-Length") {
					try {
						content_length = static_cast<size_t>(std::stoul(value));
					}
					catch (...) {
						error = "invalid Content-Length header";
						return false;
					}
					if (content_length > kMaxDapPayloadBytes) {
						error = "DAP payload exceeds 16 MB Content-Length limit";
						return false;
					}
				}
			}

			if (!saw_header || content_length == 0) {
				error = "missing DAP payload";
				return false;
			}

			if (input_buffer.size() < content_length) {
				boost::asio::read(
					socket,
					input_buffer,
					boost::asio::transfer_exactly(content_length - input_buffer.size()));
			}

			payload.resize(content_length);
			stream.read(payload.data(), static_cast<std::streamsize>(content_length));
			if (static_cast<size_t>(stream.gcount()) != content_length) {
				error = "truncated DAP payload";
				return false;
			}
			return true;
		}
		catch (std::exception const& e) {
			error = e.what();
			return false;
		}
		catch (...) {
			error = "socket read failed";
			return false;
		}
	}

	bool WriteProtocolMessage(std::string const& payload, std::string& error) override
	{
		std::ostringstream frame;
		frame << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
		auto serialized = frame.str();

		try {
			std::lock_guard<std::mutex> lock(io_mutex);
			boost::asio::write(socket, boost::asio::buffer(serialized));
			return true;
		}
		catch (std::exception const& e) {
			error = e.what();
			return false;
		}
		catch (...) {
			error = "socket write failed";
			return false;
		}
	}

	void Close() override
	{
		std::lock_guard<std::mutex> lock(io_mutex);
		boost::system::error_code ec;
		socket.shutdown(tcp::socket::shutdown_both, ec);
		socket.close(ec);
	}
};

}

class AutomationDebugService::Impl final {
	using tcp = boost::asio::ip::tcp;

	AutomationDebugService& service;
	std::mutex mutex;
	boost::asio::io_context io_context;
	std::unique_ptr<tcp::acceptor> acceptor;
	std::shared_ptr<AutomationDebugAdapter> active_adapter;
	std::thread active_adapter_thread;
	std::thread listener_thread;
	bool stop_requested = false;
	std::string token;

	void StopAdapter(std::shared_ptr<AutomationDebugAdapter> const& adapter)
	{
		if (adapter)
			adapter->Stop();
	}

	void JoinAdapterThread(std::thread& thread)
	{
		if (thread.joinable())
			thread.join();
	}

	void ReplaceActiveAdapter(std::shared_ptr<AutomationDebugAdapter> adapter)
	{
		std::shared_ptr<AutomationDebugAdapter> previous_adapter;
		std::thread previous_thread;
		{
			std::lock_guard<std::mutex> lock(mutex);
			previous_adapter = std::move(active_adapter);
			previous_thread = std::move(active_adapter_thread);
		}

		StopAdapter(previous_adapter);
		JoinAdapterThread(previous_thread);

		auto adapter_thread = std::thread([adapter] {
			adapter->Run();
		});

		bool should_discard = false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stop_requested)
				should_discard = true;
			else {
				active_adapter = std::move(adapter);
				active_adapter_thread = std::move(adapter_thread);
			}
		}
		if (!should_discard)
			return;

		StopAdapter(adapter);
		JoinAdapterThread(adapter_thread);
	}

	void AcceptLoop()
	{
		for (;;) {
			tcp::socket socket(io_context);
			boost::system::error_code ec;
			acceptor->accept(socket, ec);

			{
				std::lock_guard<std::mutex> lock(mutex);
				if (stop_requested)
					return;
			}

			if (ec) {
				if (ec == boost::asio::error::operation_aborted)
					return;
				continue;
			}

			auto adapter = std::make_shared<AutomationDebugAdapter>(
				service,
				token,
				std::make_unique<AutomationDebugSocketConnection>(std::move(socket)));
			ReplaceActiveAdapter(std::move(adapter));

			{
				std::lock_guard<std::mutex> lock(mutex);
				if (stop_requested)
					return;
			}
		}
	}

public:
	explicit Impl(AutomationDebugService& service)
	: service(service)
	{
	}

	bool Start(AutomationDebugLiveEndpoint& endpoint, AutomationDebugListenerConfig const& listener_config)
	{
		boost::system::error_code ec;
		auto address = boost::asio::ip::make_address("127.0.0.1", ec);
		if (ec)
			return false;

		acceptor = std::make_unique<tcp::acceptor>(io_context);
		acceptor->open(tcp::v4(), ec);
		if (ec)
			return false;

		acceptor->set_option(tcp::acceptor::reuse_address(true), ec);
		if (ec)
			return false;

		acceptor->bind(tcp::endpoint(address, listener_config.port), ec);
		if (ec)
			return false;

		acceptor->listen(1, ec);
		if (ec)
			return false;

		token = ResolveExpectedToken(listener_config);
		endpoint.available = true;
		endpoint.host = "127.0.0.1";
		endpoint.port = static_cast<int>(acceptor->local_endpoint().port());
		endpoint.token = token;

		listener_thread = std::thread([this] { AcceptLoop(); });
		return true;
	}

	void Stop()
	{
		std::shared_ptr<AutomationDebugAdapter> adapter;
		std::thread adapter_thread;
		unsigned short wake_port = 0;
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop_requested = true;
			adapter = std::move(active_adapter);
			adapter_thread = std::move(active_adapter_thread);
			if (acceptor && acceptor->is_open()) {
				boost::system::error_code ec;
				auto local_endpoint = acceptor->local_endpoint(ec);
				if (!ec)
					wake_port = local_endpoint.port();
			}
		}

		if (adapter)
			adapter->Stop();
		WakeAutomationDebugAcceptor(wake_port);
		if (acceptor) {
			boost::system::error_code ec;
			acceptor->cancel(ec);
			acceptor->close(ec);
		}
		io_context.stop();
		if (listener_thread.joinable())
			listener_thread.join();
		JoinAdapterThread(adapter_thread);
	}
};

AutomationDebugService::AutomationDebugService() = default;

AutomationDebugService::~AutomationDebugService()
{
	SetEnabled(false);
}

bool AutomationDebugService::IsEnabled() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return enabled;
}

bool AutomationDebugService::SetEnabled(bool value)
{
	std::unique_ptr<Impl> old_impl;
	std::shared_ptr<AutomationDebugSession> detached_session;

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (enabled == value)
			return enabled;

		if (!value) {
			enabled = false;
			client_connected = false;
			client_configured = false;
			launch_configuration = {};
			detached_session = current_session;
			current_session.reset();
			endpoint = {};
			old_impl = std::move(impl);
			++state_version;
			cv.notify_all();
		}
	}

	if (!value) {
		if (detached_session)
			detached_session->Detach();
		if (old_impl)
			old_impl->Stop();
		return false;
	}

	auto listener_config = LoadListenerConfig();
	AutomationDebugLiveEndpoint new_endpoint;
	auto new_impl = std::make_unique<Impl>(*this);
	if (!new_impl->Start(new_endpoint, listener_config))
		return false;

	{
		std::lock_guard<std::mutex> lock(mutex);
		enabled = true;
		client_connected = false;
		client_configured = false;
		launch_configuration = {};
		endpoint = std::move(new_endpoint);
		impl = std::move(new_impl);
		++state_version;
		cv.notify_all();
	}
	return true;
}

bool AutomationDebugService::Toggle()
{
	return SetEnabled(!IsEnabled());
}

AutomationDebugLiveEndpoint AutomationDebugService::GetEndpoint() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return endpoint;
}

AutomationDebugServiceStateSnapshot AutomationDebugService::GetStateSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return {
		state_version,
		enabled,
		client_connected,
		client_configured,
		session_generation,
		endpoint
	};
}

AutomationDebugServiceStateSnapshot AutomationDebugService::WaitForStateChange(size_t after_version) const
{
	std::unique_lock<std::mutex> lock(mutex);
	cv.wait(lock, [&] {
		return state_version != after_version;
	});
	return {
		state_version,
		enabled,
		client_connected,
		client_configured,
		session_generation,
		endpoint
	};
}

AutomationDebugLaunchRequest AutomationDebugService::GetLaunchConfiguration() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return launch_configuration;
}

void AutomationDebugService::SetLaunchConfiguration(AutomationDebugLaunchRequest request)
{
	std::lock_guard<std::mutex> lock(mutex);
	launch_configuration = std::move(request);
}

bool AutomationDebugService::ReportClientState(bool connected, bool configured)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (connected && local_session)
		return false;
	if (client_connected == connected && client_configured == configured)
		return true;

	client_connected = connected;
	client_configured = configured;
	++state_version;
	cv.notify_all();
	return true;
}

void AutomationDebugService::NotifyStateChange()
{
	std::lock_guard<std::mutex> lock(mutex);
	++state_version;
	cv.notify_all();
}

std::shared_ptr<AutomationDebugSession> AutomationDebugService::GetCurrentSession() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return current_session;
}

bool AutomationDebugService::HasLocalSession() const
{
	std::scoped_lock lock(mutex);
	return static_cast<bool>(local_session);
}

bool AutomationDebugService::OwnsLocalSession(std::shared_ptr<AutomationDebugSession> const& session) const
{
	std::scoped_lock lock(mutex);
	return session && local_session == session;
}

std::shared_ptr<AutomationDebugSession> AutomationDebugService::PrepareLocalSession(
	AutomationDebugTarget target,
	AutomationDebugLaunchRequest request)
{
	std::scoped_lock lock(mutex);
	if (local_session)
		throw std::runtime_error("a local automation debug session is already active");
	if (client_connected)
		throw std::runtime_error("a remote automation debug client is connected");
	if (current_session && current_session->GetStateSnapshot().invocation_active)
		throw std::runtime_error("a remote automation debug session is active");

	request.nonblocking = false;
	auto session = std::make_shared<AutomationDebugSession>(std::move(request));
	session->SetTarget(std::move(target));
	local_session = session;
	++session_generation;
	++state_version;
	cv.notify_all();
	return session;
}

std::shared_ptr<AutomationDebugSession> AutomationDebugService::PrepareSession(
	AutomationDebugTarget target,
	AutomationDebugLaunchRequest request)
{
	std::shared_ptr<AutomationDebugSession> detached_session;
	std::shared_ptr<AutomationDebugSession> session;

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (local_session)
			throw std::runtime_error("a local automation debug session is active");
		if (current_session && current_session->GetStateSnapshot().invocation_active)
			throw std::runtime_error("a remote automation debug session is active");
		if (!enabled || !client_configured)
			return {};

		if (!request.enabled)
			request = launch_configuration;

		request.enabled = true;
		request.nonblocking = false;

		session = std::make_shared<AutomationDebugSession>(std::move(request));
		session->SetTarget(std::move(target));

		detached_session = current_session;
		current_session = session;
		++session_generation;
		++state_version;
		cv.notify_all();
	}

	if (detached_session && detached_session != session)
		detached_session->Detach();

	return session;
}

void AutomationDebugService::ClearSession(std::shared_ptr<AutomationDebugSession> const& session)
{
	std::shared_ptr<AutomationDebugSession> detached_session;

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (session && local_session == session) {
			detached_session = local_session;
			local_session.reset();
			++session_generation;
			++state_version;
			cv.notify_all();
		}
		else if (session && current_session == session) {
			detached_session = current_session;
			current_session.reset();
			++session_generation;
			++state_version;
			cv.notify_all();
		}
	}

	if (detached_session)
		detached_session->Detach();
}

}
