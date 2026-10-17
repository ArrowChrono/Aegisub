#pragma once

#include "watched_file.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class MainThreadTimer;

class FontUsageInventoryWatcher final : private FileSystemWatcherListener {
	std::vector<agi::fs::path> sources;
	std::vector<agi::fs::path> discovered_directories;
	std::map<std::string, std::unique_ptr<FileSystemWatcherBackend>> subscriptions;
	std::unique_ptr<MainThreadTimer> debounce;
	std::unique_ptr<MainThreadTimer> deadline;
	std::function<void()> changed;
	std::function<void(std::string const&)> error;
	bool recursive = false;
	bool pending = false;
	bool available = true;
	bool backend_failed = false;
	bool has_directory_plan = false;
	std::set<std::string> rebind;

	bool Reconcile();
	void Flush();
	bool AffectsSources(agi::fs::path const& path) const;
	void OnFileSystemWatchEvent(FileSystemWatchEvent const& event) override;
	void OnFileSystemWatchError(std::string const& message) override;

	public:
	FontUsageInventoryWatcher(std::function<void()> changed, std::function<void(std::string const&)> error);
	~FontUsageInventoryWatcher();
	void SetSources(std::vector<agi::fs::path> const& inputs, bool recursive);
	bool UpdateDirectories(std::vector<agi::fs::path> const& directories);
	void Stop();
	bool IsAvailable() const { return available; }
};
