#include "watched_file.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/scoped_ptr.h>
#include <libaegisub/util.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <wx/app.h>
#include <wx/event.h>
#include <wx/thread.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
wxDEFINE_EVENT(EVT_NATIVE_FILE_WATCH, wxThreadEvent);

using Notification = std::variant<FileSystemWatchEvent, std::string>;
using KernelHandle = agi::scoped_holder<HANDLE, decltype(&CloseHandle)>;

class WindowsFileSystemWatcherBackend final : public FileSystemWatcherBackend {
	struct ReadFailure {
		char const *action;
		DWORD code;
	};

	wxEvtHandler events;
	FileSystemWatcherListener *listener = nullptr;
	agi::fs::path directory;
	KernelHandle directory_handle{nullptr, CloseHandle};
	KernelHandle completion_event{nullptr, CloseHandle};
	KernelHandle stop_event{nullptr, CloseHandle};
	OVERLAPPED operation{};
	alignas(DWORD) std::array<std::byte, 64 * 1024> buffer{};
	bool pending = false;
	std::jthread worker;

	std::string WindowsError(char const *action, DWORD code) const {
		return std::string(action) + " failed for " + agi::fs::PathToString(directory) +
			   " (Windows error " + std::to_string(code) + "): " + agi::util::ErrorString(static_cast<int>(code));
	}

	void Queue(Notification const& notification) {
		auto event = std::make_unique<wxThreadEvent>(EVT_NATIVE_FILE_WATCH);
		event->SetPayload(notification);
		wxQueueEvent(&events, event.release());
	}

	void OnNotification(wxThreadEvent& event) {
		assert(wxIsMainThread());
		auto *current_listener = listener;
		if (!current_listener) return;
		auto notification = event.GetPayload<Notification>();
		if (auto *error = std::get_if<std::string>(&notification)) {
			current_listener->OnFileSystemWatchError(*error);
			return;
		}
		current_listener->OnFileSystemWatchEvent(std::get<FileSystemWatchEvent>(notification));
	}

	std::optional<ReadFailure> StartRead() {
		assert(!pending);
		if (!ResetEvent(completion_event))
			return ReadFailure{.action = "ResetEvent", .code = GetLastError()};
		operation = {};
		operation.hEvent = completion_event;
		constexpr DWORD flags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
								FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
								FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY;
		if (!ReadDirectoryChangesW(directory_handle, buffer.data(), static_cast<DWORD>(buffer.size()),
								   FALSE, flags, nullptr, &operation, nullptr))
			return ReadFailure{.action = "ReadDirectoryChangesW", .code = GetLastError()};
		pending = true;
		return std::nullopt;
	}

	void CancelPending() {
		if (!pending) return;
		if (!CancelIoEx(directory_handle, &operation)) {
			auto const code = GetLastError();
			if (code != ERROR_NOT_FOUND)
				LOG_E("file/watcher") << WindowsError("CancelIoEx", code);
		}
		DWORD transferred = 0;
		if (!GetOverlappedResult(directory_handle, &operation, &transferred, TRUE)) {
			auto const code = GetLastError();
			if (code != ERROR_OPERATION_ABORTED)
				LOG_E("file/watcher") << WindowsError("GetOverlappedResult after cancellation", code);
			if (code == ERROR_IO_INCOMPLETE)
				std::terminate();
		}
		pending = false;
	}

	void ReportIoFailure(std::string message) {
		FILE_STANDARD_INFO information{};
		if (GetFileInformationByHandleEx(directory_handle, FileStandardInfo, &information, sizeof(information))) {
			if (information.DeletePending) {
				Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Deleted, .path = directory});
				return;
			}
		}
		else {
			auto const code = GetLastError();
			message += "; " + WindowsError("GetFileInformationByHandleEx", code);
		}
		Queue(message);
	}

	bool InvalidateDirectory() {
		FILE_STANDARD_INFO information{};
		if (!GetFileInformationByHandleEx(directory_handle, FileStandardInfo, &information, sizeof(information))) {
			auto const code = GetLastError();
			Queue(WindowsError("GetFileInformationByHandleEx after notification overflow", code));
			return false;
		}
		if (information.DeletePending) {
			Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Deleted, .path = directory});
			return false;
		}
		Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Invalidated, .path = directory});
		return true;
	}

	bool ProcessChanges(DWORD transferred) {
		std::optional<agi::fs::path> old_path;
		auto flush_old_path = [&] {
			if (old_path) {
				Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Deleted, .path = std::move(*old_path)});
				old_path.reset();
			}
		};
		auto malformed = [&] {
			Queue("ReadDirectoryChangesW returned malformed notification data for " + agi::fs::PathToString(directory));
			return false;
		};
		if (transferred > buffer.size()) return malformed();
		constexpr auto header_size = offsetof(FILE_NOTIFY_INFORMATION, FileName);
		std::size_t offset = 0;
		while (true) {
			auto const remaining = transferred - offset;
			if (remaining < header_size) return malformed();
			auto const *entry = reinterpret_cast<FILE_NOTIFY_INFORMATION const *>(buffer.data() + offset);
			if (!entry->FileNameLength || entry->FileNameLength % sizeof(wchar_t) ||
				entry->FileNameLength > remaining - header_size)
				return malformed();
			auto const next = entry->NextEntryOffset;
			if (next && (next < header_size + entry->FileNameLength || next >= remaining || next % alignof(DWORD)))
				return malformed();
			auto path = directory / std::wstring(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
			if (entry->Action != FILE_ACTION_RENAMED_NEW_NAME)
				flush_old_path();
			switch (entry->Action) {
				case FILE_ACTION_ADDED:
					Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Created, .path = std::move(path)});
					break;
				case FILE_ACTION_REMOVED:
					Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Deleted, .path = std::move(path)});
					break;
				case FILE_ACTION_MODIFIED:
					Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Modified, .path = std::move(path)});
					break;
				case FILE_ACTION_RENAMED_OLD_NAME:
					old_path = std::move(path);
					break;
				case FILE_ACTION_RENAMED_NEW_NAME:
					if (old_path) {
						Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Renamed, .path = std::move(*old_path), .new_path = std::move(path)});
						old_path.reset();
					}
					else
						Queue(FileSystemWatchEvent{.kind = FileSystemWatchEventKind::Created, .path = std::move(path)});
					break;
				default:
					Queue("ReadDirectoryChangesW returned an unknown notification action for " + agi::fs::PathToString(directory));
					return false;
			}
			if (!next) break;
			offset += next;
		}
		flush_old_path();
		return true;
	}

	void Run() {
		auto cancel = agi::make_scope_exit([this] { CancelPending(); });
		std::array<HANDLE, 2> const handles{stop_event, completion_event};
		while (true) {
			auto const stopped = WaitForSingleObject(stop_event, 0);
			if (stopped == WAIT_OBJECT_0) return;
			if (stopped != WAIT_TIMEOUT) {
				auto const code = stopped == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
				Queue(WindowsError("WaitForSingleObject stop", code));
				return;
			}
			if (!pending) {
				if (auto error = StartRead()) {
					if (error->code == ERROR_NOTIFY_ENUM_DIR) {
						if (!InvalidateDirectory()) return;
						continue;
					}
					ReportIoFailure(WindowsError(error->action, error->code));
					return;
				}
			}
			auto const result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
			if (result == WAIT_OBJECT_0) return;
			if (result != WAIT_OBJECT_0 + 1) {
				auto const code = result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
				Queue(WindowsError("WaitForMultipleObjects", code));
				return;
			}
			DWORD transferred = 0;
			if (!GetOverlappedResult(directory_handle, &operation, &transferred, FALSE)) {
				auto const code = GetLastError();
				if (code != ERROR_IO_INCOMPLETE)
					pending = false;
				if (code == ERROR_NOTIFY_ENUM_DIR) {
					if (!InvalidateDirectory()) return;
					continue;
				}
				ReportIoFailure(WindowsError("GetOverlappedResult", code));
				return;
			}
			pending = false;
			if (!transferred) {
				if (!InvalidateDirectory()) return;
				continue;
			}
			if (!ProcessChanges(transferred)) return;
		}
	}

	public:
	WindowsFileSystemWatcherBackend() {
		assert(wxIsMainThread());
		events.Bind(EVT_NATIVE_FILE_WATCH, &WindowsFileSystemWatcherBackend::OnNotification, this);
	}

	~WindowsFileSystemWatcherBackend() override {
		Reset();
	}

	bool WatchDirectory(agi::fs::path const& path, FileSystemWatcherListener *new_listener) override {
		Reset();
		auto rollback = agi::make_scope_exit([this] { Reset(); });
		directory = path;
		auto const handle = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
										FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
										FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			auto const code = GetLastError();
			LOG_E("file/watcher") << WindowsError("CreateFileW", code);
			return false;
		}
		directory_handle = handle;
		completion_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!completion_event) {
			auto const code = GetLastError();
			LOG_E("file/watcher") << WindowsError("CreateEventW completion", code);
			return false;
		}
		stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!stop_event) {
			auto const code = GetLastError();
			LOG_E("file/watcher") << WindowsError("CreateEventW stop", code);
			return false;
		}
		if (auto error = StartRead()) {
			LOG_E("file/watcher") << WindowsError(error->action, error->code);
			return false;
		}
		listener = new_listener;
		worker = std::jthread([this] { Run(); });
		rollback.release();
		return true;
	}

	void Reset() override {
		assert(wxIsMainThread());
		listener = nullptr;
		if (stop_event && !SetEvent(stop_event)) {
			auto const code = GetLastError();
			LOG_E("file/watcher") << WindowsError("SetEvent stop", code);
			std::terminate();
		}
		if (worker.joinable())
			worker.join();
		else
			CancelPending();
		if (wxTheApp)
			wxTheApp->RemovePendingEventHandler(&events);
		events.DeletePendingEvents();
		directory_handle = nullptr;
		completion_event = nullptr;
		stop_event = nullptr;
		directory.clear();
	}
};
}

std::unique_ptr<FileSystemWatcherBackend> CreateWinFileSystemWatcherBackend() {
	return std::make_unique<WindowsFileSystemWatcherBackend>();
}
