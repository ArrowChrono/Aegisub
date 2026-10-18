#include "discord_presence_client.h"

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/fs.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#ifdef _WIN32
#include <boost/asio/windows/stream_handle.hpp>
#else
#include <boost/asio/local/stream_protocol.hpp>
#include <unistd.h>
#endif

#include <array>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace discord_presence {
namespace {

constexpr char const *N_(char const *msgid) { return msgid; }

namespace asio = boost::asio;
using namespace std::chrono_literals;
using Error = boost::system::error_code;
#ifdef _WIN32
using Stream = asio::windows::stream_handle;
#else
using Stream = asio::local::stream_protocol::socket;
#endif

std::string Serialize(json::Object const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

std::string Packet(uint32_t opcode, std::string_view payload) {
	std::string packet(8, '\0');
	auto const length = static_cast<uint32_t>(payload.size());
	for (size_t i = 0; i < 4; ++i) {
		packet[i] = static_cast<char>((opcode >> (i * 8)) & 0xff);
		packet[i + 4] = static_cast<char>((length >> (i * 8)) & 0xff);
	}
	packet.append(payload);
	return packet;
}

uint32_t ReadWord(unsigned char const *bytes) {
	return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

std::string StringField(json::Object const& object, char const *name) {
	auto found = object.find(name);
	if (found == object.end())
		return {};
	try {
		return static_cast<json::String const&>(found->second);
	}
	catch (json::Exception const&) {
		return {};
	}
}

std::vector<std::string> Endpoints(std::string const& endpoint) {
	if (!endpoint.empty())
		return {endpoint};
	std::vector<std::string> paths;
#ifdef _WIN32
	paths.reserve(10);
	for (int index = 0; index < 10; ++index)
		paths.push_back(R"(\\.\pipe\discord-ipc-)" + std::to_string(index));
#else
	std::vector<std::filesystem::path> directories;
	directories.reserve(5);
	for (auto const *name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
		if (auto const *value = std::getenv(name); value && *value)
			directories.emplace_back(value);
	}
	directories.push_back(std::filesystem::temp_directory_path());
	paths.reserve(directories.size() * 30);
	for (auto const& directory : directories) {
		for (auto const *relative : {"", "app/com.discordapp.Discord", "snap.discord"}) {
			for (int index = 0; index < 10; ++index)
				paths.push_back(agi::fs::PathToString(directory / relative / ("discord-ipc-" + std::to_string(index))));
		}
	}
#endif
	return paths;
}

}

struct Client::Impl {
	struct Request {
		std::optional<uint64_t> application_id;
		std::optional<Activity> activity;
		bool operator==(Request const&) const = default;
	};
	struct Connection {
		Stream stream;
		asio::steady_timer timeout;
		asio::steady_timer write_timeout;
		std::array<unsigned char, 8> header{};
		std::string body;
		std::deque<std::string> writes;
		bool ready = false;
		bool closing = false;

		explicit Connection(asio::io_context& io) : stream(io), timeout(io), write_timeout(io) {}
		void Close() {
			timeout.cancel();
			write_timeout.cancel();
			Error ignored;
			stream.close(ignored);
		}
	};

	mutable std::mutex mutex;
	Request desired;
	bool update_queued = false;
	std::string status = N_("Connecting to Discord");
	asio::io_context io;
	asio::executor_work_guard<asio::io_context::executor_type> work{io.get_executor()};
	asio::steady_timer retry{io};
	asio::steady_timer publish{io};
	asio::steady_timer shutdown{io};
	std::vector<std::string> endpoints;
	Request current;
	std::shared_ptr<Connection> connection;
	std::optional<Activity> last_sent;
	uint64_t nonce = 0;
	bool stopping = false;
	std::chrono::steady_clock::time_point reconnect_after{};
	std::thread worker;

	explicit Impl(std::string const& endpoint)
		: endpoints(Endpoints(endpoint)), worker([this] { io.run(); }) {}

	~Impl() {
		asio::post(io, [this] {
			stopping = true;
			retry.cancel();
			publish.cancel();
			Retire();
			shutdown.expires_after(250ms);
			shutdown.async_wait([this](Error const&) { io.stop(); });
		});
		worker.join();
	}

	void SetStatus(std::string value) {
		std::scoped_lock lock(mutex);
		status = std::move(value);
	}

	void Apply() {
		Request next;
		{
			std::scoped_lock lock(mutex);
			next = desired;
			update_queued = false;
		}
		if (stopping)
			return;
		auto const changed_id = next.application_id != current.application_id;
		auto const retired = connection && (changed_id || !next.activity);
		if (retired)
			Retire();
		current = std::move(next);
		if (!current.application_id || !current.activity) {
			retry.cancel();
			publish.cancel();
			SetStatus(N_("Disconnected"));
			return;
		}
		if (retired)
			Retry(250ms);
		else if (!connection)
			Connect(0);
		else if (connection->ready)
			SchedulePublish();
	}

	void Retry(std::chrono::milliseconds delay = 5s) {
		if (stopping || !current.application_id || !current.activity)
			return;
		retry.expires_after(delay);
		retry.async_wait([this](Error const& error) {
			if (!error)
				Connect(0);
		});
	}

	void Failed(std::shared_ptr<Connection> const& source, std::string message) {
		source->Close();
		if (source != connection)
			return;
		connection.reset();
		last_sent.reset();
		SetStatus(std::move(message));
		Retry();
	}

	void Connect(size_t index) {
		if (stopping || !current.application_id || !current.activity || connection)
			return;
		auto const now = std::chrono::steady_clock::now();
		if (now < reconnect_after) {
			Retry(std::chrono::ceil<std::chrono::milliseconds>(reconnect_after - now));
			return;
		}
		retry.cancel();
		if (index == endpoints.size()) {
			SetStatus(N_("Waiting for Discord"));
			Retry();
			return;
		}
		auto source = std::make_shared<Connection>(io);
#ifdef _WIN32
		auto handle = CreateFileA(endpoints[index].c_str(), GENERIC_READ | GENERIC_WRITE,
								  0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			Connect(index + 1);
			return;
		}
		Error error;
		source->stream.assign(handle, error);
		if (error) {
			CloseHandle(handle);
			Connect(index + 1);
			return;
		}
		connection = source;
		Handshake(source);
#else
		connection = source;
		source->timeout.expires_after(1s);
		source->timeout.async_wait([this, source, index](Error const& error) {
			if (!error && source == connection) {
				source->Close();
				connection.reset();
				Connect(index + 1);
			}
		});
		source->stream.async_connect(asio::local::stream_protocol::endpoint(endpoints[index]),
									 [this, source, index](Error const& error) {
										 if (source != connection)
											 return;
										 source->timeout.cancel();
										 if (error) {
											 source->Close();
											 connection.reset();
											 Connect(index + 1);
										 }
										 else
											 Handshake(source);
									 });
#endif
	}

	void Handshake(std::shared_ptr<Connection> const& source) {
		SetStatus(N_("Connecting to Discord"));
		json::Object handshake;
		handshake["v"] = int64_t{1};
		handshake["client_id"] = std::to_string(*current.application_id);
		Send(source, Packet(0, Serialize(handshake)));
		source->timeout.expires_after(5s);
		source->timeout.async_wait([this, source](Error const& error) {
			if (!error)
				Failed(source, N_("Discord connection timed out"));
		});
		Read(source);
	}

	void Send(std::shared_ptr<Connection> const& source, std::string packet) {
		if (source->writes.size() >= 16) {
			Failed(source, N_("Discord is not accepting updates"));
			return;
		}
		source->writes.push_back(std::move(packet));
		if (source->writes.size() == 1)
			Write(source);
	}

	void Write(std::shared_ptr<Connection> const& source) {
		source->write_timeout.expires_after(5s);
		source->write_timeout.async_wait([this, source](Error const& error) {
			if (!error)
				Failed(source, N_("Discord update timed out"));
		});
		asio::async_write(source->stream, asio::buffer(source->writes.front()),
						  [this, source](Error const& error, size_t) {
							  source->write_timeout.cancel();
							  if (error) {
								  Failed(source, N_("Waiting for Discord"));
								  return;
							  }
							  source->writes.pop_front();
							  if (!source->writes.empty())
								  Write(source);
							  else if (source->closing)
								  source->Close();
						  });
	}

	void Read(std::shared_ptr<Connection> const& source) {
		asio::async_read(source->stream, asio::buffer(source->header),
						 [this, source](Error const& error, size_t) {
							 if (source != connection)
								 return;
							 if (error) {
								 Failed(source, N_("Waiting for Discord"));
								 return;
							 }
							 auto const opcode = ReadWord(source->header.data());
							 auto const length = ReadWord(source->header.data() + 4);
							 if (opcode > 4 || length > 64 * 1024) {
								 Failed(source, N_("Invalid Discord response"));
								 return;
							 }
							 source->body.resize(length);
							 asio::async_read(source->stream, asio::buffer(source->body),
											  [this, source, opcode](Error const& body_error, size_t) {
												  if (source != connection)
													  return;
												  if (body_error) {
													  Failed(source, N_("Waiting for Discord"));
													  return;
												  }
												  Receive(source, opcode);
												  if (source == connection)
													  Read(source);
											  });
						 });
	}

	void Receive(std::shared_ptr<Connection> const& source, uint32_t opcode) {
		if (opcode == 3) {
			Send(source, Packet(4, source->body));
			return;
		}
		if (opcode == 4)
			return;
		if (opcode == 2) {
			Failed(source, N_("Discord closed the connection"));
			return;
		}
		if (opcode != 1) {
			Failed(source, N_("Invalid Discord response"));
			return;
		}
		try {
			std::istringstream input(source->body);
			json::UnknownElement root;
			json::Reader::Read(root, input);
			auto const& response = static_cast<json::Object const&>(root);
			auto const event = StringField(response, "evt");
			if (event == "ERROR") {
				Failed(source, N_("Discord rejected the activity or Application ID"));
				return;
			}
			if (event == "READY" && StringField(response, "cmd") == "DISPATCH") {
				source->timeout.cancel();
				source->ready = true;
				Publish();
			}
			else if (StringField(response, "cmd") == "SET_ACTIVITY")
				SetStatus(N_("Connected"));
		}
		catch (std::exception const&) {
			Failed(source, N_("Invalid Discord response"));
		}
	}

	std::string ActivityPacket(std::optional<Activity> const& value) {
		json::Object args;
#ifdef _WIN32
		args["pid"] = static_cast<int64_t>(GetCurrentProcessId());
#else
		args["pid"] = static_cast<int64_t>(getpid());
#endif
		args["activity"] = json::Null();
		if (value) {
			json::Object timestamps;
			timestamps["start"] = value->started_at;
			json::Object activity;
			activity["name"] = value->name;
			activity["type"] = int64_t{0};
			activity["details"] = value->details;
			activity["state"] = value->state;
			activity["timestamps"] = std::move(timestamps);
			if (!value->large_image.empty()) {
				json::Object assets;
				assets["large_image"] = value->large_image;
				activity["assets"] = std::move(assets);
			}
			args["activity"] = std::move(activity);
		}
		json::Object command;
		command["cmd"] = "SET_ACTIVITY";
		command["args"] = std::move(args);
		command["nonce"] = std::to_string(++nonce);
		return Packet(1, Serialize(command));
	}

	void SchedulePublish() {
		publish.expires_after(1s);
		publish.async_wait([this](Error const& error) {
			if (!error)
				Publish();
		});
	}

	void Publish() {
		if (!connection || !connection->ready || !current.activity || last_sent == current.activity)
			return;
		Send(connection, ActivityPacket(current.activity));
		last_sent = current.activity;
	}

	void Retire() {
		publish.cancel();
		last_sent.reset();
		auto source = std::exchange(connection, {});
		if (!source)
			return;
		if (!source->ready) {
			source->Close();
			return;
		}
		source->closing = true;
		reconnect_after = std::chrono::steady_clock::now() + 250ms;
		Send(source, ActivityPacket(std::nullopt));
		source->timeout.expires_after(200ms);
		source->timeout.async_wait([source](Error const& error) {
			if (!error)
				source->Close();
		});
	}
};

Client::Client(std::string const& endpoint) : impl(std::make_unique<Impl>(endpoint)) {}
Client::~Client() = default;

void Client::Update(std::optional<uint64_t> application_id, std::optional<Activity> activity) {
	std::scoped_lock lock(impl->mutex);
	Impl::Request next{.application_id = application_id, .activity = std::move(activity)};
	if (next == impl->desired)
		return;
	impl->desired = std::move(next);
	if (!impl->update_queued) {
		impl->update_queued = true;
		asio::post(impl->io, [state = impl.get()] { state->Apply(); });
	}
}

std::string Client::GetStatus() const {
	std::scoped_lock lock(impl->mutex);
	return impl->status;
}

}
