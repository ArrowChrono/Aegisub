#include "watched_file.h"

#include "threaded_ui_timer.h"
#include "ui_timer.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <cwctype>
#include <utility>

namespace {
constexpr int kWatchedFileDebounceMs = 150;

std::shared_ptr<UiTimerHost> ResolveUiTimerHost() {
	if (auto host = GetUiTimerHost())
		return host;
	return CreateThreadedUiTimerHost();
}

#ifdef _WIN32
std::wstring NormalizePathForCompare(agi::fs::path const& path) {
	if (path.empty())
		return {};

	auto normalized = path.lexically_normal().native();
	auto const root_len = path.root_path().native().size();
	std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
	while (normalized.size() > root_len && !normalized.empty() && normalized.back() == L'/')
		normalized.pop_back();
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(std::towlower(ch));
	});
	return normalized;
}
#else
std::string NormalizePathForCompare(agi::fs::path const& path) {
	if (path.empty())
		return {};

	auto normalized = agi::fs::PathToGenericString(path.lexically_normal());
	auto const root_len = agi::fs::PathToGenericString(path.root_path()).size();
	while (normalized.size() > root_len && !normalized.empty() && normalized.back() == '/')
		normalized.pop_back();
	return normalized;
}
#endif

bool PathsEqual(agi::fs::path const& left, agi::fs::path const& right) {
	return NormalizePathForCompare(left) == NormalizePathForCompare(right);
}

class NullFileSystemWatcherBackend final : public FileSystemWatcherBackend {
public:
	bool WatchDirectory(agi::fs::path const&, FileSystemWatcherListener*) override {
		return false;
	}

	void Reset() override {
	}
};
}

std::unique_ptr<FileSystemWatcherBackend> CreateNullFileSystemWatcherBackend() {
	return std::make_unique<NullFileSystemWatcherBackend>();
}

std::unique_ptr<FileSystemWatcherBackend> CreateDefaultFileSystemWatcherBackend() {
	return CreateNullFileSystemWatcherBackend();
}

WatchedFile::WatchedFile(std::unique_ptr<FileSystemWatcherBackend> backend)
: backend(std::move(backend))
, debounce_timer(ResolveUiTimerHost()->CreateTimer([this] {
	if (changed_callback && !target_path.empty())
		changed_callback(target_path);
})) {
}

WatchedFile::~WatchedFile() {
	ClearTargetPath();
}

void WatchedFile::NotifyError(std::string const& message) {
	if (error_callback)
		error_callback(message);
}

bool WatchedFile::SetTargetPath(agi::fs::path const& path) {
	if (path.empty()) {
		ClearTargetPath();
		return false;
	}

	auto directory = path.parent_path();
	if (directory.empty())
		directory = agi::fs::PathFromString(".");

	bool const same_target = PathsEqual(target_path, path);
	bool const same_directory = PathsEqual(watched_directory, directory);

	target_path = path;
	if (same_target && same_directory)
		return !watched_directory.empty();

	if (debounce_timer)
		debounce_timer->Stop();
	if (backend)
		backend->Reset();
	watched_directory.clear();

	if (!backend)
		return false;

	if (!backend->WatchDirectory(directory, this)) {
		NotifyError("Failed to watch directory for external subtitle changes: "
			+ agi::fs::PathToString(directory));
		return false;
	}

	watched_directory = directory;
	return true;
}

void WatchedFile::ClearTargetPath() {
	target_path.clear();
	watched_directory.clear();
	if (debounce_timer)
		debounce_timer->Stop();
	if (backend)
		backend->Reset();
}

bool WatchedFile::EventAffectsTarget(FileSystemWatchEvent const& event) const {
	if (event.kind == FileSystemWatchEventKind::Invalidated)
		return !target_path.empty() && PathsEqual(watched_directory, event.path);
	return !target_path.empty()
		&& (PathsEqual(target_path, event.path) || PathsEqual(target_path, event.new_path));
}

void WatchedFile::OnFileSystemWatchEvent(FileSystemWatchEvent const& event) {
	if (!EventAffectsTarget(event) || !debounce_timer)
		return;

	debounce_timer->StartOnce(kWatchedFileDebounceMs);
}

void WatchedFile::OnFileSystemWatchError(std::string const& message) {
	NotifyError(message);
}
