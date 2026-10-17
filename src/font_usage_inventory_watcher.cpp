#include "font_usage_inventory_watcher.h"

#include "main_thread_timer.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <utility>

namespace {
agi::fs::path Absolute(agi::fs::path const& path) {
	if (path.is_absolute()) return path.lexically_normal();
	std::error_code error;
	auto result = std::filesystem::absolute(path, error);
	return (error ? path : result).lexically_normal();
}

std::string Key(agi::fs::path const& path) {
	auto normalized = Absolute(path);
#ifdef _WIN32
	auto wide = normalized.native();
	std::ranges::transform(wide, wide.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
	normalized = wide;
#endif
	auto key = agi::fs::PathToGenericString(normalized);
	while (key.size() > agi::fs::PathToGenericString(normalized.root_path()).size() && key.back() == '/') key.pop_back();
	return key;
}

bool SameOrBelow(agi::fs::path const& path, agi::fs::path const& root) {
	auto child = Key(path);
	auto parent = Key(root);
	return child == parent || (child.size() > parent.size() && child.starts_with(parent) &&
							   (parent.back() == '/' || child[parent.size()] == '/'));
}

}

FontUsageInventoryWatcher::FontUsageInventoryWatcher(std::function<void()> changed,
													 std::function<void(std::string const&)> error)
	: debounce(std::make_unique<MainThreadTimer>([this] { Flush(); })), deadline(std::make_unique<MainThreadTimer>([this] { Flush(); })), changed(std::move(changed)), error(std::move(error)) {
}

FontUsageInventoryWatcher::~FontUsageInventoryWatcher() { Stop(); }

void FontUsageInventoryWatcher::SetSources(std::vector<agi::fs::path> const& inputs, bool recurse) {
	Stop();
	recursive = recurse;
	for (auto const& path : inputs) sources.push_back(Absolute(path));
	Reconcile();
}

bool FontUsageInventoryWatcher::UpdateDirectories(std::vector<agi::fs::path> const& directories) {
	discovered_directories = directories;
	has_directory_plan = true;
	return Reconcile();
}

bool FontUsageInventoryWatcher::Reconcile() {
	std::map<std::string, agi::fs::path> desired;
	auto add = [&](agi::fs::path const& path) {
		if (!path.empty()) desired.try_emplace(Key(path), Absolute(path));
	};
	if (has_directory_plan)
		for (auto const& directory : discovered_directories) add(directory);
	else
		for (auto const& source : sources) add(source.parent_path());
	bool modified = false;
	if (backend_failed) {
		for (auto const& [key, backend] : subscriptions) rebind.insert(key);
		backend_failed = false;
	}
	for (auto entry = subscriptions.begin(); entry != subscriptions.end();) {
		if (!desired.contains(entry->first) || rebind.contains(entry->first)) {
			entry->second->Reset();
			entry = subscriptions.erase(entry);
			modified = true;
		}
		else
			++entry;
	}
	rebind.clear();
	available = true;
	for (auto const& [key, path] : desired) {
		if (subscriptions.contains(key)) continue;
		auto backend = CreateWxFileSystemWatcherBackend();
		if (backend && backend->WatchDirectory(path, this)) {
			subscriptions.emplace(key, std::move(backend));
			modified = true;
		}
		else {
			available = false;
			if (error) error("Failed to watch directory: " + agi::fs::PathToString(path));
		}
	}
	return modified;
}

void FontUsageInventoryWatcher::Stop() {
	debounce->Stop();
	deadline->Stop();
	pending = false;
	for (auto& [key, backend] : subscriptions) backend->Reset();
	subscriptions.clear();
	discovered_directories.clear();
	sources.clear();
	rebind.clear();
	has_directory_plan = false;
	available = true;
	backend_failed = false;
}

bool FontUsageInventoryWatcher::AffectsSources(agi::fs::path const& path) const {
	if (path.empty()) return false;
	for (auto const& source : sources) {
		if (Key(path) == Key(source) || SameOrBelow(source, path)) return true;
		if (recursive ? SameOrBelow(path, source) : Key(path.parent_path()) == Key(source)) return true;
	}
	return false;
}

void FontUsageInventoryWatcher::OnFileSystemWatchEvent(FileSystemWatchEvent const& event) {
	if (!AffectsSources(event.path) && !AffectsSources(event.new_path)) return;
	if (event.kind != FileSystemWatchEventKind::Modified) {
		if (!event.path.empty() && subscriptions.contains(Key(event.path))) rebind.insert(Key(event.path));
		if (!event.new_path.empty() && subscriptions.contains(Key(event.new_path))) rebind.insert(Key(event.new_path));
	}
	if (!pending) {
		pending = true;
		deadline->StartOnce(1000);
	}
	debounce->StartOnce(150);
}

void FontUsageInventoryWatcher::Flush() {
	if (!pending) return;
	pending = false;
	debounce->Stop();
	deadline->Stop();
	if (changed) changed();
}

void FontUsageInventoryWatcher::OnFileSystemWatchError(std::string const& message) {
	available = false;
	backend_failed = true;
	if (error) error(message);
}
