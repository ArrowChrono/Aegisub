/// @file watched_file_wx.cpp
/// @brief wxWidgets implementation of FileSystemWatcherBackend
/// @ingroup utility

#include "watched_file.h"

#ifdef _WIN32
std::unique_ptr<FileSystemWatcherBackend> CreateWinFileSystemWatcherBackend();
#else
#include "compat.h"

#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>

#include <wx/filename.h>
#include <wx/fswatcher.h>

namespace {

agi::fs::path PathFromWxFileName(wxFileName const& value) {
	auto full_path = value.GetFullPath();
	if (full_path.empty())
		full_path = value.GetPath();
	return agi::fs::PathFromString(from_wx(full_path));
}

#if wxUSE_FSWATCHER
class WxFileSystemWatcherBackend final : public FileSystemWatcherBackend {
	std::unique_ptr<wxFileSystemWatcher> watcher;
	FileSystemWatcherListener *listener = nullptr;

	void OnWatcherEvent(wxFileSystemWatcherEvent& event) {
		if (!listener)
			return;

		if (event.IsError()) {
			auto message = from_wx(event.GetErrorDescription());
			if (message.empty())
				message = "File system watcher reported an error.";
			listener->OnFileSystemWatchError(message);
			return;
		}

		auto const change_type = event.GetChangeType();
		if (change_type & wxFSW_EVENT_ACCESS)
			return;

		FileSystemWatchEvent mapped;
		mapped.path = PathFromWxFileName(event.GetPath());
		mapped.new_path = PathFromWxFileName(event.GetNewPath());

		if (change_type & wxFSW_EVENT_RENAME)
			mapped.kind = FileSystemWatchEventKind::Renamed;
		else if (change_type & wxFSW_EVENT_CREATE)
			mapped.kind = FileSystemWatchEventKind::Created;
		else if (change_type & wxFSW_EVENT_DELETE)
			mapped.kind = FileSystemWatchEventKind::Deleted;
		else if (change_type & wxFSW_EVENT_MODIFY)
			mapped.kind = FileSystemWatchEventKind::Modified;
		else
			return;

		listener->OnFileSystemWatchEvent(mapped);
	}

public:
	bool WatchDirectory(agi::fs::path const& directory, FileSystemWatcherListener* new_listener) override {
		Reset();
		listener = new_listener;

		watcher = std::make_unique<wxFileSystemWatcher>();
		watcher->Bind(wxEVT_FSWATCHER, &WxFileSystemWatcherBackend::OnWatcherEvent, this);
		if (!watcher->Add(wxFileName::DirName(to_wx(agi::fs::PathToString(directory))))) {
			Reset();
			return false;
		}
		return true;
	}

	void Reset() override {
		if (watcher) {
			watcher->Unbind(wxEVT_FSWATCHER, &WxFileSystemWatcherBackend::OnWatcherEvent, this);
			watcher.reset();
		}
		listener = nullptr;
	}
};
#endif

} // namespace
#endif

std::unique_ptr<FileSystemWatcherBackend> CreateWxFileSystemWatcherBackend() {
#ifdef _WIN32
	return CreateWinFileSystemWatcherBackend();
#elif wxUSE_FSWATCHER
	return agi::make_unique<WxFileSystemWatcherBackend>();
#else
	return CreateNullFileSystemWatcherBackend();
#endif
}
