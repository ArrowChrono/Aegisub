#pragma once

#include <libaegisub/fs_fwd.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Automation4 {

class LuaLanguageServerProcess final {
	struct Impl;
	std::unique_ptr<Impl> impl;

	public:
	enum class ReadState { Data,
						   Timeout,
						   Closed,
						   Error };

	LuaLanguageServerProcess();
	~LuaLanguageServerProcess();
	LuaLanguageServerProcess(LuaLanguageServerProcess const&) = delete;
	LuaLanguageServerProcess& operator=(LuaLanguageServerProcess const&) = delete;

	bool Start(agi::fs::path const& executable, std::vector<std::string> const& arguments,
			   agi::fs::path const& working_directory, agi::fs::path const& stderr_log, std::string& error);
	bool Write(std::string_view bytes, std::chrono::milliseconds timeout, std::string& error);
	ReadState Read(std::string& bytes, std::chrono::milliseconds timeout, std::string& error);
	void Stop();
	int ProcessId() const;
};

}
