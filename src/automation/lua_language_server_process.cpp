#include "lua_language_server_process.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Automation4 {
namespace {
constexpr std::size_t kMaxChunk = 8 * 1024 * 1024;
constexpr std::size_t kReadChunk = 64 * 1024;

#ifdef _WIN32

void Close(HANDLE& handle) {
	if (handle && handle != INVALID_HANDLE_VALUE)
		CloseHandle(handle);
	handle = nullptr;
}

std::string WindowsError(char const *action) {
	return std::string(action) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

std::wstring Utf8ToWide(std::string const& value) {
	if (value.empty())
		return {};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		return {};
	int const size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (size <= 0)
		return {};
	std::wstring result(static_cast<std::size_t>(size), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) != size)
		return {};
	return result;
}

std::wstring QuoteArgument(std::wstring const& value) {
	if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring::npos)
		return value;
	std::wstring quoted(1, L'"');
	std::size_t slashes = 0;
	for (wchar_t character : value) {
		if (character == L'\\') {
			++slashes;
			continue;
		}
		if (character == L'"') {
			quoted.append(slashes * 2 + 1, L'\\');
			quoted.push_back(L'"');
		}
		else {
			quoted.append(slashes, L'\\');
			quoted.push_back(character);
		}
		slashes = 0;
	}
	quoted.append(slashes * 2, L'\\');
	quoted.push_back(L'"');
	return quoted;
}

DWORD WaitMilliseconds(std::chrono::milliseconds timeout) {
	return static_cast<DWORD>(std::clamp<std::int64_t>(timeout.count(), 0, MAXDWORD - 1));
}

struct PendingConnect {
	OVERLAPPED operation{};
	HANDLE event = nullptr;
};

DWORD WINAPI RetireConnect(LPVOID context) {
	std::unique_ptr<PendingConnect> pending(static_cast<PendingConnect *>(context));
	WaitForSingleObject(pending->event, INFINITE);
	CloseHandle(pending->event);
	return 0;
}

void RetirePendingConnect(PendingConnect *pending) {
	HANDLE thread = CreateThread(nullptr, 0, RetireConnect, pending, 0, nullptr);
	if (thread)
		CloseHandle(thread);
}

bool CreateOverlappedPipe(bool parent_reads, HANDLE& parent, HANDLE& child, std::string& error) {
	static std::atomic<unsigned long long> next_pipe{0};
	std::wstring const name = L"\\\\.\\pipe\\aegisub-luals-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++next_pipe);
	parent = CreateNamedPipeW(name.c_str(),
							  (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED,
							  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
							  1, static_cast<DWORD>(kReadChunk), static_cast<DWORD>(kReadChunk), 0, nullptr);
	if (parent == INVALID_HANDLE_VALUE) {
		error = WindowsError("Could not create LuaLS pipe");
		return false;
	}
	SECURITY_ATTRIBUTES inherit{.nLength = sizeof(inherit), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
	child = CreateFileW(name.c_str(), parent_reads ? GENERIC_WRITE : GENERIC_READ,
						0, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (child == INVALID_HANDLE_VALUE) {
		error = WindowsError("Could not open LuaLS pipe");
		return false;
	}
	auto pending = std::make_unique<PendingConnect>();
	pending->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!pending->event) {
		error = WindowsError("Could not connect LuaLS pipe");
		return false;
	}
	pending->operation.hEvent = pending->event;
	BOOL connected = ConnectNamedPipe(parent, &pending->operation);
	DWORD const status = connected ? ERROR_SUCCESS : GetLastError();
	if (status == ERROR_IO_PENDING) {
		if (WaitForSingleObject(pending->event, 1000) != WAIT_OBJECT_0) {
			CancelIoEx(parent, &pending->operation);
			Close(parent);
			RetirePendingConnect(pending.release());
			error = "Timed out connecting LuaLS pipe";
			return false;
		}
		DWORD transferred = 0;
		connected = GetOverlappedResult(parent, &pending->operation, &transferred, FALSE);
	}
	else
		connected = connected || status == ERROR_PIPE_CONNECTED;
	CloseHandle(pending->event);
	if (!connected) {
		error = WindowsError("Could not connect LuaLS pipe");
		return false;
	}
	return true;
}

#else

void Close(int& descriptor) {
	if (descriptor >= 0)
		close(descriptor);
	descriptor = -1;
}

int PollMilliseconds(std::chrono::milliseconds timeout) {
	return static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 0, std::numeric_limits<int>::max()));
}

bool SetNonblocking(int descriptor) {
	int const flags = fcntl(descriptor, F_GETFL, 0);
	return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

#endif
}

struct LuaLanguageServerProcess::Impl {
#ifdef _WIN32
	HANDLE stdin_pipe = nullptr;
	HANDLE stdout_pipe = nullptr;
	HANDLE child_stdin = nullptr;
	HANDLE child_stdout = nullptr;
	HANDLE child_stderr = nullptr;
	HANDLE process = nullptr;
	HANDLE job = nullptr;
	HANDLE read_event = nullptr;
	HANDLE write_event = nullptr;
	OVERLAPPED read_operation{};
	OVERLAPPED write_operation{};
	bool read_pending = false;
	bool write_pending = false;
	bool write_faulted = false;
	bool read_closed = false;
	DWORD pid = 0;
	std::array<char, kReadChunk> read_buffer{};
	std::string write_buffer;

	static DWORD WINAPI Retire(LPVOID context) {
		std::unique_ptr<Impl> pending(static_cast<Impl *>(context));
		if (pending->read_pending)
			WaitForSingleObject(pending->read_event, INFINITE);
		if (pending->write_pending)
			WaitForSingleObject(pending->write_event, INFINITE);
		Close(pending->read_event);
		Close(pending->write_event);
		return 0;
	}
#else
	int stdin_socket = -1;
	int stdout_socket = -1;
	pid_t pid = -1;
	bool read_closed = false;
	bool write_faulted = false;
	std::array<char, kReadChunk> read_buffer{};
#endif

	void Stop() {
#ifdef _WIN32
		if (stdin_pipe && write_pending)
			CancelIoEx(stdin_pipe, &write_operation);
		if (stdout_pipe && read_pending)
			CancelIoEx(stdout_pipe, &read_operation);
		Close(stdin_pipe);
		Close(child_stdin);
		Close(child_stdout);
		Close(child_stderr);
		if (process && WaitForSingleObject(process, 150) == WAIT_TIMEOUT && job) {
			TerminateJobObject(job, 1);
			WaitForSingleObject(process, 500);
		}
		Close(job);
		Close(process);
		Close(stdout_pipe);
		if (read_pending && WaitForSingleObject(read_event, 100) == WAIT_OBJECT_0)
			read_pending = false;
		if (write_pending && WaitForSingleObject(write_event, 100) == WAIT_OBJECT_0)
			write_pending = false;
		if (!read_pending) {
			Close(read_event);
			read_operation = {};
		}
		if (!write_pending) {
			Close(write_event);
			write_operation = {};
			write_buffer.clear();
		}
		write_faulted = false;
		read_closed = false;
		pid = 0;
#else
		if (stdin_socket >= 0)
			shutdown(stdin_socket, SHUT_WR);
		Close(stdin_socket);
		Close(stdout_socket);
		if (pid > 0) {
			int status = 0;
			bool reaped = false;
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
			while (std::chrono::steady_clock::now() < deadline) {
				pid_t const result = waitpid(pid, &status, WNOHANG);
				if (result == pid || (result < 0 && errno == ECHILD)) {
					reaped = true;
					break;
				}
				if (result < 0 && errno != EINTR)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!reaped) {
				kill(-pid, SIGKILL);
				kill(pid, SIGKILL);
				auto const kill_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
				while (std::chrono::steady_clock::now() < kill_deadline) {
					pid_t const result = waitpid(pid, &status, WNOHANG);
					if (result == pid || (result < 0 && errno == ECHILD))
						break;
					if (result < 0 && errno != EINTR)
						break;
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			}
		}
		pid = -1;
		read_closed = false;
		write_faulted = false;
#endif
	}
};

LuaLanguageServerProcess::LuaLanguageServerProcess() : impl(std::make_unique<Impl>()) {}
LuaLanguageServerProcess::~LuaLanguageServerProcess() { Stop(); }

bool LuaLanguageServerProcess::Start(agi::fs::path const& executable,
									 std::vector<std::string> const& arguments, agi::fs::path const& working_directory,
									 agi::fs::path const& stderr_log, std::string& error) {
	Stop();
	if (!impl)
		impl = std::make_unique<Impl>();
	error.clear();
	if (executable.empty() || working_directory.empty()) {
		error = "LuaLS executable and working directory are required";
		return false;
	}
#ifdef _WIN32
	std::wstring command_line = QuoteArgument(executable.wstring());
	for (auto const& argument : arguments) {
		auto wide = Utf8ToWide(argument);
		if (!argument.empty() && wide.empty()) {
			error = "LuaLS argument is not valid UTF-8";
			return false;
		}
		command_line.push_back(L' ');
		command_line += QuoteArgument(wide);
	}
	if (!CreateOverlappedPipe(false, impl->stdin_pipe, impl->child_stdin, error) || !CreateOverlappedPipe(true, impl->stdout_pipe, impl->child_stdout, error)) {
		Stop();
		return false;
	}
	SECURITY_ATTRIBUTES inherit{.nLength = sizeof(inherit), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
	impl->child_stderr = CreateFileW(stderr_log.empty() ? L"NUL" : stderr_log.c_str(),
									 GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
									 stderr_log.empty() ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (impl->child_stderr == INVALID_HANDLE_VALUE) {
		error = WindowsError("Could not open LuaLS stderr log");
		Stop();
		return false;
	}
	impl->read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	impl->write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	impl->job = CreateJobObjectW(nullptr, nullptr);
	if (!impl->read_event || !impl->write_event || !impl->job) {
		error = WindowsError("Could not initialize LuaLS process controls");
		Stop();
		return false;
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
		error = WindowsError("Could not protect LuaLS process tree");
		Stop();
		return false;
	}
	SIZE_T attributes_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
	auto attributes = std::make_unique<std::byte[]>(attributes_size);
	auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.get());
	if (!InitializeProcThreadAttributeList(list, 1, 0, &attributes_size)) {
		error = WindowsError("Could not limit LuaLS inherited handles");
		Stop();
		return false;
	}
	HANDLE inherited[] = {impl->child_stdin, impl->child_stdout, impl->child_stderr};
	bool const handles_ready = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
														 inherited, sizeof(inherited), nullptr, nullptr) != 0;
	if (!handles_ready) {
		error = WindowsError("Could not limit LuaLS inherited handles");
		DeleteProcThreadAttributeList(list);
		Stop();
		return false;
	}
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = impl->child_stdin;
	startup.StartupInfo.hStdOutput = impl->child_stdout;
	startup.StartupInfo.hStdError = impl->child_stderr;
	startup.lpAttributeList = list;
	PROCESS_INFORMATION created{};
	bool const launched = CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
										 TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
										 nullptr, working_directory.c_str(), &startup.StartupInfo, &created) != 0;
	if (!launched)
		error = WindowsError("Could not start LuaLS");
	DeleteProcThreadAttributeList(list);
	if (!launched) {
		Stop();
		return false;
	}
	impl->process = created.hProcess;
	impl->pid = created.dwProcessId;
	if (!AssignProcessToJobObject(impl->job, impl->process)) {
		error = WindowsError("Could not supervise LuaLS process tree");
		TerminateProcess(impl->process, 1);
		CloseHandle(created.hThread);
		Stop();
		return false;
	}
	if (ResumeThread(created.hThread) == static_cast<DWORD>(-1)) {
		error = WindowsError("Could not resume LuaLS");
		CloseHandle(created.hThread);
		Stop();
		return false;
	}
	CloseHandle(created.hThread);
	Close(impl->child_stdin);
	Close(impl->child_stdout);
	Close(impl->child_stderr);
	impl->read_operation.hEvent = impl->read_event;
	impl->write_operation.hEvent = impl->write_event;
	return true;
#else
	int input[2]{-1, -1};
	int output[2]{-1, -1};
	int launch_error[2]{-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, input) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, output) != 0 || pipe(launch_error) != 0) {
		error = std::string("Could not create LuaLS sockets: ") + std::strerror(errno);
		Close(input[0]);
		Close(input[1]);
		Close(output[0]);
		Close(output[1]);
		Close(launch_error[0]);
		Close(launch_error[1]);
		return false;
	}
	if (fcntl(launch_error[1], F_SETFD, FD_CLOEXEC) != 0) {
		error = "Could not configure LuaLS launch status";
		Close(input[0]);
		Close(input[1]);
		Close(output[0]);
		Close(output[1]);
		Close(launch_error[0]);
		Close(launch_error[1]);
		return false;
	}
	int const log = open(stderr_log.empty() ? "/dev/null" : stderr_log.c_str(),
						 O_WRONLY | O_NONBLOCK | (stderr_log.empty() ? 0 : O_CREAT | O_TRUNC), 0600);
	if (log < 0) {
		error = std::string("Could not open LuaLS stderr log: ") + std::strerror(errno);
		Close(input[0]);
		Close(input[1]);
		Close(output[0]);
		Close(output[1]);
		Close(launch_error[0]);
		Close(launch_error[1]);
		return false;
	}
	std::vector<std::string> argv_strings;
	argv_strings.reserve(arguments.size() + 1);
	argv_strings.push_back(agi::fs::PathToString(executable));
	argv_strings.insert(argv_strings.end(), arguments.begin(), arguments.end());
	std::vector<char *> argv;
	argv.reserve(argv_strings.size() + 1);
	for (auto& item : argv_strings)
		argv.push_back(item.data());
	argv.push_back(nullptr);
	pid_t const child = fork();
	if (child == 0) {
		close(launch_error[0]);
		setpgid(0, 0);
		if (chdir(working_directory.c_str()) != 0 || dup2(input[1], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) {
			int const failure = errno;
			(void)write(launch_error[1], &failure, sizeof(failure));
			_exit(127);
		}
		close(input[0]);
		close(input[1]);
		close(output[0]);
		close(output[1]);
		close(log);
		execv(executable.c_str(), argv.data());
		int const failure = errno;
		(void)write(launch_error[1], &failure, sizeof(failure));
		_exit(127);
	}
	close(log);
	Close(launch_error[1]);
	Close(input[1]);
	Close(output[1]);
	if (child < 0) {
		error = std::string("Could not start LuaLS: ") + std::strerror(errno);
		Close(input[0]);
		Close(output[0]);
		Close(launch_error[0]);
		return false;
	}
	setpgid(child, child);
	impl->pid = child;
	impl->stdin_socket = input[0];
	impl->stdout_socket = output[0];
#ifdef SO_NOSIGPIPE
	int const no_sigpipe = 1;
	if (setsockopt(impl->stdin_socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
		error = "Could not protect LuaLS stdin from SIGPIPE";
		Close(launch_error[0]);
		Stop();
		return false;
	}
#elif !defined(MSG_NOSIGNAL)
	error = "LuaLS stdin cannot be protected from SIGPIPE on this platform";
	Close(launch_error[0]);
	Stop();
	return false;
#endif
	if (!SetNonblocking(impl->stdin_socket) || !SetNonblocking(impl->stdout_socket)) {
		error = "Could not configure LuaLS sockets";
		Close(launch_error[0]);
		Stop();
		return false;
	}
	pollfd launch_status{launch_error[0], POLLIN, 0};
	if (poll(&launch_status, 1, 1000) <= 0) {
		error = "Timed out waiting for LuaLS launch";
		Close(launch_error[0]);
		Stop();
		return false;
	}
	int failure = 0;
	ssize_t const result = read(launch_error[0], &failure, sizeof(failure));
	Close(launch_error[0]);
	if (result != 0) {
		error = std::string("Could not launch LuaLS: ") + std::strerror(result > 0 ? failure : errno);
		Stop();
		return false;
	}
	return true;
#endif
}

bool LuaLanguageServerProcess::Write(std::string_view bytes, std::chrono::milliseconds timeout, std::string& error) {
	error.clear();
	if (!impl) {
		error = "LuaLS stdin is unavailable";
		return false;
	}
	if (bytes.size() > kMaxChunk) {
		error = "LuaLS write exceeds 8 MiB";
		return false;
	}
#ifdef _WIN32
	if (!impl->process || !impl->stdin_pipe || impl->write_faulted || impl->write_pending) {
		error = "LuaLS stdin is unavailable";
		return false;
	}
	if (bytes.empty())
		return true;
	impl->write_buffer.assign(bytes);
	std::size_t written = 0;
	auto const deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
	while (written < impl->write_buffer.size()) {
		if (written > 0 && std::chrono::steady_clock::now() >= deadline) {
			error = "Timed out writing to LuaLS";
			impl->write_faulted = true;
			return false;
		}
		ResetEvent(impl->write_event);
		impl->write_operation = {};
		impl->write_operation.hEvent = impl->write_event;
		DWORD transferred = 0;
		auto const remaining = static_cast<DWORD>(impl->write_buffer.size() - written);
		BOOL const completed = WriteFile(impl->stdin_pipe, impl->write_buffer.data() + written,
										 remaining, &transferred, &impl->write_operation);
		if (!completed) {
			DWORD const code = GetLastError();
			if (code != ERROR_IO_PENDING) {
				error = WindowsError("Could not write to LuaLS");
				impl->write_faulted = true;
				return false;
			}
			impl->write_pending = true;
			auto const now = std::chrono::steady_clock::now();
			auto const remaining_time = now < deadline
											? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
											: std::chrono::milliseconds::zero();
			if (WaitForSingleObject(impl->write_event, WaitMilliseconds(remaining_time)) != WAIT_OBJECT_0) {
				error = "Timed out writing to LuaLS";
				impl->write_faulted = true;
				return false;
			}
			if (!GetOverlappedResult(impl->stdin_pipe, &impl->write_operation, &transferred, FALSE)) {
				error = WindowsError("Could not write to LuaLS");
				impl->write_faulted = true;
				return false;
			}
			impl->write_pending = false;
		}
		if (transferred == 0) {
			error = "LuaLS stdin closed during write";
			impl->write_faulted = true;
			return false;
		}
		written += transferred;
	}
	impl->write_buffer.clear();
	return true;
#else
	if (impl->pid <= 0 || impl->stdin_socket < 0 || impl->write_faulted) {
		error = "LuaLS stdin is unavailable";
		return false;
	}
	auto const deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
	std::size_t written = 0;
	bool attempted = false;
	while (written < bytes.size()) {
		if (attempted && std::chrono::steady_clock::now() >= deadline) {
			error = "Timed out writing to LuaLS";
			impl->write_faulted = true;
			return false;
		}
		attempted = true;
#ifdef MSG_NOSIGNAL
		ssize_t const sent = send(impl->stdin_socket, bytes.data() + written, bytes.size() - written, MSG_NOSIGNAL);
#else
		ssize_t const sent = send(impl->stdin_socket, bytes.data() + written, bytes.size() - written, 0);
#endif
		if (sent > 0) {
			written += static_cast<std::size_t>(sent);
			continue;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent == 0) {
			error = "LuaLS stdin closed during write";
			impl->write_faulted = true;
			return false;
		}
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			auto const now = std::chrono::steady_clock::now();
			auto const wait = now < deadline
								  ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
								  : std::chrono::milliseconds::zero();
			pollfd descriptor{impl->stdin_socket, POLLOUT, 0};
			int const ready = poll(&descriptor, 1, PollMilliseconds(wait));
			if (ready > 0 || (ready < 0 && errno == EINTR))
				continue;
			error = ready == 0 ? "Timed out writing to LuaLS"
							   : std::string("Could not wait for LuaLS stdin: ") + std::strerror(errno);
			impl->write_faulted = true;
			return false;
		}
		error = std::string("Could not write to LuaLS: ") + std::strerror(errno);
		impl->write_faulted = true;
		return false;
	}
	return true;
#endif
}

LuaLanguageServerProcess::ReadState LuaLanguageServerProcess::Read(std::string& bytes,
																   std::chrono::milliseconds timeout, std::string& error) {
	bytes.clear();
	error.clear();
	if (!impl) {
		error = "LuaLS stdout is unavailable";
		return ReadState::Error;
	}
	if (impl->read_closed)
		return ReadState::Closed;
#ifdef _WIN32
	if (!impl->process || !impl->stdout_pipe) {
		error = "LuaLS stdout is unavailable";
		return ReadState::Error;
	}
	DWORD transferred = 0;
	if (!impl->read_pending) {
		ResetEvent(impl->read_event);
		impl->read_operation = {};
		impl->read_operation.hEvent = impl->read_event;
		if (ReadFile(impl->stdout_pipe, impl->read_buffer.data(),
					 static_cast<DWORD>(impl->read_buffer.size()), &transferred, &impl->read_operation)) {
			if (transferred == 0) {
				impl->read_closed = true;
				return ReadState::Closed;
			}
			bytes.assign(impl->read_buffer.data(), transferred);
			return ReadState::Data;
		}
		DWORD const code = GetLastError();
		if (code == ERROR_BROKEN_PIPE) {
			impl->read_closed = true;
			return ReadState::Closed;
		}
		if (code != ERROR_IO_PENDING) {
			error = WindowsError("Could not read LuaLS stdout");
			return ReadState::Error;
		}
		impl->read_pending = true;
	}
	DWORD const waited = WaitForSingleObject(impl->read_event, WaitMilliseconds(timeout));
	if (waited == WAIT_TIMEOUT)
		return ReadState::Timeout;
	if (waited != WAIT_OBJECT_0) {
		error = WindowsError("Could not wait for LuaLS stdout");
		return ReadState::Error;
	}
	impl->read_pending = false;
	if (!GetOverlappedResult(impl->stdout_pipe, &impl->read_operation, &transferred, FALSE)) {
		if (GetLastError() == ERROR_BROKEN_PIPE) {
			impl->read_closed = true;
			return ReadState::Closed;
		}
		error = WindowsError("Could not read LuaLS stdout");
		return ReadState::Error;
	}
	if (transferred == 0) {
		impl->read_closed = true;
		return ReadState::Closed;
	}
	bytes.assign(impl->read_buffer.data(), transferred);
	return ReadState::Data;
#else
	if (impl->pid <= 0 || impl->stdout_socket < 0) {
		error = "LuaLS stdout is unavailable";
		return ReadState::Error;
	}
	auto const deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
	bool attempted = false;
	for (;;) {
		if (attempted && std::chrono::steady_clock::now() >= deadline)
			return ReadState::Timeout;
		attempted = true;
		ssize_t const received = recv(impl->stdout_socket, impl->read_buffer.data(), impl->read_buffer.size(), 0);
		if (received > 0) {
			bytes.assign(impl->read_buffer.data(), static_cast<std::size_t>(received));
			return ReadState::Data;
		}
		if (received == 0) {
			impl->read_closed = true;
			return ReadState::Closed;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			error = std::string("Could not read LuaLS stdout: ") + std::strerror(errno);
			return ReadState::Error;
		}
		auto const now = std::chrono::steady_clock::now();
		auto const wait = now < deadline
							  ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
							  : std::chrono::milliseconds::zero();
		pollfd descriptor{impl->stdout_socket, POLLIN, 0};
		int const result = poll(&descriptor, 1, PollMilliseconds(wait));
		if (result == 0)
			return ReadState::Timeout;
		if (result < 0 && errno != EINTR) {
			error = std::string("Could not wait for LuaLS stdout: ") + std::strerror(errno);
			return ReadState::Error;
		}
	}
#endif
}

void LuaLanguageServerProcess::Stop() {
	if (!impl)
		return;
	impl->Stop();
#ifdef _WIN32
	if (impl->read_pending || impl->write_pending) {
		Impl *pending = impl.release();
		HANDLE thread = CreateThread(nullptr, 0, Impl::Retire, pending, 0, nullptr);
		if (thread)
			CloseHandle(thread);
	}
#endif
}

int LuaLanguageServerProcess::ProcessId() const {
	if (!impl)
		return 0;
	return std::max(static_cast<int>(impl->pid), 0);
}

}
