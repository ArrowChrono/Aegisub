#include <main.h>

#include "../../src/watched_file.h"
#include "../../src/ui_timer.h"

#include <libaegisub/fs.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordingUiTimer final : public UiTimer {
	std::function<void()> callback;

public:
	int start_once_delay_ms = -1;
	int stop_count = 0;
	bool running = false;

	explicit RecordingUiTimer(std::function<void()> callback)
	: callback(std::move(callback)) {
	}

	void StartOnce(int delay_ms) override {
		start_once_delay_ms = delay_ms;
		running = true;
	}

	void StartRepeating(int) override {
	}

	void Stop() override {
		++stop_count;
		running = false;
	}

	bool IsRunning() const override {
		return running;
	}

	void Fire() {
		running = false;
		if (callback)
			callback();
	}
};

class RecordingUiTimerHost final : public UiTimerHost {
public:
	std::vector<RecordingUiTimer*> created_timers;

	std::unique_ptr<UiTimer> CreateTimer(std::function<void()> callback) override {
		auto timer = std::make_unique<RecordingUiTimer>(std::move(callback));
		created_timers.push_back(timer.get());
		return timer;
	}
};

class ScopedInstalledUiTimerHost {
	std::shared_ptr<UiTimerHost> previous;

public:
	explicit ScopedInstalledUiTimerHost(std::shared_ptr<UiTimerHost> host)
	: previous(GetUiTimerHost()) {
		InstallUiTimerHost(std::move(host));
	}

	~ScopedInstalledUiTimerHost() {
		InstallUiTimerHost(std::move(previous));
	}
};

class FakeFileSystemWatcherBackend final : public FileSystemWatcherBackend {
public:
	agi::fs::path watched_directory;
	FileSystemWatcherListener *listener = nullptr;
	int watch_count = 0;
	int reset_count = 0;
	bool watch_result = true;

	bool WatchDirectory(agi::fs::path const& directory, FileSystemWatcherListener* new_listener) override {
		++watch_count;
		watched_directory = directory;
		listener = new_listener;
		return watch_result;
	}

	void Reset() override {
		++reset_count;
		watched_directory.clear();
		listener = nullptr;
	}

	void Emit(FileSystemWatchEvent const& event) {
		if (listener)
			listener->OnFileSystemWatchEvent(event);
	}
};

TEST(watched_file, watches_parent_directory_of_target_path) {
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);

	auto backend = std::make_unique<FakeFileSystemWatcherBackend>();
	auto *backend_ptr = backend.get();
	WatchedFile watched_file(std::move(backend));

	auto const target_path = agi::fs::PathFromString("project/subs/external.ass");
	EXPECT_TRUE(watched_file.SetTargetPath(target_path));
	EXPECT_EQ("project/subs", agi::fs::PathToGenericString(backend_ptr->watched_directory));
	EXPECT_EQ("project/subs", agi::fs::PathToGenericString(watched_file.GetWatchedDirectory()));
}

TEST(watched_file, only_relevant_events_schedule_debounced_callback) {
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);

	auto backend = std::make_unique<FakeFileSystemWatcherBackend>();
	auto *backend_ptr = backend.get();
	WatchedFile watched_file(std::move(backend));

	int callback_count = 0;
	agi::fs::path last_path;
	watched_file.SetChangedCallback([&](agi::fs::path const& path) {
		++callback_count;
		last_path = path;
	});

	auto const target_path = agi::fs::PathFromString("project/subs/external.ass");
	auto const other_path = agi::fs::PathFromString("project/subs/other.ass");
	ASSERT_TRUE(watched_file.SetTargetPath(target_path));
	ASSERT_EQ(1u, timer_host->created_timers.size());
	auto *timer = timer_host->created_timers.front();

	backend_ptr->Emit({ FileSystemWatchEventKind::Modified, other_path, other_path });
	EXPECT_FALSE(timer->running);
	EXPECT_EQ(0, callback_count);

	backend_ptr->Emit({ FileSystemWatchEventKind::Modified, target_path, target_path });
	EXPECT_TRUE(timer->running);
	EXPECT_GT(timer->start_once_delay_ms, 0);

	timer->Fire();
	EXPECT_EQ(1, callback_count);
	EXPECT_EQ(target_path, last_path);
}

TEST(watched_file, rename_events_match_both_old_and_new_target_paths) {
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);

	auto backend = std::make_unique<FakeFileSystemWatcherBackend>();
	auto *backend_ptr = backend.get();
	WatchedFile watched_file(std::move(backend));

	int callback_count = 0;
	watched_file.SetChangedCallback([&](agi::fs::path const&) {
		++callback_count;
	});

	auto const target_path = agi::fs::PathFromString("project/subs/external.ass");
	auto const temp_path = agi::fs::PathFromString("project/subs/external.ass.tmp");
	ASSERT_TRUE(watched_file.SetTargetPath(target_path));
	auto *timer = timer_host->created_timers.front();

	backend_ptr->Emit({ FileSystemWatchEventKind::Renamed, temp_path, target_path });
	ASSERT_TRUE(timer->running);
	timer->Fire();
	EXPECT_EQ(1, callback_count);

	backend_ptr->Emit({ FileSystemWatchEventKind::Renamed, target_path, temp_path });
	ASSERT_TRUE(timer->running);
	timer->Fire();
	EXPECT_EQ(2, callback_count);
}

TEST(watched_file, directory_invalidation_rescans_target_and_ignores_other_directories) {
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);
	auto backend = std::make_unique<FakeFileSystemWatcherBackend>();
	auto *backend_ptr = backend.get();
	WatchedFile watched_file(std::move(backend));
	auto const target = agi::fs::PathFromString("project/subs/external.ass");
	auto const directory = target.parent_path();
	ASSERT_TRUE(watched_file.SetTargetPath(target));
	auto *timer = timer_host->created_timers.front();
	std::vector<agi::fs::path> changed;
	watched_file.SetChangedCallback([&](agi::fs::path const& path) { changed.push_back(path); });

	backend_ptr->Emit({.kind = FileSystemWatchEventKind::Invalidated, .path = agi::fs::PathFromString("project/other"), .new_path = {}});
	EXPECT_FALSE(timer->running);
	backend_ptr->Emit({.kind = FileSystemWatchEventKind::Modified, .path = directory, .new_path = {}});
	EXPECT_FALSE(timer->running);
	EXPECT_TRUE(changed.empty());

	backend_ptr->Emit({.kind = FileSystemWatchEventKind::Invalidated, .path = directory, .new_path = {}});
	ASSERT_TRUE(timer->running);
	EXPECT_GT(timer->start_once_delay_ms, 0);
	EXPECT_TRUE(changed.empty());
	backend_ptr->Emit({.kind = FileSystemWatchEventKind::Invalidated, .path = directory, .new_path = {}});
	timer->Fire();
	ASSERT_EQ(1u, changed.size());
	EXPECT_EQ(target, changed.front());
}

TEST(watched_file, clearing_target_stops_timer_and_resets_backend) {
	auto timer_host = std::make_shared<RecordingUiTimerHost>();
	ScopedInstalledUiTimerHost timer_scope(timer_host);

	auto backend = std::make_unique<FakeFileSystemWatcherBackend>();
	auto *backend_ptr = backend.get();
	WatchedFile watched_file(std::move(backend));

	auto const target_path = agi::fs::PathFromString("project/subs/external.ass");
	ASSERT_TRUE(watched_file.SetTargetPath(target_path));
	auto *timer = timer_host->created_timers.front();

	backend_ptr->Emit({ FileSystemWatchEventKind::Modified, target_path, target_path });
	ASSERT_TRUE(timer->running);

	watched_file.ClearTargetPath();
	EXPECT_TRUE(watched_file.GetTargetPath().empty());
	EXPECT_TRUE(watched_file.GetWatchedDirectory().empty());
	EXPECT_FALSE(timer->running);
	EXPECT_GE(timer->stop_count, 1);
	EXPECT_GE(backend_ptr->reset_count, 1);
}
}
